/*
 * quickstart.c — a tour of the shibadb C API the way an application uses it.
 *
 * Build/run standalone:
 *     cc quickstart.c $(pkg-config --cflags --libs shibadb) -o quickstart
 *     ./quickstart
 *
 * It is also built and run as the `example_quickstart` CTest, so the snippets
 * below stay correct. Everything here uses the public engine API only, favours
 * the ergonomic convenience layer, and exits non-zero on the first failure.
 */

#include <shibadb.h>
#include <shibadb_engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tiny helper: abort the demo if a call did not return SDB_OK. */
static void must(sdb_status status, const char *what)
{
    if (status != SDB_OK) {
        fprintf(stderr, "quickstart: %s failed: %s (%d)\n", what,
                sdb_status_string(status), (int)status);
        exit(1);
    }
}

static const uint8_t NS[] = "settings";
#define NS_SIZE (sizeof(NS) - 1U)

/* find() visitor: print "id => document" for each match. */
static bool on_match(void *context, const uint8_t *id, size_t id_size,
                     const uint8_t *doc, size_t doc_size)
{
    (void)context;
    printf("  %.*s => %.*s\n", (int)id_size, (const char *)id,
           (int)doc_size, (const char *)doc);
    return true; /* keep going */
}

int main(void)
{
    const char *path = "quickstart.shiba";
    sdb_database_options options;
    sdb_database *db = NULL;

    /* 1. Create a fresh database. Pass a non-empty password to
     *    options.password to get transparent authenticated encryption. */
    remove(path);
    sdb_database_options_init(&options);
    must(sdb_database_create(path, &options, &db), "create");

    /* 2. Key/value writes and ergonomic, allocating reads. */
    must(sdb_kv_put(db, NS, NS_SIZE, (const uint8_t *)"theme", 5,
                    (const uint8_t *)"dark", 4), "put");
    {
        uint8_t *value = NULL;
        size_t value_size = 0;
        must(sdb_kv_get_alloc(db, NS, NS_SIZE, (const uint8_t *)"theme", 5,
                              &value, &value_size), "get_alloc");
        printf("theme = %.*s\n", (int)value_size, (const char *)value);
        sdb_free(value);
    }

    /* 3. Atomic counters — no read-modify-write race in your code. */
    {
        int64_t visits = 0;
        must(sdb_kv_increment(db, NS, NS_SIZE, (const uint8_t *)"visits", 6,
                              1, &visits), "increment");
        must(sdb_kv_increment(db, NS, NS_SIZE, (const uint8_t *)"visits", 6,
                              9, &visits), "increment");
        printf("visits = %lld\n", (long long)visits);
    }

    /* 4. Compare-and-swap and put-if-absent for lock-free coordination. */
    must(sdb_kv_put_if_absent(db, NS, NS_SIZE, (const uint8_t *)"owner", 5,
                              (const uint8_t *)"alice", 5), "put_if_absent");
    if (sdb_kv_put_if_absent(db, NS, NS_SIZE, (const uint8_t *)"owner", 5,
                             (const uint8_t *)"bob", 3) == SDB_E_CONFLICT) {
        printf("owner already claimed\n");
    }

    /* 5. One atomic batch: all applied, or none. */
    {
        sdb_batch_op ops[2];
        memset(ops, 0, sizeof(ops));
        ops[0].kind = SDB_BATCH_OP_KV_PUT;
        ops[0].namespace_name = NS; ops[0].namespace_size = NS_SIZE;
        ops[0].key = (const uint8_t *)"lang"; ops[0].key_size = 4;
        ops[0].value = (const uint8_t *)"vi"; ops[0].value_size = 2;
        ops[1].kind = SDB_BATCH_OP_KV_PUT;
        ops[1].namespace_name = NS; ops[1].namespace_size = NS_SIZE;
        ops[1].key = (const uint8_t *)"tz"; ops[1].key_size = 2;
        ops[1].value = (const uint8_t *)"ICT"; ops[1].value_size = 3;
        must(sdb_kv_batch_apply(db, ops, 2), "batch_apply");
    }
    {
        uint64_t count = 0;
        must(sdb_kv_count(db, NS, NS_SIZE, &count), "count");
        printf("settings holds %llu keys\n", (unsigned long long)count);
    }

    /* 6. Documents with a secondary index, then find-by-field. */
    {
        const uint8_t coll[] = "users";
        const uint8_t idx[] = "by_role";
        sdb_index_term term;
        must(sdb_index_create(db, coll, sizeof(coll) - 1, idx,
                              sizeof(idx) - 1, false), "index_create");
        term.index_name = idx;
        term.index_name_size = sizeof(idx) - 1;
        term.value = (const uint8_t *)"admin";
        term.value_size = 5;
        must(sdb_document_put(db, coll, sizeof(coll) - 1,
                              (const uint8_t *)"u1", 2,
                              (const uint8_t *)"{\"name\":\"alice\"}", 16,
                              &term, 1), "document_put");
        must(sdb_document_put(db, coll, sizeof(coll) - 1,
                              (const uint8_t *)"u2", 2,
                              (const uint8_t *)"{\"name\":\"amir\"}", 15,
                              &term, 1), "document_put");
        printf("admins:\n");
        must(sdb_index_query_documents(db, coll, sizeof(coll) - 1, idx,
                                       sizeof(idx) - 1,
                                       (const uint8_t *)"admin", 5,
                                       on_match, NULL, NULL),
             "index_query_documents");
    }

    must(sdb_database_close(db), "close");
    remove(path);
    printf("quickstart: ok\n");
    return 0;
}
