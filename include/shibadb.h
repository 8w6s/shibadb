/**
 * @file shibadb.h
 * @brief ShibaDB — low-level public API: status codes, superblock
 *        format, and superblock storage.
 *
 * This header exposes the primitives shared between library
 * consumers and the higher-level engine API in
 * `shibadb_engine.h`. Everything declared here is ABI-stable for
 * the current major version (`SDB_ABI_VERSION`).
 *
 * @section overview Threading and lifetimes
 *
 * All functions in this header are stateless with respect to
 * ShibaDB globals — they read/write on-disk data through the path
 * argument, and no library-wide state is mutated. They may be
 * called concurrently from multiple threads as long as the caller
 * does not race them against each other on the *same* path.
 *
 * All output pointers are written only on success unless the
 * function description says otherwise; on failure, output buffers
 * are left untouched.
 *
 * @section errors Error handling
 *
 * Every function returns an `sdb_status` code. `SDB_OK` (0) is
 * success; every other value is a failure. Failures never leak
 * partial output. Human-readable status strings are available via
 * `sdb_status_string()`.
 */
#ifndef SHIBADB_H
#define SHIBADB_H

#include <stddef.h>
#include <stdint.h>

#include "shibadb_visibility.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Format constants
 *
 * On-disk format version and structural sizes. These values are
 * frozen for the lifetime of the current ABI major version.
 * @{
 */

/** Current on-disk superblock version. */
#define SDB_FORMAT_VERSION_V1 UINT16_C(1)

/** Serialised size of the superblock header (excluding padding). */
#define SDB_SUPERBLOCK_HEADER_SIZE ((size_t)160)

/** Minimum supported logical page size, in bytes. */
#define SDB_MIN_PAGE_SIZE UINT32_C(4096)

/** Maximum supported logical page size, in bytes. */
#define SDB_MAX_PAGE_SIZE UINT32_C(65536)

/** Length of the KDF salt stored in the superblock. */
#define SDB_SALT_SIZE ((size_t)16)

/** Length of the per-database random identifier. */
#define SDB_FILE_ID_SIZE ((size_t)16)

/** On-disk size of one superblock slot (mirror). */
#define SDB_SUPERBLOCK_SLOT_SIZE ((size_t)4096)

/** Number of superblock mirrors kept on disk. */
#define SDB_SUPERBLOCK_SLOT_COUNT ((size_t)2)

/**
 * Superblock flag indicating that page contents are stored
 * encrypted (XChaCha20-Poly1305). Absence means plaintext.
 */
#define SDB_FLAG_ENCRYPTED UINT32_C(1)

/** Length of the wrapped data key held in the superblock. */
#define SDB_WRAPPED_KEY_SIZE ((size_t)32)

/** Length of the Poly1305 tag over the wrapped data key. */
#define SDB_KEY_WRAP_TAG_SIZE ((size_t)16)

/**
 * Lower bound for PBKDF2-HMAC-SHA256 iterations enforced on every
 * open, create, and rewrite. Files whose stored value falls below
 * this bound must be migrated via password rotation before they
 * can be opened by a current build. Matches the OWASP recommended
 * work factor as of 2026-07.
 */
#define SDB_MIN_KDF_ITERATIONS UINT32_C(600000)

/**
 * Default PBKDF2-HMAC-SHA256 iteration count applied to newly
 * created encrypted databases. Currently equal to
 * `SDB_MIN_KDF_ITERATIONS`.
 */
#define SDB_DEFAULT_KDF_ITERATIONS UINT32_C(600000)

/**
 * Upper bound for PBKDF2-HMAC-SHA256 iterations. Caps
 * denial-of-service via extreme work factors in adversarial
 * superblocks.
 */
#define SDB_MAX_KDF_ITERATIONS UINT32_C(10000000)

/**
 * The C ABI major version. Two builds sharing this value provide
 * mutually compatible library-consumer contracts for every symbol
 * declared with `SDB_API`.
 */
#define SDB_ABI_VERSION UINT32_C(1)

/** ShibaDB library major version. */
#define SDB_VERSION_MAJOR UINT32_C(1)
/** ShibaDB library minor version. */
#define SDB_VERSION_MINOR UINT32_C(0)
/** ShibaDB library patch version. */
#define SDB_VERSION_PATCH UINT32_C(0)
/** ShibaDB library version as a string literal, "MAJOR.MINOR.PATCH". */
#define SDB_VERSION_STRING "1.0.0"
/**
 * Numeric version packed as `MAJOR * 1000000 + MINOR * 1000 + PATCH`,
 * matching the pattern used by common embedded databases so callers
 * can compile-time gate on library version.
 */
#define SDB_VERSION_NUMBER UINT32_C(1000000)

/** @} */

/**
 * @brief Uniform status code returned by every ShibaDB API.
 *
 * Success is `SDB_OK` (0). Every other value indicates a failure.
 * Callers should compare against the named constants rather than
 * numeric literals; enum values are stable within a major ABI but
 * the ordering is not part of the contract.
 */
typedef enum sdb_status {
    /** Operation completed successfully. */
    SDB_OK = 0,
    /** A required argument was NULL, out of range, or malformed. */
    SDB_E_INVALID_ARGUMENT = 1,
    /** An output buffer was too small to hold the result. */
    SDB_E_BUFFER_TOO_SMALL = 2,
    /** A structural magic value did not match the expected token. */
    SDB_E_BAD_MAGIC = 3,
    /** An on-disk version code is newer than this library supports. */
    SDB_E_UNSUPPORTED_VERSION = 4,
    /**
     * On-disk data failed structural or cryptographic integrity
     * checks (CRC mismatch, aliasing, out-of-range fields, or
     * failed AEAD tag validation). The database should be treated
     * as compromised until restored from backup.
     */
    SDB_E_CORRUPT = 5,
    /** Arithmetic on caller-supplied sizes would overflow. */
    SDB_E_OVERFLOW = 6,
    /** An underlying syscall returned an error (see logs). */
    SDB_E_IO = 7,
    /** An internal invariant was violated; please file a bug. */
    SDB_E_INTERNAL = 8,
    /** A read returned fewer bytes than requested (short file). */
    SDB_E_TRUNCATED = 9,
    /**
     * A required file was not present. Distinct from `SDB_E_IO`
     * so callers such as WAL recovery can treat "no work" as
     * healthy while still propagating real I/O errors.
     */
    SDB_E_NOT_FOUND = 10,
    /**
     * A password/key check failed. In encrypted DBs this is the
     * return code for both a wrong password and a tampered key
     * wrap; the two are indistinguishable without side channels.
     */
    SDB_E_AUTHENTICATION = 11,
    /**
     * A write conflicted with a concurrent transaction and was
     * rolled back. Retry the transaction from scratch.
     */
    SDB_E_CONFLICT = 12,
    /**
     * The database is exclusively held by another process or
     * handle. Try again later.
     */
    SDB_E_BUSY = 13
} sdb_status;

/**
 * @brief On-disk superblock representation, format version 1.
 *
 * A single canonical instance is written into two mirrored slots
 * on disk. Every field is little-endian on the wire; consumers
 * that touch this struct in memory should use it verbatim without
 * byte-swapping.
 *
 * @note `next_page_id` is monotonic non-decreasing across
 *       successive updates — the update path refuses to lower it
 *       to prevent the pager's canonicalize step from truncating
 *       live pages off the tail of the file.
 * @note `page_size`, `salt`, and `file_id` are immutable after
 *       creation; the update path refuses to change them.
 */
typedef struct sdb_superblock_v1 {
    /** Logical page size in bytes. Between `SDB_MIN_PAGE_SIZE` and `SDB_MAX_PAGE_SIZE`. */
    uint32_t page_size;
    /** Bitfield of `SDB_FLAG_*` values. Currently only `SDB_FLAG_ENCRYPTED` is defined. */
    uint32_t flags;
    /** Monotonic update generation. Higher = newer. */
    uint64_t generation;
    /** LSN of the last WAL checkpoint durably applied. */
    uint64_t checkpoint_lsn;
    /** Page id of the top-level B-tree root, or 0 for empty. */
    uint64_t root_page;
    /** Head of the free-page list, or 0 if none. */
    uint64_t freelist_page;
    /** Low-water-mark for pages ever allocated. Monotonic. */
    uint64_t next_page_id;
    /** Identifier of the KDF/key-wrap scheme in use. */
    uint32_t key_wrap_id;
    /** PBKDF2 iteration count for password rotation. */
    uint32_t kdf_iterations;
    /** Random 16-byte KDF salt, fixed at creation time. */
    uint8_t salt[SDB_SALT_SIZE];
    /** Random 16-byte per-database identifier, fixed at creation time. */
    uint8_t file_id[SDB_FILE_ID_SIZE];
    /** Data-encryption key encrypted under the password-derived key. */
    uint8_t wrapped_key[SDB_WRAPPED_KEY_SIZE];
    /** Poly1305 tag authenticating the wrapped key + AAD. */
    uint8_t key_wrap_tag[SDB_KEY_WRAP_TAG_SIZE];
} sdb_superblock_v1;

/**
 * @brief Result of reading the two on-disk superblock mirrors.
 *
 * Reports which mirrors were valid and which the store selected
 * as the authoritative copy (highest generation, with the newer
 * mirror winning ties). If both mirrors have the same generation
 * but disagree on any other field, the read fails with
 * `SDB_E_CORRUPT`.
 */
typedef struct sdb_superblock_read_result {
    /** The selected superblock. */
    sdb_superblock_v1 superblock;
    /** Number of mirrors that passed structural validation (0, 1, or 2). */
    uint8_t valid_mirror_count;
    /** Index (0 or 1) of the mirror whose content was selected. */
    uint8_t selected_slot;
} sdb_superblock_read_result;

/**
 * @brief Return the ABI major version this binary implements.
 *
 * Guaranteed to equal `SDB_ABI_VERSION` at the time this
 * translation unit was compiled. Consumers loading ShibaDB
 * dynamically should call this and refuse to use the library
 * when it does not match the ABI they compiled against.
 *
 * @return `SDB_ABI_VERSION` as an unsigned integer.
 */
SDB_API uint32_t sdb_abi_version(void);

/**
 * @brief Return a static, human-readable library version string.
 *
 * @return NUL-terminated string owned by the library. The pointer
 *         remains valid for the lifetime of the process.
 */
SDB_API const char *sdb_version_string(void);

/**
 * @brief Return the runtime library version packed as an integer.
 *
 * Layout: `MAJOR * 1000000 + MINOR * 1000 + PATCH`. Callers loading
 * ShibaDB dynamically can compare against `SDB_VERSION_NUMBER` at
 * compile time to accept only a compatible library.
 */
SDB_API uint32_t sdb_version_number(void);

/**
 * @brief Return a static description of a status code.
 *
 * Suitable for log lines and error messages. Unknown values map
 * to the literal string `"unknown status"`.
 *
 * @param status Any `sdb_status` value.
 * @return NUL-terminated string owned by the library.
 */
SDB_API const char *sdb_status_string(sdb_status status);

/**
 * @brief Serialise a superblock into a fixed-size byte buffer.
 *
 * The output layout is the on-disk mirror slot format: header +
 * zero padding, all little-endian. This function does not touch
 * the filesystem.
 *
 * @param superblock Non-NULL pointer to a fully-populated struct.
 * @param output     Non-NULL, at least `output_size` bytes.
 * @param output_size Exactly `SDB_SUPERBLOCK_SLOT_SIZE`.
 * @retval SDB_OK on success.
 * @retval SDB_E_INVALID_ARGUMENT for NULL pointers, wrong
 *         output_size, or out-of-range struct fields.
 */
SDB_API sdb_status sdb_superblock_v1_encode(
    const sdb_superblock_v1 *superblock,
    uint8_t *output,
    size_t output_size
);

/**
 * @brief Parse a superblock from a byte buffer.
 *
 * Validates magic, version, CRC, field ranges, and trailing zero
 * padding. On failure, `*superblock_out` is left untouched.
 *
 * @param input       Non-NULL buffer to parse.
 * @param input_size  Exactly `SDB_SUPERBLOCK_SLOT_SIZE`.
 * @param superblock_out Non-NULL, receives the parsed struct on
 *                       success.
 * @retval SDB_OK on success.
 * @retval SDB_E_INVALID_ARGUMENT for NULL pointers or wrong size.
 * @retval SDB_E_CORRUPT on any structural or CRC failure.
 * @retval SDB_E_BAD_MAGIC when the magic bytes do not match.
 * @retval SDB_E_UNSUPPORTED_VERSION when the version field is
 *         newer than this library implements.
 */
SDB_API sdb_status sdb_superblock_v1_decode(
    const uint8_t *input,
    size_t input_size,
    sdb_superblock_v1 *superblock_out
);

/**
 * @brief Atomically create a fresh database file with the given
 *        initial superblock.
 *
 * Writes both mirror slots and fsyncs them, then fsyncs the
 * parent directory so the mirror pair reaches durable storage
 * before this call returns. Fails if `path` already exists.
 *
 * @param path       Non-NULL, valid path in a writable directory.
 * @param superblock Non-NULL initial superblock.
 * @retval SDB_OK on success.
 * @retval SDB_E_IO if a syscall failed (see errno-level logs).
 * @retval SDB_E_INVALID_ARGUMENT for NULL/bad arguments.
 */
SDB_API sdb_status sdb_superblock_store_create(
    const char *path,
    const sdb_superblock_v1 *superblock
);

/**
 * @brief Read both superblock mirrors and select the newer one.
 *
 * The higher-generation mirror wins. If both mirrors have the
 * same generation but disagree on any other field, this call
 * fails with `SDB_E_CORRUPT` — a split-brain guard.
 *
 * @param path       Non-NULL path to an existing database file.
 * @param result_out Non-NULL, receives the parsed result on
 *                   success.
 * @retval SDB_OK on success.
 * @retval SDB_E_CORRUPT if both mirrors are unreadable, both
 *         mirrors disagree at equal generation, or a decoded
 *         mirror fails validation.
 * @retval SDB_E_NOT_FOUND if the file does not exist.
 * @retval SDB_E_IO on other syscall failure.
 */
SDB_API sdb_status sdb_superblock_store_read(
    const char *path,
    sdb_superblock_read_result *result_out
);

/**
 * @brief Atomically publish a new superblock generation.
 *
 * Writes the update to the older mirror first (with fsync), then
 * to the newer mirror (with fsync). A crash at any point leaves
 * one valid mirror behind for recovery.
 *
 * @param path       Non-NULL path to an existing database file.
 * @param superblock Non-NULL new superblock. Must have strictly
 *                   greater `generation` than the current
 *                   on-disk value. `file_id`, `salt`, and
 *                   `page_size` must be unchanged; `next_page_id`
 *                   must be non-decreasing (retreat is rejected
 *                   to prevent data-loss on canonicalize).
 * @retval SDB_OK on success.
 * @retval SDB_E_INVALID_ARGUMENT if any of the above invariants
 *         is violated. The database is left untouched.
 * @retval SDB_E_IO on syscall failure.
 */
SDB_API sdb_status sdb_superblock_store_update(
    const char *path,
    const sdb_superblock_v1 *superblock
);

#ifdef __cplusplus
}
#endif

#endif
