#include "crypto.h"

#include "chacha_simd.h"
#include "cpu_features.h"

#include <string.h>

typedef struct sdb_poly1305 {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t buffer[16];
    size_t buffered;
} sdb_poly1305;

static uint32_t sdb_rotl32(uint32_t value, unsigned amount)
{
    return (value << amount) | (value >> (32U - amount));
}

static void sdb_chacha_quarter(
    uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d
)
{
    *a += *b;
    *d ^= *a;
    *d = sdb_rotl32(*d, 16U);
    *c += *d;
    *b ^= *c;
    *b = sdb_rotl32(*b, 12U);
    *a += *b;
    *d ^= *a;
    *d = sdb_rotl32(*d, 8U);
    *c += *d;
    *b ^= *c;
    *b = sdb_rotl32(*b, 7U);
}

static void sdb_chacha_rounds(uint32_t state[16])
{
    unsigned round;
    for (round = 0U; round < 10U; ++round) {
        sdb_chacha_quarter(&state[0], &state[4], &state[8], &state[12]);
        sdb_chacha_quarter(&state[1], &state[5], &state[9], &state[13]);
        sdb_chacha_quarter(&state[2], &state[6], &state[10], &state[14]);
        sdb_chacha_quarter(&state[3], &state[7], &state[11], &state[15]);
        sdb_chacha_quarter(&state[0], &state[5], &state[10], &state[15]);
        sdb_chacha_quarter(&state[1], &state[6], &state[11], &state[12]);
        sdb_chacha_quarter(&state[2], &state[7], &state[8], &state[13]);
        sdb_chacha_quarter(&state[3], &state[4], &state[9], &state[14]);
    }
}

static void sdb_chacha_state(
    uint32_t state[16],
    const uint8_t key[32],
    uint32_t counter,
    const uint8_t nonce[12]
)
{
    static const uint32_t constants[4] = {
        UINT32_C(0x61707865), UINT32_C(0x3320646e),
        UINT32_C(0x79622d32), UINT32_C(0x6b206574)
    };
    size_t index;
    (void)memcpy(state, constants, sizeof(constants));
    for (index = 0U; index < 8U; ++index) {
        state[4U + index] = sdb_read_u32_le(key + (index * 4U));
    }
    state[12] = counter;
    state[13] = sdb_read_u32_le(nonce);
    state[14] = sdb_read_u32_le(nonce + 4U);
    state[15] = sdb_read_u32_le(nonce + 8U);
}

static void sdb_chacha_block(
    const uint8_t key[32],
    uint32_t counter,
    const uint8_t nonce[12],
    uint8_t output[64]
)
{
    uint32_t initial[16];
    uint32_t state[16];
    size_t index;
    sdb_chacha_state(initial, key, counter, nonce);
    (void)memcpy(state, initial, sizeof(state));
    sdb_chacha_rounds(state);
    for (index = 0U; index < 16U; ++index) {
        sdb_write_u32_le(output + (index * 4U), state[index] + initial[index]);
    }
    sdb_secure_zero(initial, sizeof(initial));
    sdb_secure_zero(state, sizeof(state));
}

void sdb_hchacha20(
    const uint8_t key[32], const uint8_t nonce[16], uint8_t output[32]
)
{
    uint32_t state[16];
    size_t index;
    static const uint32_t constants[4] = {
        UINT32_C(0x61707865), UINT32_C(0x3320646e),
        UINT32_C(0x79622d32), UINT32_C(0x6b206574)
    };
    (void)memcpy(state, constants, sizeof(constants));
    for (index = 0U; index < 8U; ++index) {
        state[4U + index] = sdb_read_u32_le(key + (index * 4U));
    }
    for (index = 0U; index < 4U; ++index) {
        state[12U + index] = sdb_read_u32_le(nonce + (index * 4U));
    }
    sdb_chacha_rounds(state);
    sdb_write_u32_le(output, state[0]);
    sdb_write_u32_le(output + 4U, state[1]);
    sdb_write_u32_le(output + 8U, state[2]);
    sdb_write_u32_le(output + 12U, state[3]);
    sdb_write_u32_le(output + 16U, state[12]);
    sdb_write_u32_le(output + 20U, state[13]);
    sdb_write_u32_le(output + 24U, state[14]);
    sdb_write_u32_le(output + 28U, state[15]);
    sdb_secure_zero(state, sizeof(state));
}

