
#include "shibadb.h"
#include "crypto.h"
#include "key_manager.h"
#include "sync.h"
#include "superblock_store.h"
#include "page_cache.h"
#include "page.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_status_strings(void)
{
    static const sdb_status codes[] = {
        SDB_OK,
        SDB_E_INVALID_ARGUMENT,
        SDB_E_BUFFER_TOO_SMALL,
        SDB_E_BAD_MAGIC,
        SDB_E_UNSUPPORTED_VERSION,
        SDB_E_CORRUPT,
        SDB_E_OVERFLOW,
        SDB_E_IO,
        SDB_E_INTERNAL,
        SDB_E_TRUNCATED,
        SDB_E_NOT_FOUND,
        SDB_E_AUTHENTICATION,
        SDB_E_CONFLICT,
        SDB_E_BUSY,
    };
    size_t i;
    for (i = 0U; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        const char *s = sdb_status_string(codes[i]);
        assert(s != NULL);
        assert(s[0] != '\0');
    }

    assert(strcmp(sdb_status_string((sdb_status)9999), "unknown status") == 0);
    (void)puts("status strings: ok");
}

static void test_version(void)
{
    const char *v = sdb_version_string();
    assert(v != NULL);
    assert(v[0] != '\0');
    assert(sdb_abi_version() > 0U);
    (void)puts("version helpers: ok");
}

static void test_random_bad_args(void)
{
    uint8_t buf[16];
    assert(sdb_random_bytes(NULL, sizeof(buf)) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_random_bytes(buf, 0U) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_random_bytes(buf, sizeof(buf)) == SDB_OK);
    (void)puts("random bad args: ok");
}

static void test_key_wrap_unwrap(void)
{
    sdb_superblock_v1 sb;
    uint8_t data_key[32];
    uint8_t recovered[32];
    const uint8_t password[] = "hunter2";
    size_t i;

    (void)memset(&sb, 0, sizeof(sb));
    sb.page_size = 4096U;
    sb.next_page_id = 1U;
    sb.flags = SDB_FLAG_ENCRYPTED;
    sb.key_wrap_id = 1U;
    sb.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    for (i = 0U; i < SDB_FILE_ID_SIZE; ++i) {
        sb.file_id[i] = (uint8_t)(0xa0U + i);
        sb.salt[i] = (uint8_t)(0xbbU + i);
    }
    for (i = 0U; i < 32U; ++i) {
        data_key[i] = (uint8_t)(0x11U + i);
    }

    assert(sdb_key_wrap(
        &sb, password, sizeof(password) - 1U, data_key
    ) == SDB_OK);

    assert(sdb_key_unwrap(
        &sb, password, sizeof(password) - 1U, recovered
    ) == SDB_OK);
    assert(memcmp(recovered, data_key, 32U) == 0);

    (void)memset(recovered, 0xff, sizeof(recovered));
    assert(sdb_key_unwrap(
        &sb, (const uint8_t *)"wrong!!", 7U, recovered
    ) == SDB_E_AUTHENTICATION);
    for (i = 0U; i < 32U; ++i) {
        assert(recovered[i] == 0U);
    }

    assert(sdb_key_wrap(NULL, password, 6U, data_key) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_key_wrap(&sb, NULL, 0U, data_key) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_key_wrap(&sb, password, 0U, data_key) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_key_wrap(&sb, password, 6U, NULL) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_key_unwrap(NULL, password, 6U, recovered) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_key_unwrap(&sb, password, 0U, recovered) == SDB_E_INVALID_ARGUMENT);

    sb.flags = 0U;
    assert(sdb_key_wrap(&sb, password, 6U, data_key) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_key_unwrap(&sb, password, 6U, recovered) == SDB_E_INVALID_ARGUMENT);

    (void)puts("key wrap/unwrap: ok");
}

static void test_sync_bad_args(void)
{
    sdb_process_lock lock;

    assert(sdb_mutex_init(NULL) == SDB_E_INVALID_ARGUMENT);

    sdb_mutex_destroy(NULL);

    assert(sdb_process_lock_acquire("/tmp/x", NULL) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_process_lock_acquire_database(NULL, &lock)
        == SDB_E_INVALID_ARGUMENT);
    assert(sdb_process_lock_acquire_database("", &lock)
        == SDB_E_INVALID_ARGUMENT);
    assert(sdb_process_lock_acquire_database("/tmp/x", NULL)
        == SDB_E_INVALID_ARGUMENT);

    (void)sdb_process_lock_release(NULL);

    (void)puts("sync bad args: ok");
}

