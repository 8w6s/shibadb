/*
 * Component micro-benchmarks for the crypto/checksum hot paths.
 *
 * The engine-level bench_kv only reports end-to-end ops/s, which hides where
 * time actually goes. This harness isolates each primitive so a SIMD backend
 * can be measured against the scalar one in MB/s (throughput) and, for PBKDF2,
 * in milliseconds-per-derive (the cost paid on every encrypted-DB open).
 *
 * Links ShibaDB::core (static) so it can reach the internal crypto/checksum
 * symbols directly. Config via env vars: SDB_BENCH_MB (bytes streamed per
 * throughput test, default 256 MB) and SDB_BENCH_PBKDF2 (KDF iterations,
 * default 600000 = the OWASP floor the engine uses). Set SDB_NO_SIMD=1 to
 * force the scalar backend and compare against the SIMD numbers.
 */

#include "cpu_features.h"
#include "crypto.h"
#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_timer.h"

static uint64_t bench_env_u64(const char *name, uint64_t fallback)
{
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long long value;
    if (text == NULL || text[0] == '\0') {
        return fallback;
    }
    value = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0ULL) {
        return fallback;
    }
    return (uint64_t)value;
}

static void report_throughput(
    const char *name, uint64_t total_bytes, uint64_t elapsed_ns
)
{
    const double secs = (double)elapsed_ns / 1e9;
    const double mb = (double)total_bytes / 1e6;
    (void)printf(
        "  %-28s %9.1f MB/s  (%6.1f MB in %7.1f ms)\n",
        name,
        secs > 0.0 ? mb / secs : 0.0,
        mb,
        secs * 1e3
    );
}

int main(void)
{
    const uint64_t stream_mb = bench_env_u64("SDB_BENCH_MB", 256U);
    const uint32_t pbkdf2_iters =
        (uint32_t)bench_env_u64("SDB_BENCH_PBKDF2", 600000U);
    const size_t chunk = (size_t)1U << 20; /* 1 MiB working buffer */
    const size_t page = 4096U;             /* representative page payload */
    const uint64_t total_bytes = stream_mb * UINT64_C(1000000);

    uint8_t *buffer = (uint8_t *)malloc(chunk);
    uint8_t *ciphertext = (uint8_t *)malloc(page);
    uint8_t key[32];
    uint8_t nonce[24];
    uint8_t tag[16];
    uint8_t digest[32];
    uint8_t derived[32];
    uint64_t processed;
    uint64_t start;
    uint64_t elapsed;
    volatile uint32_t crc_sink = 0U;

    if (buffer == NULL || ciphertext == NULL) {
        (void)fprintf(stderr, "bench_crypto: out of memory\n");
        free(buffer);
        free(ciphertext);
        return 1;
    }
    (void)memset(buffer, 0xA5, chunk);
    (void)memset(key, 0x24, sizeof(key));
    (void)memset(nonce, 0x42, sizeof(nonce));

    (void)printf(
        "shibadb crypto micro-benchmark: stream=%lluMB pbkdf2_iters=%lu\n",
        (unsigned long long)stream_mb, (unsigned long)pbkdf2_iters
    );
    {
        const sdb_cpu_features *cpu = sdb_cpu_features_get();
        (void)printf(
            "  cpu: sse2=%d sse41=%d avx2=%d sha_ni=%d pclmulqdq=%d\n",
            (int)cpu->sse2, (int)cpu->sse41, (int)cpu->avx2,
            (int)cpu->sha, (int)cpu->pclmulqdq
        );
    }

    /* SHA-256 one-shot throughput (drives sha256_transform, the SHA-NI seat). */
    start = bench_now_ns();
    for (processed = 0U; processed < total_bytes; processed += chunk) {
        sdb_sha256(buffer, chunk, digest);
    }
    elapsed = bench_now_ns() - start;
    report_throughput("SHA-256", processed, elapsed);

    /* CRC-32 throughput (page + WAL checksums; the PCLMULQDQ seat). */
    start = bench_now_ns();
    for (processed = 0U; processed < total_bytes; processed += chunk) {
        crc_sink ^= sdb_crc32(buffer, chunk);
    }
    elapsed = bench_now_ns() - start;
    report_throughput("CRC-32", processed, elapsed);
    (void)crc_sink;

    /* XChaCha20-Poly1305 encrypt of a 4 KiB page (ChaCha + Poly1305 seats). */
    start = bench_now_ns();
    for (processed = 0U; processed < total_bytes; processed += page) {
        sdb_status status = sdb_xchacha20poly1305_encrypt(
            key, nonce, NULL, 0U, buffer, page, ciphertext, tag
        );
        if (status != SDB_OK) {
            (void)fprintf(stderr, "bench_crypto: encrypt failed (%d)\n",
                (int)status);
            free(buffer);
            free(ciphertext);
            return 1;
        }
    }
    elapsed = bench_now_ns() - start;
    report_throughput("XChaCha20-Poly1305 enc", processed, elapsed);

    /* PBKDF2 = the cost of opening an encrypted database. Report ms/derive. */
    start = bench_now_ns();
    {
        sdb_status status = sdb_pbkdf2_hmac_sha256(
            key, sizeof(key), nonce, 16U, pbkdf2_iters, derived, sizeof(derived)
        );
        if (status != SDB_OK) {
            (void)fprintf(stderr, "bench_crypto: pbkdf2 failed (%d)\n",
                (int)status);
            free(buffer);
            free(ciphertext);
            return 1;
        }
    }
    elapsed = bench_now_ns() - start;
    (void)printf(
        "  %-28s %9.1f ms/derive  (%lu iters = one encrypted-DB open)\n",
        "PBKDF2-HMAC-SHA256",
        (double)elapsed / 1e6,
        (unsigned long)pbkdf2_iters
    );

    free(buffer);
    free(ciphertext);
    return 0;
}
