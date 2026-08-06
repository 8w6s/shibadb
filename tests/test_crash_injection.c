/*
 * Crash-injection durability gate (POSIX only). A child process opens the
 * database and commits a stream of auto-commit puts; every time sdb_kv_put
 * returns SDB_OK it writes that sequence number to a pipe. The parent lets it
 * run for a random interval, then SIGKILLs it at an arbitrary point — very
 * often mid-commit (mid-pwrite, mid-fsync, mid-checkpoint). The parent then
 * drains the pipe to learn exactly which commits the child had ACKNOWLEDGED,
 * reopens the database (forcing WAL recovery), and asserts:
 *
 *   - every acknowledged commit is present and holds its exact value
 *     (durability: an SDB_OK commit is never lost across a crash),
 *   - no value is wrong (no torn/double-applied write),
 *   - sdb_database_verify reports a structurally clean database (crash left no
 *     corruption the recovery path failed to repair),
 *   - the acknowledged set only grows across rounds (no committed data
 *     vanishes on a later crash/reopen).
 *
 * The database file is REUSED across rounds, so each crash lands on a DB that
 * already survived previous crashes — recovery is exercised repeatedly on an
 * accumulating dataset.
 *
 * Scope: SIGKILL models an application crash. Data already fsync'd is on disk,
 * so this proves the recovery/WAL-replay path is correct and leaves no
 * corruption, and that acknowledged (fsync'd) commits survive. It does NOT by
 * itself simulate power-loss of not-yet-fsync'd writes (that needs a
 * fault-injecting block device such as dm-flakey); that remains a separate
 * gap.
 */
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
int main(void)
{
    (void)puts("crash injection: skipped (POSIX only)");
    return 0;
}
#else

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ACKED 200000U

static const char *db_path = "test-crash.tmp";
static const char *wal_path = "test-crash.tmp.wal";
static const char *lock_path = "test-crash.tmp.lock";
static const uint8_t namespace_name[] = "crash";
static const uint8_t crash_password[] = "crash-injection-password";

/* Shared with forked children via the address space at fork() time. */
static bool campaign_encrypted = false;

static void fill_options(sdb_database_options *options)
{
    sdb_database_options_init(options);
    if (campaign_encrypted) {
        options->kdf_iterations = SDB_MIN_KDF_ITERATIONS;
        options->password = crash_password;
        options->password_size = sizeof(crash_password) - 1U;
    }
}

static void encode_u32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8U);
    out[2] = (uint8_t)(v >> 16U);
    out[3] = (uint8_t)(v >> 24U);
}

/*
 * Value is a deterministic function of the key, so a wrong/torn/double-applied
 * write is detectable: the only correct value for seq is (seq*2654435761).
 */
static uint32_t value_of(uint32_t seq)
{
    return (uint32_t)(seq * UINT32_C(2654435761));
}

/*
 * Child: commit forever, reporting each acknowledged seq up the pipe. Never
 * returns normally — the parent kills it.
 */
static void child_commit_loop(int report_fd)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    uint32_t seq = 0U;
    fill_options(&options);
    if (sdb_database_open(db_path, &options, &database) != SDB_OK) {
        _exit(2);
    }
    for (;;) {
        uint8_t key[4];
        uint8_t value[4];
        encode_u32(key, seq);
        encode_u32(value, value_of(seq));
        if (sdb_kv_put(
                database, namespace_name, sizeof(namespace_name),
                key, sizeof(key), value, sizeof(value)
            ) == SDB_OK) {
            /*
             * Reported only AFTER the commit acked. If we are killed between
             * the ack and this write, the seq is simply not in the acked set —
             * conservative, never a false durability failure.
             */
            if (write(report_fd, &seq, sizeof(seq)) != (ssize_t)sizeof(seq)) {
                _exit(3);
            }
            ++seq;
        }
    }
}

/* Parent: drain every fully-received seq the child reported before dying. */
static uint32_t drain_acked(int report_fd, uint8_t *acked_bitmap, uint32_t *max_seq)
{
    uint32_t highest = 0U;
    bool any = false;
    uint32_t buffer[1024];
    ssize_t got;
    /* Non-blocking: the write end is closed (child dead), so read drains to EOF. */
    while ((got = read(report_fd, buffer, sizeof(buffer))) > 0) {
        size_t count = (size_t)got / sizeof(uint32_t);
        size_t i;
        for (i = 0U; i < count; ++i) {
            const uint32_t seq = buffer[i];
            if (seq < MAX_ACKED) {
                acked_bitmap[seq] = 1U;
                if (!any || seq > highest) {
                    highest = seq;
                    any = true;
                }
            }
        }
    }
    *max_seq = highest;
    return any ? 1U : 0U;
}

