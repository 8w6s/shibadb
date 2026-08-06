/*
 * Helper for the dm-flakey power-loss durability test (see
 * tests/powerloss_test.sh). Two modes:
 *
 *   write N PATH   open the encrypted DB at PATH, auto-commit records 0..N-1
 *                  (each auto-commit fsyncs the WAL before returning SDB_OK),
 *                  print "READY" and then BLOCK FOREVER without closing.
 *                  Not closing is deliberate: a clean close would checkpoint
 *                  and fsync everything, masking whether the PER-COMMIT fsync
 *                  actually persisted each record. The parent yanks power
 *                  (dm-flakey drop_writes) and SIGKILLs us here.
 *
 *   verify N PATH  reopen the DB (forcing WAL recovery over the post-power-loss
 *                  backing image) and assert every record 0..N-1 is present
 *                  with its exact value and that verify() is clean.
 *
 * The claim under test: an auto-commit that returned SDB_OK is durable across
 * a real power loss (writes that never reached the platter are gone), because
 * its fsync truly pushed the WAL frame to stable storage before acking.
 */
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t namespace_name[] = "pl";
static const uint8_t password[] = "powerloss-test-password";

static void encode_u32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8U);
    out[2] = (uint8_t)(v >> 16U);
    out[3] = (uint8_t)(v >> 24U);
}

static uint32_t value_of(uint32_t seq)
{
    return (uint32_t)(seq * UINT32_C(2654435761));
}

static void fill_options(sdb_database_options *options)
{
    sdb_database_options_init(options);
    options->kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    options->password = password;
    options->password_size = sizeof(password) - 1U;
}

int main(int argc, char **argv)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    const char *mode;
    uint32_t count;
    const char *path;
    uint32_t seq;

    if (argc != 4) {
        (void)fprintf(stderr, "usage: %s write|verify N PATH\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    count = (uint32_t)strtoul(argv[2], NULL, 10);
    path = argv[3];
    fill_options(&options);

    if (strcmp(mode, "write") == 0) {
        /*
         * Fresh DB created by the parent's mkfs+first run? No — create here so
         * the file lives on the flakey-backed filesystem.
         */
        if (sdb_database_create(path, &options, &database) != SDB_OK) {
            /* Maybe it already exists (re-run) — try open. */
            if (sdb_database_open(path, &options, &database) != SDB_OK) {
                return 3;
            }
        }
        for (seq = 0U; seq < count; ++seq) {
            uint8_t key[4];
            uint8_t value[4];
            encode_u32(key, seq);
            encode_u32(value, value_of(seq));
            if (sdb_kv_put(
                    database, namespace_name, sizeof(namespace_name),
                    key, sizeof(key), value, sizeof(value)
                ) != SDB_OK) {
                return 4;
            }
        }
        /*
         * Every one of `count` commits has returned SDB_OK, i.e. its WAL frame
         * was fsync'd. Announce and block WITHOUT closing.
         */
        (void)printf("READY %u\n", count);
        (void)fflush(stdout);
        for (;;) {
            (void)sleep(3600);
        }
        /* unreachable */
    }

    if (strcmp(mode, "verify") == 0) {
        sdb_verify_result verify_result;
        size_t present = 0U;
        if (sdb_database_open(path, &options, &database) != SDB_OK) {
            (void)fprintf(stderr, "verify: open failed\n");
            return 5;
        }
        if (sdb_database_verify(database, &verify_result) != SDB_OK) {
            (void)fprintf(stderr, "verify: structural verify FAILED\n");
            return 6;
        }
        for (seq = 0U; seq < count; ++seq) {
            uint8_t key[4];
            uint8_t expected[4];
            uint8_t actual[4];
            size_t actual_size = 0U;
            sdb_status status;
            encode_u32(key, seq);
            encode_u32(expected, value_of(seq));
            status = sdb_kv_get(
                database, namespace_name, sizeof(namespace_name),
                key, sizeof(key), actual, sizeof(actual), &actual_size
            );
            if (status != SDB_OK) {
                (void)fprintf(
                    stderr, "verify: LOST acked commit %u (status %d)\n",
                    (unsigned)seq, (int)status
                );
                return 7;
            }
            if (actual_size != sizeof(expected)
                || memcmp(actual, expected, sizeof(expected)) != 0) {
                (void)fprintf(
                    stderr, "verify: WRONG value for commit %u\n",
                    (unsigned)seq
                );
                return 8;
            }
            ++present;
        }
        (void)sdb_database_close(database);
        (void)printf("VERIFIED %zu/%u acked commits durable\n", present, count);
        return 0;
    }

    (void)fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