static void sdb_chacha_xor(
    const uint8_t key[32],
    const uint8_t nonce[12],
    uint32_t counter,
    const uint8_t *input,
    uint8_t *output,
    size_t size
)
{
    uint8_t block[64];
#if SDB_CPU_X86
    if (size >= 512U && sdb_cpu_features_get()->avx2) {
        const size_t groups = size / 512U;
        const size_t bytes = groups * 512U;
        sdb_chacha8_xor(key, nonce, counter, input, output, groups);
        counter += (uint32_t)(groups * 8U);
        input += bytes;
        output += bytes;
        size -= bytes;
    }
#endif
    while (size != 0U) {
        const size_t take = size < sizeof(block) ? size : sizeof(block);
        size_t index;
        sdb_chacha_block(key, counter, nonce, block);
        for (index = 0U; index < take; ++index) {
            output[index] = input[index] ^ block[index];
        }
        input += take;
        output += take;
        size -= take;
        ++counter;
    }
    sdb_secure_zero(block, sizeof(block));
}

static void sdb_poly1305_blocks(
    sdb_poly1305 *context,
    const uint8_t *message,
    size_t size,
    uint32_t high_bit
)
{
    const uint32_t r0 = context->r[0];
    const uint32_t r1 = context->r[1];
    const uint32_t r2 = context->r[2];
    const uint32_t r3 = context->r[3];
    const uint32_t r4 = context->r[4];
    const uint32_t s1 = r1 * 5U;
    const uint32_t s2 = r2 * 5U;
    const uint32_t s3 = r3 * 5U;
    const uint32_t s4 = r4 * 5U;
    uint32_t h0 = context->h[0];
    uint32_t h1 = context->h[1];
    uint32_t h2 = context->h[2];
    uint32_t h3 = context->h[3];
    uint32_t h4 = context->h[4];
    while (size >= 16U) {
        uint64_t d0;
        uint64_t d1;
        uint64_t d2;
        uint64_t d3;
        uint64_t d4;
        uint32_t carry;
        h0 += sdb_read_u32_le(message) & UINT32_C(0x3ffffff);
        h1 += (sdb_read_u32_le(message + 3U) >> 2U)
            & UINT32_C(0x3ffffff);
        h2 += (sdb_read_u32_le(message + 6U) >> 4U)
            & UINT32_C(0x3ffffff);
        h3 += (sdb_read_u32_le(message + 9U) >> 6U)
            & UINT32_C(0x3ffffff);
        h4 += (sdb_read_u32_le(message + 12U) >> 8U) | high_bit;
        d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4
            + (uint64_t)h2 * s3 + (uint64_t)h3 * s2
            + (uint64_t)h4 * s1;
        d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0
            + (uint64_t)h2 * s4 + (uint64_t)h3 * s3
            + (uint64_t)h4 * s2;
        d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1
            + (uint64_t)h2 * r0 + (uint64_t)h3 * s4
            + (uint64_t)h4 * s3;
        d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2
            + (uint64_t)h2 * r1 + (uint64_t)h3 * r0
            + (uint64_t)h4 * s4;
        d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3
            + (uint64_t)h2 * r2 + (uint64_t)h3 * r1
            + (uint64_t)h4 * r0;
        carry = (uint32_t)(d0 >> 26U);
        h0 = (uint32_t)d0 & UINT32_C(0x3ffffff);
        d1 += carry;
        carry = (uint32_t)(d1 >> 26U);
        h1 = (uint32_t)d1 & UINT32_C(0x3ffffff);
        d2 += carry;
        carry = (uint32_t)(d2 >> 26U);
        h2 = (uint32_t)d2 & UINT32_C(0x3ffffff);
        d3 += carry;
        carry = (uint32_t)(d3 >> 26U);
        h3 = (uint32_t)d3 & UINT32_C(0x3ffffff);
        d4 += carry;
        carry = (uint32_t)(d4 >> 26U);
        h4 = (uint32_t)d4 & UINT32_C(0x3ffffff);
        h0 += carry * 5U;
        carry = h0 >> 26U;
        h0 &= UINT32_C(0x3ffffff);
        h1 += carry;
        message += 16U;
        size -= 16U;
    }
    context->h[0] = h0;
    context->h[1] = h1;
    context->h[2] = h2;
    context->h[3] = h3;
    context->h[4] = h4;
}

