#include "crypto.h"

#include <stdlib.h>
#include <string.h>

static const uint32_t sdb_sha256_constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)
};

static uint32_t sdb_rotr32(uint32_t value, unsigned amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static uint32_t sdb_read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U)
        | ((uint32_t)input[1] << 16U)
        | ((uint32_t)input[2] << 8U)
        | (uint32_t)input[3];
}

static void sdb_write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void sdb_sha256_transform(
    sdb_sha256_context *context, const uint8_t block[64]
)
{
    uint32_t schedule[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        schedule[index] = sdb_read_u32_be(block + (index * 4U));
    }
    for (index = 16U; index < 64U; ++index) {
        const uint32_t s0 =
            sdb_rotr32(schedule[index - 15U], 7U)
            ^ sdb_rotr32(schedule[index - 15U], 18U)
            ^ (schedule[index - 15U] >> 3U);
        const uint32_t s1 =
            sdb_rotr32(schedule[index - 2U], 17U)
            ^ sdb_rotr32(schedule[index - 2U], 19U)
            ^ (schedule[index - 2U] >> 10U);
        schedule[index] = schedule[index - 16U] + s0
            + schedule[index - 7U] + s1;
    }
    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (index = 0U; index < 64U; ++index) {
        const uint32_t sum1 = sdb_rotr32(e, 6U)
            ^ sdb_rotr32(e, 11U) ^ sdb_rotr32(e, 25U);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temporary1 =
            h + sum1 + choose + sdb_sha256_constants[index] + schedule[index];
        const uint32_t sum0 = sdb_rotr32(a, 2U)
            ^ sdb_rotr32(a, 13U) ^ sdb_rotr32(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
    sdb_secure_zero(schedule, sizeof(schedule));
}

void sdb_sha256_init(sdb_sha256_context *context)
{
    static const uint32_t initial[8] = {
        UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
        UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
        UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
        UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
    };
    (void)memset(context, 0, sizeof(*context));
    (void)memcpy(context->state, initial, sizeof(initial));
}

void sdb_sha256_update(
    sdb_sha256_context *context, const uint8_t *data, size_t size
)
{
    while (size != 0U) {
        const size_t available = 64U - context->buffered;
        const size_t take = size < available ? size : available;
        (void)memcpy(context->buffer + context->buffered, data, take);
        context->buffered += take;
        context->bit_count += (uint64_t)take * UINT64_C(8);
        data += take;
        size -= take;
        if (context->buffered == 64U) {
            sdb_sha256_transform(context, context->buffer);
            context->buffered = 0U;
        }
    }
}

void sdb_sha256_final(
    sdb_sha256_context *context, uint8_t output[32]
)
{
    uint64_t bit_count = context->bit_count;
    size_t index;
    context->buffer[context->buffered++] = 0x80U;
    if (context->buffered > 56U) {
        (void)memset(
            context->buffer + context->buffered, 0, 64U - context->buffered
        );
        sdb_sha256_transform(context, context->buffer);
        context->buffered = 0U;
    }
    (void)memset(
        context->buffer + context->buffered, 0, 56U - context->buffered
    );
    for (index = 0U; index < 8U; ++index) {
        context->buffer[63U - index] = (uint8_t)bit_count;
        bit_count >>= 8U;
    }
    sdb_sha256_transform(context, context->buffer);
    for (index = 0U; index < 8U; ++index) {
        sdb_write_u32_be(output + (index * 4U), context->state[index]);
    }
    sdb_secure_zero(context, sizeof(*context));
}

void sdb_secure_zero(void *memory, size_t size)
{
    volatile uint8_t *cursor = (volatile uint8_t *)memory;
    while (size-- != 0U) {
        *cursor++ = 0U;
    }
}

bool sdb_constant_time_equal(
    const uint8_t *left, const uint8_t *right, size_t size
)
{
    uint8_t difference = 0U;
    size_t index;
    for (index = 0U; index < size; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

void sdb_sha256(const uint8_t *data, size_t size, uint8_t output[32])
{
    sdb_sha256_context context;
    sdb_sha256_init(&context);
    if (size != 0U) {
        sdb_sha256_update(&context, data, size);
    }
    sdb_sha256_final(&context, output);
}

void sdb_hmac_sha256(
    const uint8_t *key,
    size_t key_size,
    const uint8_t *data,
    size_t data_size,
    uint8_t output[32]
)
{
    uint8_t normalized[64];
    uint8_t inner_hash[32];
    uint8_t pad[64];
    sdb_sha256_context context;
    size_t index;
    (void)memset(normalized, 0, sizeof(normalized));
    if (key_size > 64U) {
        sdb_sha256(key, key_size, normalized);
    } else if (key_size != 0U) {
        (void)memcpy(normalized, key, key_size);
    }
    for (index = 0U; index < 64U; ++index) {
        pad[index] = normalized[index] ^ 0x36U;
    }
    sdb_sha256_init(&context);
    sdb_sha256_update(&context, pad, sizeof(pad));
    if (data_size != 0U) {
        sdb_sha256_update(&context, data, data_size);
    }
    sdb_sha256_final(&context, inner_hash);
    for (index = 0U; index < 64U; ++index) {
        pad[index] = normalized[index] ^ 0x5cU;
    }
    sdb_sha256_init(&context);
    sdb_sha256_update(&context, pad, sizeof(pad));
    sdb_sha256_update(&context, inner_hash, sizeof(inner_hash));
    sdb_sha256_final(&context, output);
    sdb_secure_zero(normalized, sizeof(normalized));
    sdb_secure_zero(inner_hash, sizeof(inner_hash));
    sdb_secure_zero(pad, sizeof(pad));
}

sdb_status sdb_pbkdf2_hmac_sha256(
    const uint8_t *password,
    size_t password_size,
    const uint8_t *salt,
    size_t salt_size,
    uint32_t iterations,
    uint8_t *output,
    size_t output_size
)
{
    uint8_t *message;
    size_t message_size;
    uint32_t block = 1U;
    if ((password == NULL && password_size != 0U)
        || (salt == NULL && salt_size != 0U)
        || (output == NULL && output_size != 0U)
        || iterations == 0U || salt_size > SIZE_MAX - 4U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    message_size = salt_size + 4U;
    message = (uint8_t *)malloc(message_size);
    if (message == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    if (salt_size != 0U) {
        (void)memcpy(message, salt, salt_size);
    }
    while (output_size != 0U) {
        uint8_t u[32];
        uint8_t result[32];
        uint32_t iteration;
        size_t take = output_size < 32U ? output_size : 32U;
        message[salt_size] = (uint8_t)(block >> 24U);
        message[salt_size + 1U] = (uint8_t)(block >> 16U);
        message[salt_size + 2U] = (uint8_t)(block >> 8U);
        message[salt_size + 3U] = (uint8_t)block;
        sdb_hmac_sha256(
            password, password_size, message, message_size, u
        );
        (void)memcpy(result, u, sizeof(result));
        for (iteration = 1U; iteration < iterations; ++iteration) {
            size_t index;
            sdb_hmac_sha256(password, password_size, u, sizeof(u), u);
            for (index = 0U; index < sizeof(result); ++index) {
                result[index] ^= u[index];
            }
        }
        (void)memcpy(output, result, take);
        output += take;
        output_size -= take;
        if (block == UINT32_MAX && output_size != 0U) {
            sdb_secure_zero(u, sizeof(u));
            sdb_secure_zero(result, sizeof(result));
            sdb_secure_zero(message, message_size);
            free(message);
            return SDB_E_OVERFLOW;
        }
        ++block;
        sdb_secure_zero(u, sizeof(u));
        sdb_secure_zero(result, sizeof(result));
    }
    sdb_secure_zero(message, message_size);
    free(message);
    return SDB_OK;
}
