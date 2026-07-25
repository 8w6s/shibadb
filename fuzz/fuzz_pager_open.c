/*
 * End-to-end open-path fuzz for sdb_pager_open.
 *
 * Unlike fuzz_superblock (which only exercises the decoder) or
 * fuzz_wal_recover (which only exercises the WAL parser), this
 * target drives the full open pipeline: superblock read across
 * both mirrors, split-brain rejection, canonical file-size
 * enforcement, WAL sweep, and the .replace marker recovery hook.
 * All of those layers consume attacker-controlled bytes at open
 * time if the DB file was tampered with while unmounted.
 *
 * Strategy: dump the fuzz input as the entire on-disk state of a
 * candidate DB file (superblock mirrors + tail), then try
 * sdb_pager_open. Any crash, ASan hit, or read past the end of
 * the file is a bug in the open pipeline.
 *
 * We deliberately do not fuzz the encrypted variant here; that
 * would multiply the harness by the KDF work factor. The
 * encrypted envelope has its own target (fuzz_encrypted_envelope).
 */

#include "shibadb.h"
#include "pager.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SDB_FUZZ_PAGER_MAX_INPUT ((size_t)(1U << 18)) /* 256 KiB */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static char g_db_path[80];
static int g_path_ready = 0;

static void ensure_path(void)
{
    if (!g_path_ready) {
        (void)snprintf(g_db_path, sizeof(g_db_path),
            "/tmp/shibadb-fuzz-pager-%d.db", (int)getpid());
        g_path_ready = 1;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    sdb_pager pager;
    FILE *f;

    if (size > SDB_FUZZ_PAGER_MAX_INPUT) {
        return 0;
    }
    ensure_path();

    /* Fresh DB file per iteration — deterministic state entering
     * pager_open. Best-effort cleanup on the way in as well. */
    (void)unlink(g_db_path);
    if (size == 0U) {
        /* pager_open on a non-existent path exercises a distinct
         * branch (create-or-fail); include it in the fuzz space. */
    } else {
        f = fopen(g_db_path, "wb");
        if (f == NULL) {
            return 0;
        }
        (void)fwrite(data, 1U, size, f);
        (void)fclose(f);
    }

    if (sdb_pager_open(g_db_path, &pager) == SDB_OK) {
        /* Successful open means the fuzz input parsed as a valid
         * DB. Exercise a minimal round-trip so any latent
         * corruption in the pager state surfaces. */
        (void)sdb_pager_payload_capacity(&pager);
        (void)sdb_pager_close(&pager);
    }
    (void)unlink(g_db_path);
    return 0;
}