static void sdb_poly1305_init(
    sdb_poly1305 *context, const uint8_t key[32]
)
{
    (void)memset(context, 0, sizeof(*context));
    context->r[0] = sdb_read_u32_le(key) & UINT32_C(0x3ffffff);
    context->r[1] = (sdb_read_u32_le(key + 3U) >> 2U)
        & UINT32_C(0x3ffff03);
    context->r[2] = (sdb_read_u32_le(key + 6U) >> 4U)
        & UINT32_C(0x3ffc0ff);
    context->r[3] = (sdb_read_u32_le(key + 9U) >> 6U)
        & UINT32_C(0x3f03fff);
    context->r[4] = (sdb_read_u32_le(key + 12U) >> 8U)
        & UINT32_C(0x00fffff);
    context->pad[0] = sdb_read_u32_le(key + 16U);
    context->pad[1] = sdb_read_u32_le(key + 20U);
    context->pad[2] = sdb_read_u32_le(key + 24U);
    context->pad[3] = sdb_read_u32_le(key + 28U);
}

static void sdb_poly1305_update(
    sdb_poly1305 *context, const uint8_t *message, size_t size
)
{
    if (context->buffered != 0U) {
        const size_t available = 16U - context->buffered;
        const size_t take = size < available ? size : available;
        (void)memcpy(context->buffer + context->buffered, message, take);
        context->buffered += take;
        message += take;
        size -= take;
        if (context->buffered == 16U) {
            sdb_poly1305_blocks(
                context, context->buffer, 16U, UINT32_C(1) << 24U
            );
            context->buffered = 0U;
        }
    }
    if (size >= 16U) {
        const size_t full_size = size & ~(size_t)15U;
        sdb_poly1305_blocks(
            context, message, full_size, UINT32_C(1) << 24U
        );
        message += full_size;
        size -= full_size;
    }
    if (size != 0U) {
        (void)memcpy(context->buffer, message, size);
        context->buffered = size;
    }
}

