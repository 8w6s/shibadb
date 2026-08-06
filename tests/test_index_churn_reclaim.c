/*
 * Index churn reclaim gate (documents). A document carries index rows the
 * plain kv/blob churn gate never exercises: a per-term index_entry row, a
 * unique_guard row for unique indexes, and a reverse (doc -> terms) bookkeeping
 * row. Before this fix a document DELETE reclaimed only metadata + chunks and
 * an OVERWRITE reclaimed nothing at all, so the index_entry / unique_guard rows
 * of the superseded version orphaned. A delete+reinsert or overwrite churn of
 * indexed documents therefore grew the dead-row count (and the file) without
 * bound, reclaimable only by compact().
 *
 * This gate churns indexed documents three ways and asserts, every round, that
 *   (1) find() by the CURRENT term value returns exactly the live document,
 *   (2) the unique constraint is still enforced,
 *   (3) find() by a SUPERSEDED value returns nothing (old index rows gone), and
 *   (4) the dead-row count stays bounded — it does not grow with the round
 *       count the way an unreclaimed index would.
 * Finally it compacts, reopens, and re-checks (1)-(4) so the reverse row's
 * verify/compact liveness integration is proven end to end.
 */
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DOCS 400U
#define ROUNDS 8U
/*
 * Perfect reclaim leaves zero dead rows after each committed round. A broken
 * reclaim leaks several rows PER DOCUMENT PER ROUND (chunk + two index_entry +
 * a guard), so it blows past this bound within the first round or two while a
 * working implementation stays near zero. The point of the bound is to reject
 * linear growth, not to measure an exact count.
 */
#define STALE_BOUND ((uint64_t)DOCS)

static const char *db_path = "test-index-churn-reclaim.tmp";
static const char *wal_path = "test-index-churn-reclaim.tmp.wal";
static const char *lock_path = "test-index-churn-reclaim.tmp.lock";
static const uint8_t collection[] = "users";
static const uint8_t email_index[] = "email";
static const uint8_t city_index[] = "city";
static const uint8_t password[] = "index-churn-reclaim-password";

/*
 * Four indexes for the large-term regression case (DEFECT 2): a document with
 * several ~1000B indexed values whose reverse rows, if packed into one value,
 * would exceed a single b-tree leaf payload.
 */
#define BIG_FIELDS 4U
#define BIG_VALUE_SIZE 1000U
static const uint8_t big_index[BIG_FIELDS][5] = {
    "big0", "big1", "big2", "big3"
};
/*
 * A doc_id space well clear of the churn docs (0..DOCS-1) and the 0xF00D
 * unique-conflict intruder used elsewhere.
 */
#define TERMLESS_ID UINT32_C(0x00A00001)
#define TERMLESS_HADTERMS_ID UINT32_C(0x00A00002)
#define BIG_DOC_ID UINT32_C(0x00A00003)

static void encode_u32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8U);
    out[2] = (uint8_t)(v >> 16U);
    out[3] = (uint8_t)(v >> 24U);
}

/*
 * Build the email term value for document i in overwrite-round r. r == UINT32_MAX
 * means the base value written by the delete/reinsert phase.
 */
static size_t make_email(uint8_t buffer[64], uint32_t i, uint32_t r)
{
    int written = r == UINT32_MAX
        ? snprintf((char *)buffer, 64U, "user%u@example.com", i)
        : snprintf((char *)buffer, 64U, "user%u-r%u@example.com", i, r);
    assert(written > 0 && written < 64);
    return (size_t)written;
}

static size_t make_city(uint8_t buffer[32], uint32_t i)
{
    int written = snprintf((char *)buffer, 32U, "city%u", i % 16U);
    assert(written > 0 && written < 32);
    return (size_t)written;
}

typedef struct find_context {
    const uint8_t *expect_id;
    size_t expect_id_size;
    size_t matches;
    size_t expected_hits;
} find_context;

