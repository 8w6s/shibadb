#include "shibadb.h"
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t NS[] = "ns";
static const uint8_t NS2[] = "other";

static void remove_db(const char *path)
{
    char wal[512];
    (void)remove(path);
    (void)snprintf(wal, sizeof(wal), "%s.wal", path);
    (void)remove(wal);
}

static void put(sdb_database *db, const uint8_t *ns, size_t ns_size,
    const char *key, const char *value)
{
    assert(sdb_kv_put(
        db, ns, ns_size, (const uint8_t *)key, strlen(key),
        (const uint8_t *)value, strlen(value)
    ) == SDB_OK);
}

/* Collect (key,value) pairs a cursor yields, in order, into a flat buffer. */
struct pairs {
    char key[32][64];
    char val[32][64];
    size_t n;
};

static void collect_cursor(sdb_cursor *cur, struct pairs *out)
{
    out->n = 0U;
    assert(sdb_cursor_first(cur) == SDB_OK);
    while (sdb_cursor_valid(cur)) {
        uint8_t kb[64];
        uint8_t vb[64];
        size_t ks = 0U;
        size_t vs = 0U;
        assert(sdb_cursor_read(cur, kb, sizeof(kb), &ks, vb, sizeof(vb), &vs)
            == SDB_OK);
        assert(ks < 64U && vs < 64U);
        (void)memcpy(out->key[out->n], kb, ks);
        out->key[out->n][ks] = '\0';
        (void)memcpy(out->val[out->n], vb, vs);
        out->val[out->n][vs] = '\0';
        ++out->n;
        assert(sdb_cursor_next(cur) == SDB_OK);
    }
}

static bool scan_collector(void *context, const uint8_t *key, size_t key_size,
    const uint8_t *value, size_t value_size)
{
    struct pairs *p = (struct pairs *)context;
    (void)memcpy(p->key[p->n], key, key_size);
    p->key[p->n][key_size] = '\0';
    (void)memcpy(p->val[p->n], value, value_size);
    p->val[p->n][value_size] = '\0';
    ++p->n;
    return true;
}

/*
 * A cursor over a KV namespace yields exactly the user (key,value) pairs put
 * into that namespace, in ascending key order, isolated from other namespaces.
 */
