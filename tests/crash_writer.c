/*
 * crash_writer — a real (not fault-injected) crash-durability workload.
 *
 * Two modes, both over an optionally-encrypted database:
 *   crash_writer <db> [password]           write mode: increment a durable
 *       counter forever, printing each ACKED value (one per line, flushed) so
 *       an external driver can hard-kill the process (TerminateProcess) and
 *       know exactly which commits were acknowledged.
 *   crash_writer <db> [password] verify     verify mode: reopen (running WAL
 *       recovery), read the counter, run deep sdb_database_verify, and print
 *       "counter=<n> verify=<OK|err>".
 *
 * The durability contract the driver checks: after a hard kill, the reopened
 * counter must be >= the last value the writer printed (an acked commit is
 * fsync-durable before sdb_kv_increment returns), and verify must be OK
 * (no corruption from the interrupted commit).
 */

#include <shibadb.h>
#include <shibadb_engine.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t NS[] = "crash";
static const uint8_t KEY[] = "counter";
#define NS_SIZE (sizeof(NS) - 1U)
#define KEY_SIZE (sizeof(KEY) - 1U)

static sdb_status open_or_create(const char *path, const char *password,
                                 sdb_database **db_out)
{
    sdb_database_options options;
    sdb_status status;
    sdb_database_options_init(&options);
    if (password != NULL && password[0] != '\0') {
        options.password = (const uint8_t *)password;
        options.password_size = strlen(password);
    }
    status = sdb_database_open(path, &options, db_out);
    if (status == SDB_OK) {
        return SDB_OK;
    }
    return sdb_database_create(path, &options, db_out);
}

int main(int argc, char **argv)
{
    const char *path;
    const char *password;
    int verify_mode;
    sdb_database *db = NULL;
    sdb_status status;

    if (argc < 2) {
        fprintf(stderr, "usage: crash_writer <db> [password] [verify]\n");
        return 2;
    }
    path = argv[1];
    password = (argc >= 3 && argv[2][0] != '\0') ? argv[2] : NULL;
    verify_mode = (argc >= 4 && strcmp(argv[3], "verify") == 0);

    status = open_or_create(path, password, &db);
    if (status != SDB_OK) {
        fprintf(stderr, "open/create failed: %s\n", sdb_status_string(status));
        return 3;
    }

    if (verify_mode) {
        uint8_t buffer[8];
        size_t got = 0;
        int64_t current = 0;
        sdb_verify_result result;
        (void)memset(&result, 0, sizeof(result));
        status = sdb_kv_get(db, NS, NS_SIZE, KEY, KEY_SIZE,
                            buffer, sizeof(buffer), &got);
        if (status == SDB_OK && got == 8U) {
            uint64_t raw = 0U;
            int i;
            for (i = 0; i < 8; ++i) {
                raw |= (uint64_t)buffer[i] << (8 * i);
            }
            current = (int64_t)raw;
        } else if (status != SDB_E_NOT_FOUND && status != SDB_OK) {
            fprintf(stderr, "read failed: %s\n", sdb_status_string(status));
            (void)sdb_database_close(db);
            return 4;
        }
        status = sdb_database_verify(db, &result);
        printf("counter=%lld verify=%s\n", (long long)current,
               status == SDB_OK ? "OK" : sdb_status_string(status));
        fflush(stdout);
        (void)sdb_database_close(db);
        return status == SDB_OK ? 0 : 5;
    }

    /* Write mode: commit acked increments as fast as durability allows. */
    for (;;) {
        int64_t value = 0;
        status = sdb_kv_increment(db, NS, NS_SIZE, KEY, KEY_SIZE, 1, &value);
        if (status != SDB_OK) {
            fprintf(stderr, "increment failed: %s\n",
                    sdb_status_string(status));
            (void)sdb_database_close(db);
            return 6;
        }
        printf("%lld\n", (long long)value);
        fflush(stdout);
    }
}