static bool collect_visitor(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    find_context *ctx = (find_context *)context;
    ctx->matches += 1U;
    if (document_id_size == ctx->expect_id_size
        && memcmp(document_id, ctx->expect_id, document_id_size) == 0) {
        ctx->expected_hits += 1U;
    }
    return true;
}

/* Count matches for (index_name == value); report how many equal expect_id. */
static void find_index(
    sdb_database *database,
    const uint8_t *index_name, size_t index_name_size,
    const uint8_t *value, size_t value_size,
    const uint8_t *expect_id, size_t expect_id_size,
    size_t *matches_out, size_t *expected_hits_out
)
{
    find_context ctx;
    size_t visit_count = 0U;
    ctx.expect_id = expect_id;
    ctx.expect_id_size = expect_id_size;
    ctx.matches = 0U;
    ctx.expected_hits = 0U;
    assert(sdb_index_visit(
        database, collection, sizeof(collection) - 1U,
        index_name, index_name_size,
        value, value_size, collect_visitor, &ctx, &visit_count
    ) == SDB_OK);
    /* match_count_out and the callback invocation count must agree. */
    assert(visit_count == ctx.matches);
    *matches_out = ctx.matches;
    *expected_hits_out = ctx.expected_hits;
}

static void find_email(
    sdb_database *database,
    const uint8_t *email, size_t email_size,
    const uint8_t *expect_id, size_t expect_id_size,
    size_t *matches_out, size_t *expected_hits_out
)
{
    find_index(
        database, email_index, sizeof(email_index) - 1U,
        email, email_size, expect_id, expect_id_size,
        matches_out, expected_hits_out
    );
}

/* Put document i in overwrite-round r inside an open transaction. */
static sdb_status txn_put_doc(
    sdb_transaction *txn, uint32_t i, uint32_t r
)
{
    uint8_t doc_id[4];
    uint8_t email[64];
    uint8_t city[32];
    uint8_t body[8];
    sdb_index_term terms[2];
    size_t email_size = make_email(email, i, r);
    size_t city_size = make_city(city, i);
    encode_u32(doc_id, i);
    (void)memcpy(body, "docbody", 7U);
    terms[0].index_name = email_index;
    terms[0].index_name_size = sizeof(email_index) - 1U;
    terms[0].value = email;
    terms[0].value_size = email_size;
    terms[1].index_name = city_index;
    terms[1].index_name_size = sizeof(city_index) - 1U;
    terms[1].value = city;
    terms[1].value_size = city_size;
    return sdb_transaction_document_put(
        txn, collection, sizeof(collection) - 1U,
        doc_id, sizeof(doc_id), body, 7U, terms, 2U
    );
}

/*
 * Verify find()/unique correctness for the CURRENT overwrite-round r. Also
 * asserts a superseded email (round r-1, or the base value) is gone.
 */
static void assert_live_state(sdb_database *database, uint32_t r)
{
    uint32_t sample[5];
    size_t s;
    sample[0] = 0U;
    sample[1] = 1U;
    sample[2] = DOCS / 2U;
    sample[3] = DOCS - 2U;
    sample[4] = DOCS - 1U;
    for (s = 0U; s < 5U; ++s) {
        uint32_t i = sample[s];
        uint8_t doc_id[4];
        uint8_t email[64];
        size_t email_size;
        size_t matches;
        size_t hits;
        encode_u32(doc_id, i);
        /* Current email resolves to exactly this document. */
        email_size = make_email(email, i, r);
        find_email(
            database, email, email_size, doc_id, sizeof(doc_id),
            &matches, &hits
        );
        assert(matches == 1U && hits == 1U);
        /*
         * A superseded email (previous value) resolves to nothing: the old
         * index_entry row was reclaimed.
         */
        if (r != UINT32_MAX) {
            uint8_t old_email[64];
            size_t old_size = r == 0U
                ? make_email(old_email, i, UINT32_MAX)
                : make_email(old_email, i, r - 1U);
            find_email(
                database, old_email, old_size, doc_id, sizeof(doc_id),
                &matches, &hits
            );
            assert(matches == 0U && hits == 0U);
        }
    }
}