static void sdb_poly1305_final(
    sdb_poly1305 *context, uint8_t tag[16]
)
{
    uint32_t h0;
    uint32_t h1;
    uint32_t h2;
    uint32_t h3;
    uint32_t h4;
    uint32_t g0;
    uint32_t g1;
    uint32_t g2;
    uint32_t g3;
    uint32_t g4;
    uint32_t carry;
    uint32_t mask;
    uint64_t f;
    if (context->buffered != 0U) {
        context->buffer[context->buffered++] = 1U;
        (void)memset(
            context->buffer + context->buffered,
            0,
            16U - context->buffered
        );
        sdb_poly1305_blocks(context, context->buffer, 16U, 0U);
    }
    h0 = context->h[0];
    h1 = context->h[1];
    h2 = context->h[2];
    h3 = context->h[3];
    h4 = context->h[4];
    carry = h1 >> 26U;
    h1 &= UINT32_C(0x3ffffff);
    h2 += carry;
    carry = h2 >> 26U;
    h2 &= UINT32_C(0x3ffffff);
    h3 += carry;
    carry = h3 >> 26U;
    h3 &= UINT32_C(0x3ffffff);
    h4 += carry;
    carry = h4 >> 26U;
    h4 &= UINT32_C(0x3ffffff);
    h0 += carry * 5U;
    carry = h0 >> 26U;
    h0 &= UINT32_C(0x3ffffff);
    h1 += carry;
    g0 = h0 + 5U;
    carry = g0 >> 26U;
    g0 &= UINT32_C(0x3ffffff);
    g1 = h1 + carry;
    carry = g1 >> 26U;
    g1 &= UINT32_C(0x3ffffff);
    g2 = h2 + carry;
    carry = g2 >> 26U;
    g2 &= UINT32_C(0x3ffffff);
    g3 = h3 + carry;
    carry = g3 >> 26U;
    g3 &= UINT32_C(0x3ffffff);
    g4 = h4 + carry - (UINT32_C(1) << 26U);
    mask = (g4 >> 31U) - 1U;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;
    f = (uint64_t)(h0 | (h1 << 26U))
        + context->pad[0];
    sdb_write_u32_le(tag, (uint32_t)f);
    f = (uint64_t)((h1 >> 6U) | (h2 << 20U))
        + context->pad[1] + (f >> 32U);
    sdb_write_u32_le(tag + 4U, (uint32_t)f);
    f = (uint64_t)((h2 >> 12U) | (h3 << 14U))
        + context->pad[2] + (f >> 32U);
    sdb_write_u32_le(tag + 8U, (uint32_t)f);
    f = (uint64_t)((h3 >> 18U) | (h4 << 8U))
        + context->pad[3] + (f >> 32U);
    sdb_write_u32_le(tag + 12U, (uint32_t)f);
    sdb_secure_zero(context, sizeof(*context));
}

static void sdb_poly1305_pad16(sdb_poly1305 *context, size_t size)
{
    static const uint8_t zeroes[16] = {0U};
    const size_t remainder = size & 15U;
    if (remainder != 0U) {
        sdb_poly1305_update(context, zeroes, 16U - remainder);
    }
}

