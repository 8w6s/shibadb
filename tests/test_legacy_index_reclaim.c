#include "engine_internal.h"
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *db_path = "test-legacy-index-reclaim.tmp";
static const char *wal_path = "test-legacy-index-reclaim.tmp.wal";
static const char *lock_path = "test-legacy-index-reclaim.tmp.lock";
static const uint8_t collection[] = "users";
static const uint8_t email_index[] = "email";
static const uint8_t password[] = "legacy-index-reclaim-password";

typedef struct visit_count {
    const uint8_t *expected_id;
    size_t expected_id_size;
    size_t matches;
} visit_count;

static bool count_match(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    visit_count *count = (visit_count *)context;
    assert(document_id_size == count->expected_id_size);
    assert(memcmp(document_id, count->expected_id, document_id_size) == 0);
    ++count->matches;
    return true;
}

static size_t find_count(
    sdb_database *database,
    const uint8_t *email,
    size_t email_size,
    const uint8_t *expected_id,
    size_t expected_id_size
)
{
    visit_count count;
    size_t reported = 0U;
    count.expected_id = expected_id;
    count.expected_id_size = expected_id_size;
    count.matches = 0U;
    assert(sdb_index_visit(
        database, collection, sizeof(collection) - 1U,
        email_index, sizeof(email_index) - 1U,
        email, email_size, count_match, &count, &reported
    ) == SDB_OK);
    assert(reported == count.matches);
    return count.matches;
}

static void put_email(
    sdb_database *database,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *email,
    size_t email_size
)
{
    static const uint8_t body[] = "{}";
    sdb_index_term term;
    term.index_name = email_index;
    term.index_name_size = sizeof(email_index) - 1U;
    term.value = email;
    term.value_size = email_size;
    assert(sdb_document_put(
        database, collection, sizeof(collection) - 1U,
        document_id, document_id_size,
        body, sizeof(body) - 1U, &term, 1U
    ) == SDB_OK);
}

int main(void)
{
    const uint8_t id[] = "legacy-doc";
    const uint8_t intruder_id[] = "legacy-intruder";
    const uint8_t old_email[] = "legacy-old@example.com";
    const uint8_t new_email[] = "legacy-new@example.com";
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_verify_result verify_result;
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

    put_email(
        database, id, sizeof(id) - 1U,
        old_email, sizeof(old_email) - 1U
    );
    assert(sdb_engine_strip_document_reverse_for_testing(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id) - 1U
    ) == SDB_OK);
    put_email(
        database, id, sizeof(id) - 1U,
        new_email, sizeof(new_email) - 1U
    );
    assert(find_count(
        database, old_email, sizeof(old_email) - 1U,
        id, sizeof(id) - 1U
    ) == 0U);
    assert(find_count(
        database, new_email, sizeof(new_email) - 1U,
        id, sizeof(id) - 1U
    ) == 1U);
    put_email(
        database, intruder_id, sizeof(intruder_id) - 1U,
        old_email, sizeof(old_email) - 1U
    );
    assert(sdb_document_delete(
        database, collection, sizeof(collection) - 1U,
        intruder_id, sizeof(intruder_id) - 1U
    ) == SDB_OK);

    assert(sdb_document_delete(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id) - 1U
    ) == SDB_OK);
    put_email(
        database, id, sizeof(id) - 1U,
        new_email, sizeof(new_email) - 1U
    );
    assert(sdb_engine_strip_document_reverse_for_testing(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id) - 1U
    ) == SDB_OK);
    assert(sdb_document_delete(
        database, collection, sizeof(collection) - 1U,
        id, sizeof(id) - 1U
    ) == SDB_OK);
    assert(find_count(
        database, new_email, sizeof(new_email) - 1U,
        id, sizeof(id) - 1U
    ) == 0U);
    assert(sdb_database_verify(database, &verify_result) == SDB_OK);
    assert(verify_result.stale_entry_count == 0U);
    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(db_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)printf("legacy index reclaim: ok\n");
    return 0;
}