/*
 * The unique "email" index must reject a DIFFERENT document that claims an
 * email already held by a live document. Uses the current value of document 0
 * in overwrite-round r. Runs with no transaction open (auto-commit path).
 */
static void assert_unique_enforced(sdb_database *database, uint32_t r)
{
    uint8_t intruder_id[4];
    uint8_t email[64];
    uint8_t city[32];
    uint8_t body[8];
    sdb_index_term terms[2];
    size_t email_size = make_email(email, 0U, r);
    size_t city_size = make_city(city, 0U);
    encode_u32(intruder_id, 0xF00DU);
    (void)memcpy(body, "docbody", 7U);
    terms[0].index_name = email_index;
    terms[0].index_name_size = sizeof(email_index) - 1U;
    terms[0].value = email;
    terms[0].value_size = email_size;
    terms[1].index_name = city_index;
    terms[1].index_name_size = sizeof(city_index) - 1U;
    terms[1].value = city;
    terms[1].value_size = city_size;
    assert(sdb_document_put(
        database, collection, sizeof(collection) - 1U,
        intruder_id, sizeof(intruder_id), body, 7U, terms, 2U
    ) == SDB_E_CONFLICT);
}

static void insert_all(sdb_database *database, uint32_t r)
{
    sdb_transaction *txn = NULL;
    uint32_t i;
    assert(sdb_transaction_begin(database, &txn) == SDB_OK);
    for (i = 0U; i < DOCS; ++i) {
        assert(txn_put_doc(txn, i, r) == SDB_OK);
    }
    assert(sdb_transaction_commit(txn) == SDB_OK);
    assert(sdb_transaction_close(txn) == SDB_OK);
}

static void delete_all(sdb_database *database)
{
    sdb_transaction *txn = NULL;
    uint32_t i;
    assert(sdb_transaction_begin(database, &txn) == SDB_OK);
    for (i = 0U; i < DOCS; ++i) {
        uint8_t doc_id[4];
        encode_u32(doc_id, i);
        assert(sdb_transaction_document_delete(
            txn, collection, sizeof(collection) - 1U, doc_id, sizeof(doc_id)
        ) == SDB_OK);
    }
    assert(sdb_transaction_commit(txn) == SDB_OK);
    assert(sdb_transaction_close(txn) == SDB_OK);
}

/*
 * DEFECT 1 gate: documents with no matching index terms (term_count == 0) must
 * commit and be readable — fresh, re-put, and after having HAD terms. Before
 * the fix the term_count==0 path unconditionally deleted a (nonexistent) reverse
 * row and poisoned the whole put.
 */