static void test_page_encode_decode_bad_args(void)
{
    uint8_t page[4096];
    sdb_page_view view;
    uint8_t payload[16] = {0};

    assert(sdb_page_encode(NULL, 4096U, (uint16_t)SDB_PAGE_TYPE_DATA,
        1U, 0U, payload, sizeof(payload)) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_encode(page, 4096U, (uint16_t)SDB_PAGE_TYPE_DATA,
        0U, 0U, payload, sizeof(payload)) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_encode(page, 4096U, (uint16_t)9999U,
        1U, 0U, payload, sizeof(payload)) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_encode(page, 100U, (uint16_t)SDB_PAGE_TYPE_DATA,
        1U, 0U, payload, sizeof(payload)) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_encode(page, 3000U, (uint16_t)SDB_PAGE_TYPE_DATA,
        1U, 0U, payload, sizeof(payload)) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_encode(page, 4096U, (uint16_t)SDB_PAGE_TYPE_DATA,
        1U, 0U, payload, (size_t)0xffffffffU + 1U)
        == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_encode(page, 4096U, (uint16_t)SDB_PAGE_TYPE_DATA,
        1U, 0U, NULL, 8U) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_decode(NULL, 4096U, 1U, &view) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_decode(page, 4096U, 1U, NULL) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_decode(page, 4096U, 0U, &view) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_decode(page, 100U, 1U, &view) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_decode(page, 3000U, 1U, &view) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_encode(page, 4096U, (uint16_t)SDB_PAGE_TYPE_DATA,
        1U, 42U, payload, sizeof(payload)) == SDB_OK);
    assert(sdb_page_decode(page, 4096U, 1U, &view) == SDB_OK);
    assert(view.page_id == 1U);
    assert(view.page_lsn == 42U);
    assert(view.payload_size == sizeof(payload));

    assert(sdb_page_decode(page, 4096U, 2U, &view) == SDB_E_CORRUPT);

    (void)puts("page encode/decode bad args: ok");
}

static void test_page_cache_bad_args(void)
{
    sdb_page_cache cache;
    uint8_t buf[4096];

    assert(sdb_page_cache_init(NULL, 4U, 4096U) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_cache_init(&cache, 0U, 4096U) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_cache_init(&cache, 4U, 0U) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_page_cache_init(&cache, SIZE_MAX / 2U, 8U) == SDB_E_OVERFLOW);

    assert(sdb_page_cache_get(NULL, 1U, buf) == false);

    assert(sdb_page_cache_init(&cache, 4U, 4096U) == SDB_OK);
    assert(sdb_page_cache_get(&cache, 1U, NULL) == false);

    sdb_page_cache_put(NULL, 1U, buf);
    sdb_page_cache_put(&cache, 1U, NULL);

    sdb_page_cache_remove(NULL, 1U);

    (void)memset(buf, 0x77, sizeof(buf));
    sdb_page_cache_put(&cache, 42U, buf);
    (void)memset(buf, 0, sizeof(buf));
    assert(sdb_page_cache_get(&cache, 42U, buf) == true);
    assert(buf[0] == 0x77);

    sdb_page_cache_remove(&cache, 42U);
    assert(sdb_page_cache_get(&cache, 42U, buf) == false);

    sdb_page_cache_destroy(&cache);
    sdb_page_cache_destroy(NULL);

    (void)puts("page_cache bad args: ok");
}

static void test_xchacha_partial_block(void)
{

    static const uint8_t key[32] = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U};
    static const uint8_t nonce[24] = {0x11U, 0x12U, 0x13U};
    uint8_t plaintext[17];
    uint8_t ciphertext[17];
    uint8_t roundtrip[17];
    uint8_t tag[16];
    size_t i;
    for (i = 0U; i < sizeof(plaintext); ++i) {
        plaintext[i] = (uint8_t)(0x40U + i);
    }
    assert(sdb_xchacha20poly1305_encrypt(
        key, nonce, NULL, 0U,
        plaintext, sizeof(plaintext),
        ciphertext, tag
    ) == SDB_OK);
    assert(sdb_xchacha20poly1305_decrypt(
        key, nonce, NULL, 0U,
        ciphertext, sizeof(ciphertext),
        tag, roundtrip
    ) == SDB_OK);
    assert(memcmp(roundtrip, plaintext, sizeof(plaintext)) == 0);
    (void)puts("xchacha partial-block: ok");
}

static void test_superblock_store_bad_args(void)
{
    sdb_superblock_v1 sb;
    sdb_superblock_read_result result;

    (void)memset(&sb, 0, sizeof(sb));
    sb.page_size = 4096U;
    sb.next_page_id = 1U;
    { size_t i; for (i = 0U; i < SDB_FILE_ID_SIZE; ++i) sb.file_id[i] = 1U; }

    assert(sdb_superblock_store_read_file(NULL, &result)
        == SDB_E_INVALID_ARGUMENT);

    assert(sdb_superblock_store_create(NULL, &sb) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_superblock_store_create("/tmp/x", NULL) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_superblock_store_read(NULL, &result) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_superblock_store_read("/tmp/x", NULL) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_superblock_store_update(NULL, &sb) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_superblock_store_update("/tmp/x", NULL) == SDB_E_INVALID_ARGUMENT);

    (void)puts("superblock_store bad args: ok");
}

int main(void)
{
    test_status_strings();
    test_version();
    test_random_bad_args();
    test_key_wrap_unwrap();
    test_sync_bad_args();
    test_superblock_store_bad_args();
    test_page_cache_bad_args();
    test_page_encode_decode_bad_args();
    test_xchacha_partial_block();
    (void)puts("coverage gap tests: all ok");
    return 0;
}