static sdb_status sdb_xchacha_setup(
    const uint8_t key[32],
    const uint8_t nonce[24],
    uint8_t subkey[32],
    uint8_t ietf_nonce[12]
)
{
    if (key == NULL || nonce == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    sdb_hchacha20(key, nonce, subkey);
    (void)memset(ietf_nonce, 0, 4U);
    (void)memcpy(ietf_nonce + 4U, nonce + 16U, 8U);
    return SDB_OK;
}

static void sdb_aead_tag(
    const uint8_t poly_key[32],
    const uint8_t *aad,
    size_t aad_size,
    const uint8_t *ciphertext,
    size_t ciphertext_size,
    uint8_t tag[16]
)
{
    sdb_poly1305 context;
    uint8_t lengths[16];
    sdb_poly1305_init(&context, poly_key);
    if (aad_size != 0U) {
        sdb_poly1305_update(&context, aad, aad_size);
    }
    sdb_poly1305_pad16(&context, aad_size);
    if (ciphertext_size != 0U) {
        sdb_poly1305_update(&context, ciphertext, ciphertext_size);
    }
    sdb_poly1305_pad16(&context, ciphertext_size);
    sdb_write_u64_le(lengths, (uint64_t)aad_size);
    sdb_write_u64_le(lengths + 8U, (uint64_t)ciphertext_size);
    sdb_poly1305_update(&context, lengths, sizeof(lengths));
    sdb_poly1305_final(&context, tag);
    sdb_secure_zero(lengths, sizeof(lengths));
}

sdb_status sdb_xchacha20poly1305_encrypt(
    const uint8_t key[32],
    const uint8_t nonce[24],
    const uint8_t *aad,
    size_t aad_size,
    const uint8_t *plaintext,
    size_t plaintext_size,
    uint8_t *ciphertext,
    uint8_t tag[16]
)
{
    uint8_t subkey[32];
    uint8_t ietf_nonce[12];
    uint8_t first_block[64];
    /*
     * The message stream starts at counter 1, so the largest accepted size
     * must keep the final 32-bit block counter at or below UINT32_MAX —
     * otherwise the counter wraps to 0 and reuses the Poly1305-key block.
     * (size_t)(UINT32_MAX - 1) * 64 is far beyond any real page; the bound
     * exists so the invariant holds by construction.
     */
    if (key == NULL || nonce == NULL || tag == NULL
        || (aad == NULL && aad_size != 0U)
        || (plaintext == NULL && plaintext_size != 0U)
        || (ciphertext == NULL && plaintext_size != 0U)
        || plaintext_size > (size_t)(UINT32_MAX - 1U) * 64U) {
        return SDB_E_INVALID_ARGUMENT;
    }

    {
        const sdb_status setup_status =
            sdb_xchacha_setup(key, nonce, subkey, ietf_nonce);
        if (setup_status != SDB_OK) {
            return setup_status;
        }
    }
    sdb_chacha_block(subkey, 0U, ietf_nonce, first_block);
    if (plaintext_size != 0U) {
        sdb_chacha_xor(
            subkey,
            ietf_nonce,
            1U,
            plaintext,
            ciphertext,
            plaintext_size
        );
    }
    sdb_aead_tag(
        first_block, aad, aad_size, ciphertext, plaintext_size, tag
    );
    sdb_secure_zero(subkey, sizeof(subkey));
    sdb_secure_zero(ietf_nonce, sizeof(ietf_nonce));
    sdb_secure_zero(first_block, sizeof(first_block));
    return SDB_OK;
}

sdb_status sdb_xchacha20poly1305_decrypt(
    const uint8_t key[32],
    const uint8_t nonce[24],
    const uint8_t *aad,
    size_t aad_size,
    const uint8_t *ciphertext,
    size_t ciphertext_size,
    const uint8_t tag[16],
    uint8_t *plaintext
)
{
    uint8_t subkey[32];
    uint8_t ietf_nonce[12];
    uint8_t first_block[64];
    uint8_t expected[16];
    if (key == NULL || nonce == NULL || tag == NULL
        || (aad == NULL && aad_size != 0U)
        || (ciphertext == NULL && ciphertext_size != 0U)
        || (plaintext == NULL && ciphertext_size != 0U)
        || ciphertext_size > (size_t)(UINT32_MAX - 1U) * 64U) {
        return SDB_E_INVALID_ARGUMENT;
    }

    {
        const sdb_status setup_status =
            sdb_xchacha_setup(key, nonce, subkey, ietf_nonce);
        if (setup_status != SDB_OK) {
            return setup_status;
        }
    }
    sdb_chacha_block(subkey, 0U, ietf_nonce, first_block);
    sdb_aead_tag(
        first_block, aad, aad_size, ciphertext, ciphertext_size, expected
    );
    if (!sdb_constant_time_equal(expected, tag, sizeof(expected))) {
        sdb_secure_zero(subkey, sizeof(subkey));
        sdb_secure_zero(ietf_nonce, sizeof(ietf_nonce));
        sdb_secure_zero(first_block, sizeof(first_block));
        sdb_secure_zero(expected, sizeof(expected));
        return SDB_E_CORRUPT;
    }
    if (ciphertext_size != 0U) {
        sdb_chacha_xor(
            subkey,
            ietf_nonce,
            1U,
            ciphertext,
            plaintext,
            ciphertext_size
        );
    }
    sdb_secure_zero(subkey, sizeof(subkey));
    sdb_secure_zero(ietf_nonce, sizeof(ietf_nonce));
    sdb_secure_zero(first_block, sizeof(first_block));
    sdb_secure_zero(expected, sizeof(expected));
    return SDB_OK;
}