static void run_termless_case(sdb_database *database)
{
    uint8_t id[4];
    uint8_t body[16];
    uint8_t out[16];
    size_t out_size;
    uint8_t email[64];
    size_t email_size;
    size_t matches;
    size_t hits;

    /* (a) Fresh insert with zero terms. */
    encode_u32(id, TERMLESS_ID);
    (void)memcpy(body, "no-terms", 8U);
    assert(sdb_document_put(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id), body, 8U, NULL, 0U
    ) == SDB_OK);
    assert(sdb_document_get(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id), out, sizeof(out), &out_size
    ) == SDB_OK);
    assert(out_size == 8U && memcmp(out, "no-terms", 8U) == 0);

    /* (b) Re-put the same termless document (overwrite, still zero terms). */
    (void)memcpy(body, "no-terms2", 9U);
    assert(sdb_document_put(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id), body, 9U, NULL, 0U
    ) == SDB_OK);
    assert(sdb_document_get(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id), out, sizeof(out), &out_size
    ) == SDB_OK);
    assert(out_size == 9U && memcmp(out, "no-terms2", 9U) == 0);

    /*
     * (c) A document that HAD an indexed term, then re-put with zero terms: the
     * old index_entry + unique_guard + reverse rows must be reclaimed, so a
     * find() by the old email returns nothing and get() returns the new body.
     */
    encode_u32(id, TERMLESS_HADTERMS_ID);
    email_size = make_email(email, 0xABCU, 0U);
    {
        sdb_index_term term;
        term.index_name = email_index;
        term.index_name_size = sizeof(email_index) - 1U;
        term.value = email;
        term.value_size = email_size;
        (void)memcpy(body, "hadterms", 8U);
        assert(sdb_document_put(
            database, collection, sizeof(collection) - 1U,
            id, sizeof(id), body, 8U, &term, 1U
        ) == SDB_OK);
    }
    find_email(database, email, email_size, id, sizeof(id), &matches, &hits);
    assert(matches == 1U && hits == 1U);
    (void)memcpy(body, "dropped", 7U);
    assert(sdb_document_put(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id), body, 7U, NULL, 0U
    ) == SDB_OK);
    find_email(database, email, email_size, id, sizeof(id), &matches, &hits);
    assert(matches == 0U && hits == 0U);
    assert(sdb_document_get(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id), out, sizeof(out), &out_size
    ) == SDB_OK);
    assert(out_size == 7U && memcmp(out, "dropped", 7U) == 0);

    /* Clean up so the DOCS census used by later phases stays exact. */
    encode_u32(id, TERMLESS_ID);
    assert(sdb_document_delete(
        database, collection, sizeof(collection) - 1U, id, sizeof(id)
    ) == SDB_OK);
    encode_u32(id, TERMLESS_HADTERMS_ID);
    assert(sdb_document_delete(
        database, collection, sizeof(collection) - 1U, id, sizeof(id)
    ) == SDB_OK);
}

/*
 * Deterministic ~1000B value for (field, variant); the first 8 bytes encode
 * field+variant so every (field, variant) pair yields a distinct value.
 */
static void fill_big(uint8_t *buffer, uint32_t field, uint32_t variant)
{
    size_t k;
    for (k = 0U; k < BIG_VALUE_SIZE; ++k) {
        buffer[k] = (uint8_t)(65U + ((field + variant + (uint32_t)k) % 26U));
    }
    encode_u32(buffer, field);
    encode_u32(buffer + 4U, variant);
}

static sdb_status big_put_doc(sdb_database *database, uint32_t variant)
{
    uint8_t id[4];
    uint8_t body[8];
    uint8_t values[BIG_FIELDS][BIG_VALUE_SIZE];
    sdb_index_term terms[BIG_FIELDS];
    uint32_t f;
    encode_u32(id, BIG_DOC_ID);
    (void)memcpy(body, "bigbody", 7U);
    for (f = 0U; f < BIG_FIELDS; ++f) {
        fill_big(values[f], f, variant);
        terms[f].index_name = big_index[f];
        terms[f].index_name_size = sizeof(big_index[f]) - 1U;
        terms[f].value = values[f];
        terms[f].value_size = BIG_VALUE_SIZE;
    }
    return sdb_document_put(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id), body, 7U, terms, BIG_FIELDS
    );
}

static void big_find(
    sdb_database *database, uint32_t field, uint32_t variant,
    const uint8_t *id, size_t expected_matches
)
{
    uint8_t value[BIG_VALUE_SIZE];
    size_t matches;
    size_t hits;
    fill_big(value, field, variant);
    find_index(
        database, big_index[field], sizeof(big_index[field]) - 1U,
        value, BIG_VALUE_SIZE, id, 4U, &matches, &hits
    );
    assert(matches == expected_matches);
    assert(hits == expected_matches);
}

/*
 * DEFECT 2 gate: a document indexing several ~1000B terms. The per-term reverse
 * rows each fit a leaf; a single packed all-terms value (~4KB) would exceed one
 * b-tree payload and fail to insert/overwrite. Churn it both ways and confirm
 * find() stays exact and the dead-row count stays bounded.
 */
