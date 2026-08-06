/*
 * Coverage for sdb_database_info(): the O(1) open-handle configuration probe.
 * Exercises the NULL-argument contract and the plaintext/encrypted snapshots
 * using only the public engine API.
 */

#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void cleanup(const char *path)
{
    char sidecar[256];
    remove(path);
    (void)snprintf(sidecar, sizeof(sidecar), "%s.lock", path);
    remove(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s.wal", path);
    remove(sidecar);
}

int main(void)
{
    const char *plain = "test_info_plain.shiba";
    const char *encrypted = "test_info_encrypted.shiba";
    sdb_database_options options;
    sdb_database *db = NULL;
    sdb_info_result info;

    cleanup(plain);
    cleanup(encrypted);

    /* Plaintext database. */
    sdb_database_options_init(&options);
    assert(sdb_database_create(plain, &options, &db) == SDB_OK);

    /* NULL-argument contract. */
    assert(sdb_database_info(NULL, &info) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_database_info(db, NULL) == SDB_E_INVALID_ARGUMENT);

    /* Prefill with 0xFF so we can see every field is written. */
    (void)memset(&info, 0xFF, sizeof(info));
    assert(sdb_database_info(db, &info) == SDB_OK);
    assert(info.struct_size == (uint32_t)sizeof(info));
    assert(info.page_size == SDB_ENGINE_DEFAULT_PAGE_SIZE);
    assert(!info.encrypted);
    assert(info.page_count >= 1U);
    assert(sdb_database_close(db) == SDB_OK);
    db = NULL;

    /* Encrypted database: the snapshot must report encryption + KDF cost. */
    sdb_database_options_init(&options);
    options.password = (const uint8_t *)"correct horse battery staple";
    options.password_size = 28U;
    options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    assert(sdb_database_create(encrypted, &options, &db) == SDB_OK);
    assert(sdb_database_info(db, &info) == SDB_OK);
    assert(info.encrypted);
    assert(info.kdf_iterations == SDB_MIN_KDF_ITERATIONS);
    assert(info.page_size == SDB_ENGINE_DEFAULT_PAGE_SIZE);
    assert(sdb_database_close(db) == SDB_OK);

    cleanup(plain);
    cleanup(encrypted);
    (void)puts("database_info tests: ok");
    return 0;
}
