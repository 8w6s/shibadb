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
 * Reverse prefix scan: the sugar walks the prefix range in descending key
 * order, and a limit takes the LAST n matches rather than the first n
 * reversed. An empty prefix reverses the whole namespace.
 */
static void test_scan_prefix_reverse(void)
{
    static const char *path = "test-scan-prefix-rev.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_scan_options sopt;
    struct pairs got;
    size_t count = 0U;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    put(db, NS, 2U, "post:1", "hello");
    put(db, NS, 2U, "user:1", "alice");
    put(db, NS, 2U, "user:2", "bob");
    put(db, NS, 2U, "user:3", "carol");

    /* descending over the prefix range */
    got.n = 0U;
    sdb_scan_options_init(&sopt);
    sopt.reverse = true;
    assert(sdb_kv_scan_prefix(db, NS, 2U, (const uint8_t *)"user:", 5U,
        &sopt, scan_collector, &got, &count) == SDB_OK);
    assert(count == 3U && got.n == 3U);
    assert(strcmp(got.key[0], "user:3") == 0 && strcmp(got.val[0], "carol") == 0);
    assert(strcmp(got.key[1], "user:2") == 0);
    assert(strcmp(got.key[2], "user:1") == 0);

    /* reverse + limit yields the greatest n matches, not the first n flipped */
    got.n = 0U;
    count = 0U;
    sdb_scan_options_init(&sopt);
    sopt.reverse = true;
    sopt.limit = 2U;
    assert(sdb_kv_scan_prefix(db, NS, 2U, (const uint8_t *)"user:", 5U,
        &sopt, scan_collector, &got, &count) == SDB_OK);
    assert(count == 2U && got.n == 2U);
    assert(strcmp(got.key[0], "user:3") == 0);
    assert(strcmp(got.key[1], "user:2") == 0);

    /* forward + limit still takes the smallest n (the reverse flag is the only
     * difference) */
    got.n = 0U;
    count = 0U;
    sdb_scan_options_init(&sopt);
    sopt.limit = 2U;
    assert(sdb_kv_scan_prefix(db, NS, 2U, (const uint8_t *)"user:", 5U,
        &sopt, scan_collector, &got, &count) == SDB_OK);
    assert(count == 2U && got.n == 2U);
    assert(strcmp(got.key[0], "user:1") == 0);
    assert(strcmp(got.key[1], "user:2") == 0);

    /* empty prefix reverses the whole namespace */
    got.n = 0U;
    count = 0U;
    sdb_scan_options_init(&sopt);
    sopt.reverse = true;
    assert(sdb_kv_scan_prefix(db, NS, 2U, NULL, 0U,
        &sopt, scan_collector, &got, &count) == SDB_OK);
    assert(count == 4U && got.n == 4U);
    assert(strcmp(got.key[0], "user:3") == 0);
    assert(strcmp(got.key[3], "post:1") == 0);

    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * A reverse prefix scan needs the prefix's byte-successor as the range's upper
 * edge. An all-0xFF prefix has no successor (incrementing carries off the end),
 * so the walk must fall back to the namespace end instead of computing a bogus
 * bound and dropping every match.
 */
static void test_scan_prefix_reverse_max_prefix(void)
{
    static const char *path = "test-scan-prefix-rev-ff.tmp";
    static const uint8_t k_ff_01[2] = {0xFFU, 0x01U};
    static const uint8_t k_ff_02[2] = {0xFFU, 0x02U};
    static const uint8_t k_fe[1] = {0xFEU};
    static const uint8_t prefix_ff[1] = {0xFFU};
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_scan_options sopt;
    struct pairs got;
    size_t count = 0U;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    assert(sdb_kv_put(db, NS, 2U, k_fe, sizeof(k_fe),
        (const uint8_t *)"n", 1U) == SDB_OK);
    assert(sdb_kv_put(db, NS, 2U, k_ff_01, sizeof(k_ff_01),
        (const uint8_t *)"a", 1U) == SDB_OK);
    assert(sdb_kv_put(db, NS, 2U, k_ff_02, sizeof(k_ff_02),
        (const uint8_t *)"b", 1U) == SDB_OK);

    got.n = 0U;
    sdb_scan_options_init(&sopt);
    sopt.reverse = true;
    assert(sdb_kv_scan_prefix(db, NS, 2U, prefix_ff, sizeof(prefix_ff),
        &sopt, scan_collector, &got, &count) == SDB_OK);
    /* only the two 0xFF-prefixed keys, descending; the 0xFE key is excluded */
    assert(count == 2U && got.n == 2U);
    assert(memcmp(got.key[0], k_ff_02, sizeof(k_ff_02)) == 0);
    assert(memcmp(got.key[1], k_ff_01, sizeof(k_ff_01)) == 0);

    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * Differential property: for many random key sets and prefixes, a reverse scan
 * must yield exactly the forward scan's matches in the opposite order AND be
 * strictly descending, and reverse+limit must return the GREATEST `limit` of
 * them. Random multi-leaf trees are what caught the earlier right-sibling
 * desync, so the property is asserted over generated data rather than over one
 * hand-written fixture.
 */
static uint32_t reverse_rng_state = 0x12345678U;

static uint32_t reverse_rng(void)
{
    reverse_rng_state ^= reverse_rng_state << 13;
    reverse_rng_state ^= reverse_rng_state >> 17;
    reverse_rng_state ^= reverse_rng_state << 5;
    return reverse_rng_state;
}

/* Wider than struct pairs: these rounds hold up to a few hundred keys. */
struct scan_keys {
    char key[400][32];
    size_t n;
};

static bool scan_key_collector(void *context, const uint8_t *key,
    size_t key_size, const uint8_t *value, size_t value_size)
{
    struct scan_keys *out = (struct scan_keys *)context;
    (void)value;
    (void)value_size;
    assert(out->n < 400U);
    assert(key_size < 32U);
    (void)memcpy(out->key[out->n], key, key_size);
    out->key[out->n][key_size] = '\0';
    out->n += 1U;
    return true;
}

static void scan_keys_collect(sdb_database *db, const char *prefix,
    size_t prefix_size, bool reverse, uint64_t limit, struct scan_keys *out)
{
    sdb_scan_options options;
    out->n = 0U;
    sdb_scan_options_init(&options);
    options.reverse = reverse;
    options.limit = limit;
    assert(sdb_kv_scan_prefix(
        db, NS, 2U,
        prefix_size != 0U ? (const uint8_t *)prefix : NULL, prefix_size,
        &options, scan_key_collector, out, NULL
    ) == SDB_OK);
}

static void test_scan_prefix_reverse_mirrors_forward(void)
{
    static const char *path = "test-scan-prefix-rev-prop.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    struct scan_keys forward;
    struct scan_keys reverse;
    struct scan_keys limited;
    char prefix[3];
    size_t prefix_size;
    size_t keys;
    size_t i;
    int round;

    for (round = 0; round < 12; ++round) {
        remove_db(path);
        sdb_database_options_init(&opt);
        assert(sdb_database_create(path, &opt, &db) == SDB_OK);
        keys = 50U + (size_t)(reverse_rng() % 250U);
        for (i = 0; i < keys; ++i) {
            char k[24];
            (void)snprintf(k, sizeof(k), "%c%c%04u",
                (char)('a' + (reverse_rng() % 4U)),
                (char)('a' + (reverse_rng() % 4U)),
                (unsigned)(reverse_rng() % 9999U));
            put(db, NS, 2U, k, "x");
        }
        prefix_size = (size_t)(reverse_rng() % 3U);
        prefix[0] = (char)('a' + (reverse_rng() % 4U));
        prefix[1] = (char)('a' + (reverse_rng() % 4U));
        prefix[2] = '\0';

        scan_keys_collect(db, prefix, prefix_size, false, 0U, &forward);
        scan_keys_collect(db, prefix, prefix_size, true, 0U, &reverse);
        assert(reverse.n == forward.n);
        for (i = 0; i < forward.n; ++i) {
            assert(strcmp(forward.key[i], reverse.key[forward.n - 1U - i])
                == 0);
        }
        /* Strictly descending, so the order is real and not an artifact of
         * insertion order. */
        for (i = 1U; i < reverse.n; ++i) {
            assert(strcmp(reverse.key[i - 1U], reverse.key[i]) > 0);
        }
        if (forward.n > 2U) {
            uint64_t limit =
                (uint64_t)(1U + (reverse_rng() % (unsigned)(forward.n - 1U)));
            scan_keys_collect(db, prefix, prefix_size, true, limit, &limited);
            assert(limited.n == (size_t)limit);
            for (i = 0; i < limited.n; ++i) {
                assert(strcmp(limited.key[i], reverse.key[i]) == 0);
            }
        }
        assert(sdb_database_close(db) == SDB_OK);
    }
    remove_db(path);
}

/*
 * The prefix bound is checked before the reverse path copies it into a
 * successor buffer, so an oversized prefix is refused in BOTH directions and
 * the exact maximum is still accepted (off-by-one guard on the copy).
 */
static void test_scan_prefix_size_bound(void)
{
    static const char *path = "test-scan-prefix-bound.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_scan_options options;
    static uint8_t oversized[SDB_ENGINE_MAX_NAME_SIZE + 1];
    struct scan_keys got;
    size_t count = 123U;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    put(db, NS, 2U, "a", "1");
    (void)memset(oversized, 'x', sizeof(oversized));

    sdb_scan_options_init(&options);
    options.reverse = true;
    assert(sdb_kv_scan_prefix(db, NS, 2U, oversized, sizeof(oversized),
        &options, scan_key_collector, &got, &count) == SDB_E_INVALID_ARGUMENT);
    /* Rejected before the out-param is touched. */
    assert(count == 123U);

    sdb_scan_options_init(&options);
    assert(sdb_kv_scan_prefix(db, NS, 2U, oversized, sizeof(oversized),
        &options, scan_key_collector, &got, &count) == SDB_E_INVALID_ARGUMENT);
    assert(count == 123U);

    /* Exactly at the cap is legal (and simply matches nothing here). */
    got.n = 0U;
    sdb_scan_options_init(&options);
    options.reverse = true;
    assert(sdb_kv_scan_prefix(db, NS, 2U, oversized, SDB_ENGINE_MAX_NAME_SIZE,
        &options, scan_key_collector, &got, &count) == SDB_OK);
    assert(count == 0U && got.n == 0U);

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
    test_scan_prefix_reverse();
    test_scan_prefix_reverse_max_prefix();
    test_scan_prefix_reverse_mirrors_forward();
    test_scan_prefix_size_bound();
    test_snapshot_close_refused_while_cursor_open();
    test_cursor_limit_and_seek_reset();
    test_list_namespaces();
    test_cursor_reverse();
    test_cursor_reverse_multileaf();
    (void)puts("scan/cursor/enumerate: ok");
    return 0;
}