static void run_big_term_case(sdb_database *database)
{
    uint8_t id[4];
    sdb_verify_result verify_result;
    uint32_t round;
    uint32_t f;
    encode_u32(id, BIG_DOC_ID);

    assert(big_put_doc(database, 0U) == SDB_OK);
    for (f = 0U; f < BIG_FIELDS; ++f) {
        big_find(database, f, 0U, id, 1U);
    }

    /* Overwrite churn: every big value changes each round. */
    for (round = 1U; round <= ROUNDS; ++round) {
        assert(big_put_doc(database, round) == SDB_OK);
        for (f = 0U; f < BIG_FIELDS; ++f) {
            big_find(database, f, round, id, 1U);      /* new value -> doc */
            big_find(database, f, round - 1U, id, 0U); /* old value -> gone */
        }
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
        assert(verify_result.stale_entry_count <= STALE_BOUND);
    }

    /* Delete + reinsert churn. */
    for (round = 0U; round < ROUNDS; ++round) {
        assert(sdb_document_delete(
            database, collection, sizeof(collection) - 1U, id, sizeof(id)
        ) == SDB_OK);
        assert(big_put_doc(database, 100U + round) == SDB_OK);
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
        assert(verify_result.stale_entry_count <= STALE_BOUND);
    }
    for (f = 0U; f < BIG_FIELDS; ++f) {
        big_find(database, f, 100U + ROUNDS - 1U, id, 1U);
    }

    /* Clean up so the DOCS census holds for the compact phase. */
    assert(sdb_document_delete(
        database, collection, sizeof(collection) - 1U, id, sizeof(id)
    ) == SDB_OK);
}

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_compact_result compact_result;
    sdb_verify_result verify_result;
    unsigned round;

    (void)remove(db_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    sdb_database_options_init(&options);
    options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    options.password = password;
    options.password_size = sizeof(password) - 1U;
    assert(sdb_database_create(db_path, &options, &database) == SDB_OK);

    assert(sdb_index_create(
        database, collection, sizeof(collection) - 1U,
        email_index, sizeof(email_index) - 1U, true
    ) == SDB_OK);
    assert(sdb_index_create(
        database, collection, sizeof(collection) - 1U,
        city_index, sizeof(city_index) - 1U, false
    ) == SDB_OK);
    {
        uint32_t bf;
        for (bf = 0U; bf < BIG_FIELDS; ++bf) {
            assert(sdb_index_create(
                database, collection, sizeof(collection) - 1U,
                big_index[bf], sizeof(big_index[bf]) - 1U, false
            ) == SDB_OK);
        }
    }

    /* Phase 1: delete + reinsert churn (base email value, r == UINT32_MAX). */
    insert_all(database, UINT32_MAX);
    for (round = 0U; round < ROUNDS; ++round) {
        delete_all(database);
        insert_all(database, UINT32_MAX);
        assert_live_state(database, UINT32_MAX);
        assert_unique_enforced(database, UINT32_MAX);
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
        assert(verify_result.live_chunk_count == DOCS);
        assert(verify_result.stale_entry_count <= STALE_BOUND);
    }

    /*
     * Phase 2: overwrite churn with a CHANGED email each round. This forces
     * reclamation of the previous version's index_entry AND unique_guard rows
     * (the email value differs, so the old guard key differs).
     */
    for (round = 0U; round < ROUNDS; ++round) {
        insert_all(database, round);
        assert_live_state(database, round);
        assert_unique_enforced(database, round);
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
        assert(verify_result.live_chunk_count == DOCS);
        assert(verify_result.stale_entry_count <= STALE_BOUND);
    }

    /*
     * Phase 2b: overwrite each document with the SAME (unchanged) unique email
     * repeatedly. This is the guard-ordering hazard: the unique_guard key has
     * no generation, so the reclaim of the previous version must run BEFORE the
     * new index_write_term re-creates the guard, or it would delete the guard
     * it just wrote. If that ordering were wrong, find()/unique below would
     * break and the guard would leak.
     */
    for (round = 0U; round < ROUNDS; ++round) {
        insert_all(database, ROUNDS - 1U);
        assert_live_state(database, ROUNDS - 1U);
        assert_unique_enforced(database, ROUNDS - 1U);
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
        assert(verify_result.live_chunk_count == DOCS);
        assert(verify_result.stale_entry_count <= STALE_BOUND);
    }

    /*
     * Phase 2c (DEFECT 1): term-less document puts. Self-contained; cleans up
     * its docs so the DOCS census below stays exact.
     */
    run_termless_case(database);

    /*
     * Phase 2d (DEFECT 2): a document indexing four ~1000B terms, churned both
     * ways. Self-contained; cleans up its doc afterwards.
     */
    run_big_term_case(database);

    /*
     * Both cases delete every document they created, so the live set is exactly
     * the DOCS churn documents again.
     */
    assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    assert(verify_result.live_chunk_count == DOCS);
    assert(verify_result.stale_entry_count <= STALE_BOUND);

    /*
     * Phase 3: compact, reopen, and re-verify correctness. After compaction
     * every superseded row is physically gone, so the dead-row count must drop
     * to zero while find()/unique still hold.
     */
    assert(sdb_database_compact(database, &options, &compact_result) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
    database = NULL;

    assert(sdb_database_open(db_path, &options, &database) == SDB_OK);
    assert_live_state(database, ROUNDS - 1U);
    assert_unique_enforced(database, ROUNDS - 1U);
    assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    assert(verify_result.live_chunk_count == DOCS);
    assert(verify_result.stale_entry_count == 0U);

    /*
     * One more overwrite round after reopen proves the reverse rows survived
     * compaction and still drive reclamation.
     */
    insert_all(database, ROUNDS);
    assert_live_state(database, ROUNDS);
    assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    assert(verify_result.live_chunk_count == DOCS);
    assert(verify_result.stale_entry_count <= STALE_BOUND);

    /*
     * Duplicate-term regression (round-2 review): a document that indexes the
     * SAME (index_name,value) twice — e.g. a multi-value tag list ["x","x"] —
     * must stay updatable/deletable. Before the idempotent-NOT_FOUND fix, the
     * per-term reclaim double-deleted the shared index_entry and aborted with
     * NOT_FOUND, wedging the document permanently.
     */
    {
        const uint8_t dup_id[] = "dupdoc";
        const uint8_t dup_city[] = "cityDup";
        sdb_index_term dup_terms[2];
        dup_terms[0].index_name = city_index;
        dup_terms[0].index_name_size = sizeof(city_index) - 1U;
        dup_terms[0].value = dup_city;
        dup_terms[0].value_size = sizeof(dup_city) - 1U;
        dup_terms[1] = dup_terms[0];
        assert(sdb_document_put(
            database, collection, sizeof(collection) - 1U,
            dup_id, sizeof(dup_id) - 1U, (const uint8_t *)"{}", 2U,
            dup_terms, 2U
        ) == SDB_OK);
        /* overwrite must not wedge */
        assert(sdb_document_put(
            database, collection, sizeof(collection) - 1U,
            dup_id, sizeof(dup_id) - 1U, (const uint8_t *)"{}", 2U,
            dup_terms, 2U
        ) == SDB_OK);
        /* delete must not wedge */
        assert(sdb_document_delete(
            database, collection, sizeof(collection) - 1U,
            dup_id, sizeof(dup_id) - 1U
        ) == SDB_OK);
    }

    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(db_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)printf(
        "index churn reclaim: ok (%u docs, delete+overwrite churn, "
        "compact+reopen; index rows reclaimed, find/unique intact)\n",
        DOCS
    );
    return 0;
}