static void test_cursor_kv_forward(void)
{
    static const char *path = "test-scan-cursor.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_snapshot *snap = NULL;
    sdb_cursor *cur = NULL;
    struct pairs got;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    /* insert out of order; cursor must return sorted */
    put(db, NS, 2U, "banana", "2");
    put(db, NS, 2U, "apple", "1");
    put(db, NS, 2U, "cherry", "3");
    put(db, NS2, 5U, "zebra", "99"); /* other namespace, must not appear */

    assert(sdb_snapshot_open(db, &snap) == SDB_OK);
    assert(sdb_cursor_open(snap, SDB_KEYSPACE_KV, NS, 2U, NULL, &cur) == SDB_OK);
    /* unpositioned: next before first is an error */
    assert(sdb_cursor_next(cur) == SDB_E_INVALID_ARGUMENT);

    collect_cursor(cur, &got);
    assert(got.n == 3U);
    assert(strcmp(got.key[0], "apple") == 0 && strcmp(got.val[0], "1") == 0);
    assert(strcmp(got.key[1], "banana") == 0 && strcmp(got.val[1], "2") == 0);
    assert(strcmp(got.key[2], "cherry") == 0 && strcmp(got.val[2], "3") == 0);

    /* seek: position at first key >= "banana" */
    assert(sdb_cursor_seek(cur, (const uint8_t *)"banana", 6U) == SDB_OK);
    assert(sdb_cursor_valid(cur));
    {
        const uint8_t *kp;
        size_t ks;
        assert(sdb_cursor_key(cur, &kp, &ks) == SDB_OK);
        assert(ks == 6U && memcmp(kp, "banana", 6U) == 0);
    }

    assert(sdb_cursor_close(cur) == SDB_OK);
    assert(sdb_snapshot_close(snap) == SDB_OK);
    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * A live snapshot blocks writers on the same handle with SDB_E_BUSY, and
 * releasing it restores writability.
 */
static void test_snapshot_blocks_writer(void)
{
    static const char *path = "test-scan-busy.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_snapshot *snap = NULL;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    put(db, NS, 2U, "k", "v");

    assert(sdb_snapshot_open(db, &snap) == SDB_OK);
    /* mutating op while snapshot is live must be refused */
    assert(sdb_kv_put(db, NS, 2U, (const uint8_t *)"k2", 2U,
        (const uint8_t *)"v2", 2U) == SDB_E_BUSY);
    /* a second snapshot is refused too */
    {
        sdb_snapshot *snap2 = NULL;
        assert(sdb_snapshot_open(db, &snap2) == SDB_E_BUSY);
        assert(snap2 == NULL);
    }
    assert(sdb_snapshot_close(snap) == SDB_OK);
    /* writable again */
    put(db, NS, 2U, "k2", "v2");

    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * Prefix scan sugar visits only keys beginning with the prefix; empty prefix
 * visits the whole namespace.
 */
static void test_scan_prefix(void)
{
    static const char *path = "test-scan-prefix.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    struct pairs got;
    size_t count = 0U;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    put(db, NS, 2U, "user:1", "alice");
    put(db, NS, 2U, "user:2", "bob");
    put(db, NS, 2U, "post:1", "hello");

    got.n = 0U;
    assert(sdb_kv_scan_prefix(db, NS, 2U, (const uint8_t *)"user:", 5U,
        NULL, scan_collector, &got, &count) == SDB_OK);
    assert(count == 2U && got.n == 2U);
    assert(strcmp(got.key[0], "user:1") == 0 && strcmp(got.val[0], "alice") == 0);
    assert(strcmp(got.key[1], "user:2") == 0 && strcmp(got.val[1], "bob") == 0);

    /* empty prefix = whole namespace (3 keys, sorted: post:1, user:1, user:2) */
    got.n = 0U;
    count = 0U;
    assert(sdb_kv_scan_prefix(db, NS, 2U, NULL, 0U,
        NULL, scan_collector, &got, &count) == SDB_OK);
    assert(count == 3U && got.n == 3U);
    assert(strcmp(got.key[0], "post:1") == 0);

    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * A cursor must not outlive its snapshot: closing the snapshot while a cursor
 * is still open is refused with SDB_E_BUSY (else a writer could mutate the tree
 * under the live cursor, or the handle could be torn down beneath it). Closing
 * the cursor first restores the ability to close the snapshot.
 */
static void test_snapshot_close_refused_while_cursor_open(void)
{
    static const char *path = "test-scan-order.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_snapshot *snap = NULL;
    sdb_cursor *cur = NULL;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    put(db, NS, 2U, "a", "1");

    assert(sdb_snapshot_open(db, &snap) == SDB_OK);
    assert(sdb_cursor_open(snap, SDB_KEYSPACE_KV, NS, 2U, NULL, &cur) == SDB_OK);
    /* snapshot still has an open cursor -> refuse to close it */
    assert(sdb_snapshot_close(snap) == SDB_E_BUSY);
    /* close the cursor, then the snapshot closes cleanly */
    assert(sdb_cursor_close(cur) == SDB_OK);
    assert(sdb_snapshot_close(snap) == SDB_OK);
    /* writable again after the snapshot is truly gone */
    put(db, NS, 2U, "b", "2");

    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * A limit stops the cursor after `limit` positioning steps, and a subsequent
 * seek re-arms the cursor (the limit counter resets like sdb_cursor_first).
 */
static void test_cursor_limit_and_seek_reset(void)
{
    static const char *path = "test-scan-limit.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_snapshot *snap = NULL;
    sdb_cursor *cur = NULL;
    sdb_cursor_options copt;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    put(db, NS, 2U, "a", "1");
    put(db, NS, 2U, "b", "2");
    put(db, NS, 2U, "c", "3");

    assert(sdb_snapshot_open(db, &snap) == SDB_OK);
    sdb_cursor_options_init(&copt);
    copt.limit = 1U;
    assert(sdb_cursor_open(snap, SDB_KEYSPACE_KV, NS, 2U, &copt, &cur) == SDB_OK);

    /* first entry is allowed; the next step trips the limit */
    assert(sdb_cursor_first(cur) == SDB_OK);
    assert(sdb_cursor_valid(cur));
    {
        const uint8_t *kp;
        size_t ks;
        assert(sdb_cursor_key(cur, &kp, &ks) == SDB_OK);
        assert(ks == 1U && kp[0] == 'a');
    }
    assert(sdb_cursor_next(cur) == SDB_OK);
    assert(!sdb_cursor_valid(cur)); /* limit reached */

    /* a seek restarts iteration: the target must be reachable again */
    assert(sdb_cursor_seek(cur, (const uint8_t *)"b", 1U) == SDB_OK);
    assert(sdb_cursor_valid(cur));
    {
        const uint8_t *kp;
        size_t ks;
        assert(sdb_cursor_key(cur, &kp, &ks) == SDB_OK);
        assert(ks == 1U && kp[0] == 'b');
    }

    assert(sdb_cursor_close(cur) == SDB_OK);
    assert(sdb_snapshot_close(snap) == SDB_OK);
    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

struct ns_entry {
    uint16_t kind;
    char name[64];
};

struct ns_list {
    struct ns_entry entries[32];
    size_t n;
};

static bool ns_collector(void *context, uint16_t kind,
    const uint8_t *name, size_t name_size)
{
    struct ns_list *out = (struct ns_list *)context;
    out->entries[out->n].kind = kind;
    (void)memcpy(out->entries[out->n].name, name, name_size);
    out->entries[out->n].name[name_size] = '\0';
    ++out->n;
    return true;
}

/*
 * list_namespaces reports each distinct (kind, namespace) that holds data
 * exactly once, in on-disk order, across KV/blob/document keyspaces.
 */
static void test_list_namespaces(void)
{
    static const char *path = "test-scan-ns.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    struct ns_list got;
    size_t count = 0U;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    /* two KV namespaces, several keys each (must dedup to one entry each) */
    put(db, (const uint8_t *)"aa", 2U, "k1", "1");
    put(db, (const uint8_t *)"aa", 2U, "k2", "2");
    put(db, (const uint8_t *)"aa", 2U, "k3", "3");
    put(db, (const uint8_t *)"bb", 2U, "k1", "1");
    /* a blob namespace (different kind, same-looking name space) */
    assert(sdb_blob_put(db, (const uint8_t *)"cc", 2U,
        (const uint8_t *)"b1", 2U, (const uint8_t *)"x", 1U) == SDB_OK);

    got.n = 0U;
    assert(sdb_list_namespaces(db, ns_collector, &got, &count) == SDB_OK);
    assert(count == 3U && got.n == 3U);
    /* KV (kind 1) sorts before blob (kind 2); within KV, "aa" before "bb" */
    assert(got.entries[0].kind == 1U && strcmp(got.entries[0].name, "aa") == 0);
    assert(got.entries[1].kind == 1U && strcmp(got.entries[1].name, "bb") == 0);
    assert(got.entries[2].kind == 2U && strcmp(got.entries[2].name, "cc") == 0);

    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * A reverse cursor yields the namespace's keys in descending order, and an
 * upper/lower bound clips the reverse range the same half-open way.
 */
static void test_cursor_reverse(void)
{
    static const char *path = "test-scan-rev.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_snapshot *snap = NULL;
    sdb_cursor *cur = NULL;
    sdb_cursor_options copt;
    struct pairs got;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    put(db, NS, 2U, "apple", "1");
    put(db, NS, 2U, "banana", "2");
    put(db, NS, 2U, "cherry", "3");
    put(db, NS2, 5U, "zzz", "9"); /* other namespace, must not appear */

    assert(sdb_snapshot_open(db, &snap) == SDB_OK);
    sdb_cursor_options_init(&copt);
    copt.reverse = true;
    assert(sdb_cursor_open(snap, SDB_KEYSPACE_KV, NS, 2U, &copt, &cur) == SDB_OK);
    collect_cursor(cur, &got);
    assert(got.n == 3U);
    assert(strcmp(got.key[0], "cherry") == 0 && strcmp(got.val[0], "3") == 0);
    assert(strcmp(got.key[1], "banana") == 0 && strcmp(got.val[1], "2") == 0);
    assert(strcmp(got.key[2], "apple") == 0 && strcmp(got.val[2], "1") == 0);
    assert(sdb_cursor_close(cur) == SDB_OK);

    /* reverse over the half-open range [banana, cherry): only banana */
    sdb_cursor_options_init(&copt);
    copt.reverse = true;
    copt.lower_bound = (const uint8_t *)"banana";
    copt.lower_bound_size = 6U;
    copt.upper_bound = (const uint8_t *)"cherry";
    copt.upper_bound_size = 6U;
    assert(sdb_cursor_open(snap, SDB_KEYSPACE_KV, NS, 2U, &copt, &cur) == SDB_OK);
    collect_cursor(cur, &got);
    assert(got.n == 1U);
    assert(strcmp(got.key[0], "banana") == 0);
    assert(sdb_cursor_close(cur) == SDB_OK);

    assert(sdb_snapshot_close(snap) == SDB_OK);
    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * Regression for the seek-to-end / right-sibling-hop desync: a reverse scan of
 * a namespace that spans MANY leaves and is FOLLOWED by another namespace must
 * still return every key. The bug silently dropped the namespace's last leaf
 * because the reverse start hopped to the next namespace's leaf without
 * updating the descent path. Needs enough keys to force multiple leaves.
 */
static void test_cursor_reverse_multileaf(void)
{
    static const char *path = "test-scan-rev-multi.tmp";
    enum { N = 400 };
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_snapshot *snap = NULL;
    sdb_cursor *cur = NULL;
    size_t index;
    size_t count = 0U;
    long previous = (long)N; /* keys are 0..N-1; start above the max */

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    for (index = 0U; index < (size_t)N; ++index) {
        char key[16];
        char value[16];
        (void)snprintf(key, sizeof(key), "k%05zu", index);
        (void)snprintf(value, sizeof(value), "%zu", index);
        put(db, (const uint8_t *)"aa", 2U, key, value);
    }
    /*
     * a namespace that sorts AFTER "aa", so "aa"'s last leaf has a right
     * sibling belonging to another namespace
     */
    put(db, (const uint8_t *)"zz", 2U, "x", "1");

    assert(sdb_snapshot_open(db, &snap) == SDB_OK);
    {
        sdb_cursor_options copt;
        sdb_cursor_options_init(&copt);
        copt.reverse = true;
        assert(sdb_cursor_open(snap, SDB_KEYSPACE_KV,
            (const uint8_t *)"aa", 2U, &copt, &cur) == SDB_OK);
    }
    assert(sdb_cursor_first(cur) == SDB_OK);
    while (sdb_cursor_valid(cur)) {
        const uint8_t *kp;
        size_t ks;
        long id;
        char id_text[8];
        assert(sdb_cursor_key(cur, &kp, &ks) == SDB_OK);
        assert(ks == 6U); /* "kNNNNN" */
        /* cursor keys are raw bytes, not NUL-terminated: copy before parsing */
        memcpy(id_text, kp + 1, ks - 1U);
        id_text[ks - 1U] = '\0';
        id = atol(id_text);
        assert(id == previous - 1); /* strictly descending, no gap */
        previous = id;
        ++count;
        assert(sdb_cursor_next(cur) == SDB_OK);
    }
    assert(count == (size_t)N); /* every key, including the last leaf */
    assert(previous == 0);

    assert(sdb_cursor_close(cur) == SDB_OK);
    assert(sdb_snapshot_close(snap) == SDB_OK);
    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

int main(void)
{
    test_cursor_kv_forward();
    test_snapshot_blocks_writer();
    test_scan_prefix();
    test_snapshot_close_refused_while_cursor_open();
    test_cursor_limit_and_seek_reset();
    test_list_namespaces();
    test_cursor_reverse();
    test_cursor_reverse_multileaf();
    (void)puts("scan/cursor/enumerate: ok");
    return 0;
}
