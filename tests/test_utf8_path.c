#include "file.h"
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char database_path[] =
    "test-utf8-"
    "\xE6\x9F\xB4\xE7\x8A\xAC"
    ".tmp";
static const char wal_path[] =
    "test-utf8-"
    "\xE6\x9F\xB4\xE7\x8A\xAC"
    ".tmp.wal";
static const char lock_path[] =
    "test-utf8-"
    "\xE6\x9F\xB4\xE7\x8A\xAC"
    ".tmp.lock";

static void cleanup(void)
{
    assert(sdb_file_remove(wal_path, true) == SDB_OK);
    assert(sdb_file_remove(database_path, true) == SDB_OK);
    assert(sdb_file_remove(lock_path, true) == SDB_OK);
}

int main(void)
{
    static const uint8_t namespace_name[] = "utf8-path";
    static const uint8_t key[] = "key";
    static const uint8_t value[] = "value";
    uint8_t actual[16];
    size_t actual_size = 0U;
    sdb_database_options options;
    sdb_database *database = NULL;

    cleanup();
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        database_path, &options, &database
    ) == SDB_OK);
    assert(sdb_kv_put(
        database, namespace_name, sizeof(namespace_name),
        key, sizeof(key), value, sizeof(value)
    ) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
    database = NULL;
    assert(sdb_database_open(
        database_path, &options, &database
    ) == SDB_OK);
    assert(sdb_kv_get(
        database, namespace_name, sizeof(namespace_name),
        key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(value));
    assert(memcmp(actual, value, sizeof(value)) == 0);
    assert(sdb_database_close(database) == SDB_OK);
    cleanup();
    (void)puts("UTF-8 path tests: ok");
    return 0;
}
