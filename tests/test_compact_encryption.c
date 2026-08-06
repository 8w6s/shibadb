/*
 * Encryption-at-rest must survive compaction and migration. ShibaDB's core
 * promise is encryption by default; a caller compacting an encrypted database
 * to reclaim space (or migrating it) must NOT silently end up with a plaintext
 * file on disk just because the target options carried no password — the
 * Python binding's compact()/migrate() default password=None makes that an
 * easy, catastrophic mistake.
 *
 * Guards:
 *   - compact/migrate of an ENCRYPTED source with a no-password target is
 *     refused (SDB_E_INVALID_ARGUMENT), and the on-disk database is left
 *     untouched and still encrypted (it cannot be opened without the password,
 *     and opens correctly WITH it, data intact).
 *   - compact/migrate that keeps a password succeeds and stays encrypted.
 *   - a genuinely plaintext source can still be compacted with no password
 *     (that path never had encryption to lose), and may even be encrypted via
 *     a password-bearing target (adding protection is fine).
 */
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t password[] = "compact-encryption-secret";
static const uint8_t namespace_name[] = "s";
static const uint8_t key[] = "k";
static const uint8_t secret_value[] = "TOP-SECRET-CANARY-0xDEADBEEF";

static const char *enc_path = "test-compact-enc.tmp";
static const char *enc_wal = "test-compact-enc.tmp.wal";
static const char *enc_lock = "test-compact-enc.tmp.lock";
static const char *plain_path = "test-compact-plain.tmp";
static const char *plain_wal = "test-compact-plain.tmp.wal";
static const char *plain_lock = "test-compact-plain.tmp.lock";

static void cleanup(void)
{
    (void)remove(enc_path);
    (void)remove(enc_wal);
    (void)remove(enc_lock);
    (void)remove(plain_path);
    (void)remove(plain_wal);
    (void)remove(plain_lock);
}

static void assert_secret_readable(sdb_database *database)
{
    uint8_t actual[64];
    size_t actual_size = 0U;
    assert(sdb_kv_get(
        database, namespace_name, sizeof(namespace_name),
        key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(secret_value));
    assert(memcmp(actual, secret_value, sizeof(secret_value)) == 0);
}

int main(void)
{
    sdb_database_options encrypted_options;
    sdb_database_options plaintext_options;
    sdb_database *database = NULL;
    sdb_compact_result compact_result;

    cleanup();

    sdb_database_options_init(&encrypted_options);
    encrypted_options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    encrypted_options.password = password;
    encrypted_options.password_size = sizeof(password) - 1U;

    sdb_database_options_init(&plaintext_options);
    plaintext_options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    /* no password -> a plaintext target */

    /* Encrypted source with sensitive data. */
    assert(sdb_database_create(enc_path, &encrypted_options, &database)
        == SDB_OK);
    assert(sdb_kv_put(
        database, namespace_name, sizeof(namespace_name),
        key, sizeof(key), secret_value, sizeof(secret_value)
    ) == SDB_OK);

    /*
     * (A) compact to a no-password target must be REFUSED, not silently strip
     * encryption. This is the assertion that fails before the fix.
     */
    assert(sdb_database_compact(database, &plaintext_options, &compact_result)
        == SDB_E_INVALID_ARGUMENT);
    /* (B) migrate shares the path and must refuse identically. */
    assert(sdb_database_migrate(
        database, SDB_FORMAT_VERSION_V1, &plaintext_options, &compact_result
    ) == SDB_E_INVALID_ARGUMENT);

    /* The refused ops left the live DB usable and unchanged. */
    assert_secret_readable(database);

    /* (C) compact keeping the password succeeds and stays encrypted. */
    assert(sdb_database_compact(database, &encrypted_options, &compact_result)
        == SDB_OK);
    assert_secret_readable(database);
    assert(sdb_database_close(database) == SDB_OK);
    database = NULL;

    /*
     * (D) On disk it is STILL encrypted: cannot open without the password,
     * opens with it, data intact. If any earlier step had stripped encryption
     * this open-without-password would wrongly succeed.
     */
    assert(sdb_database_open(enc_path, &plaintext_options, &database)
        != SDB_OK);
    assert(database == NULL);
    assert(sdb_database_open(enc_path, &encrypted_options, &database)
        == SDB_OK);
    assert_secret_readable(database);
    assert(sdb_database_close(database) == SDB_OK);
    database = NULL;

    /*
     * (E) A genuinely plaintext source can still be compacted with no
     * password — there was no encryption to lose.
     */
    assert(sdb_database_create(plain_path, &plaintext_options, &database)
        == SDB_OK);
    assert(sdb_kv_put(
        database, namespace_name, sizeof(namespace_name),
        key, sizeof(key), secret_value, sizeof(secret_value)
    ) == SDB_OK);
    assert(sdb_database_compact(database, &plaintext_options, &compact_result)
        == SDB_OK);
    assert_secret_readable(database);
    /* And it may be upgraded to encrypted via a password-bearing target. */
    assert(sdb_database_compact(database, &encrypted_options, &compact_result)
        == SDB_OK);
    assert_secret_readable(database);
    assert(sdb_database_close(database) == SDB_OK);
    database = NULL;

    cleanup();
    (void)puts("compact/migrate encryption preservation: ok");
    return 0;
}