static void verify_acked(uint8_t *acked_bitmap, uint32_t max_seq)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_verify_result verify_result;
    uint32_t seq;
    size_t checked = 0U;
    fill_options(&options);
    /* Reopen forces WAL recovery over whatever the crash left on disk. */
    assert(sdb_database_open(db_path, &options, &database) == SDB_OK);
    /* Structural integrity: recovery must have left a clean database. */
    assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    for (seq = 0U; seq <= max_seq && seq < MAX_ACKED; ++seq) {
        uint8_t key[4];
        uint8_t expected[4];
        uint8_t actual[4];
        size_t actual_size = 0U;
        if (!acked_bitmap[seq]) {
            continue;
        }
        encode_u32(key, seq);
        encode_u32(expected, value_of(seq));
        /* An acknowledged commit MUST be durable and hold its exact value. */
        assert(sdb_kv_get(
            database, namespace_name, sizeof(namespace_name),
            key, sizeof(key), actual, sizeof(actual), &actual_size
        ) == SDB_OK);
        assert(actual_size == sizeof(expected));
        assert(memcmp(actual, expected, sizeof(expected)) == 0);
        ++checked;
    }
    assert(sdb_database_close(database) == SDB_OK);
    (void)checked;
}

/*
 * Run `rounds` crash rounds against a fresh database of the current mode.
 * Kill delay is base_us + a varied span so kills land across the whole commit
 * pipeline; for the encrypted mode base_us is large enough to clear the
 * PBKDF2 open so the child reaches its commit loop before being killed.
 * Returns the number of distinct acknowledged commits proven durable.
 */
static size_t run_campaign(
    unsigned rounds, unsigned base_us, unsigned span_us, unsigned seed
)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    uint8_t *acked_bitmap;
    unsigned round;
    size_t total_acked = 0U;
    uint32_t seq;

    (void)remove(db_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    fill_options(&options);
    assert(sdb_database_create(db_path, &options, &database) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);

    acked_bitmap = (uint8_t *)calloc(MAX_ACKED, 1U);
    assert(acked_bitmap != NULL);

    for (round = 0U; round < rounds; ++round) {
        int pipe_fds[2];
        pid_t pid;
        uint32_t max_seq = 0U;
        uint32_t had_any;
        unsigned delay_us;
        seed = seed * 1103515245U + 12345U;
        delay_us = base_us + (seed >> 11U) % span_us;

        assert(pipe(pipe_fds) == 0);
        pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            (void)close(pipe_fds[0]);
            child_commit_loop(pipe_fds[1]);
            _exit(0); /* unreachable */
        }
        (void)close(pipe_fds[1]);
        usleep(delay_us);
        assert(kill(pid, SIGKILL) == 0);
        {
            int status;
            assert(waitpid(pid, &status, 0) == pid);
        }
        (void)fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
        had_any = drain_acked(pipe_fds[0], acked_bitmap, &max_seq);
        (void)close(pipe_fds[0]);
        verify_acked(acked_bitmap, had_any ? max_seq : 0U);
    }

    /* Final tally: everything ever acknowledged is still present + correct. */
    fill_options(&options);
    assert(sdb_database_open(db_path, &options, &database) == SDB_OK);
    {
        sdb_verify_result verify_result;
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    }
    for (seq = 0U; seq < MAX_ACKED; ++seq) {
        uint8_t key[4];
        uint8_t expected[4];
        uint8_t actual[4];
        size_t actual_size = 0U;
        if (!acked_bitmap[seq]) {
            continue;
        }
        encode_u32(key, seq);
        encode_u32(expected, value_of(seq));
        assert(sdb_kv_get(
            database, namespace_name, sizeof(namespace_name),
            key, sizeof(key), actual, sizeof(actual), &actual_size
        ) == SDB_OK);
        assert(actual_size == sizeof(expected));
        assert(memcmp(actual, expected, sizeof(expected)) == 0);
        ++total_acked;
    }
    assert(sdb_database_close(database) == SDB_OK);

    free(acked_bitmap);
    (void)remove(db_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    return total_acked;
}

int main(void)
{
    size_t plaintext_acked;
    size_t encrypted_acked;

    /* Plaintext: fast commits, kills land densely across the commit pipeline. */
    campaign_encrypted = false;
    plaintext_acked = run_campaign(16U, 20000U, 500000U, 0x1234567U);

    /*
     * Encrypted (the product's default): PBKDF2 open measures ~1.1s, so the
     * base delay is set past it (1.3s) with a wide span, landing kills during
     * encrypted commits / WAL fsync / checkpoint rather than inside key
     * derivation. Fewer rounds because each open pays the full PBKDF2 cost.
     */
    campaign_encrypted = true;
    encrypted_acked = run_campaign(8U, 1300000U, 900000U, 0x89abcdeU);

    (void)printf(
        "crash injection: ok (plaintext %zu + encrypted %zu acked commits, "
        "all durable, no corruption across %u SIGKILLs)\n",
        plaintext_acked, encrypted_acked, 24U
    );
    return 0;
}

#endif
