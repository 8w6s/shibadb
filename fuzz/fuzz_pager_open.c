
#include "shibadb.h"
#include "pager.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SDB_FUZZ_PAGER_MAX_INPUT ((size_t)(1U << 18))

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

    (void)unlink(g_db_path);
    if (size == 0U) {

    } else {
        f = fopen(g_db_path, "wb");
        if (f == NULL) {
            return 0;
        }
        (void)fwrite(data, 1U, size, f);
        (void)fclose(f);
    }

    if (sdb_pager_open(g_db_path, &pager) == SDB_OK) {

        (void)sdb_pager_payload_capacity(&pager);
        (void)sdb_pager_close(&pager);
    }
    (void)unlink(g_db_path);
    return 0;
}
