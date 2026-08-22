/*
 * shibadb — command-line interface for the ShibaDB embedded database.
 *
 * A thin, portable front-end over the public engine API (shibadb_engine.h).
 * It links the same library an application would and performs no I/O of its
 * own beyond argv/stdin/stdout, so its behaviour is exactly what an embedder
 * sees. Values supplied on the command line are treated as raw bytes; values
 * printed to stdout are written verbatim (binary-safe) with no added newline
 * unless noted.
 *
 * Password handling: prefer the SHIBADB_PASSWORD environment variable over
 * --password, which is visible in the process list on most systems. A
 * password of length zero (unset / empty) opens the database in plaintext
 * mode, matching sdb_database_* semantics.
 */

#include <shibadb.h>
#include <shibadb_engine.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Shared option parsing                                              */
/* ------------------------------------------------------------------ */

typedef struct cli_options {
    const char *password;   /* NULL when none supplied */
    uint32_t page_size;     /* 0 = engine default */
    uint32_t kdf_iterations;/* 0 = engine default */
    const char *prefix;     /* scan prefix, NULL = whole namespace */
    uint64_t limit;         /* 0 = unlimited */
    uint64_t synchronous;   /* SDB_SYNCHRONOUS_FULL (0) or _NORMAL (1) */
    bool reverse;           /* scan: descending key order */
    bool force;             /* backup: replace existing destination */
    bool unique;            /* mkindex: create a unique index */
    const char *index_specs[16]; /* docput: repeated --index name=value */
    size_t index_count;
} cli_options;

static void cli_options_init(cli_options *opts) {
    opts->password = NULL;
    opts->page_size = 0U;
    opts->kdf_iterations = 0U;
    opts->prefix = NULL;
    opts->limit = 0U;
    opts->synchronous = 0U;
    opts->reverse = false;
    opts->force = false;
    opts->unique = false;
    opts->index_count = 0U;
}

/*
 * Pull recognised --flags out of argv in place, leaving only positional
 * arguments in [out_argv, *out_argc). Returns 0 on success, -1 on a malformed
 * flag (message already printed). Unknown --flags are an error so typos fail
 * loudly rather than being silently ignored as positionals.
 */
static int cli_parse_options(int argc, char **argv, cli_options *opts,
                             char **out_argv, int *out_argc) {
    int positional = 0;
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--password") == 0 || strcmp(arg, "-p") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "shibadb: %s requires a value\n", arg);
                return -1;
            }
            opts->password = argv[++i];
        } else if (strcmp(arg, "--prefix") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "shibadb: --prefix requires a value\n");
                return -1;
            }
            opts->prefix = argv[++i];
        } else if (strcmp(arg, "--page-size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "shibadb: --page-size requires a value\n");
                return -1;
            }
            opts->page_size = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--kdf-iterations") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "shibadb: --kdf-iterations requires a value\n");
                return -1;
            }
            opts->kdf_iterations = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--limit") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "shibadb: --limit requires a value\n");
                return -1;
            }
            opts->limit = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(arg, "--synchronous") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "shibadb: --synchronous requires a value\n");
                return -1;
            }
            const char *mode = argv[++i];
            if (strcmp(mode, "full") == 0) {
                opts->synchronous = SDB_SYNCHRONOUS_FULL;
            } else if (strcmp(mode, "normal") == 0) {
                opts->synchronous = SDB_SYNCHRONOUS_NORMAL;
            } else {
                fprintf(stderr,
                        "shibadb: --synchronous must be 'full' or 'normal'\n");
                return -1;
            }
        } else if (strcmp(arg, "--reverse") == 0) {
            opts->reverse = true;
        } else if (strcmp(arg, "--force") == 0 || strcmp(arg, "-f") == 0) {
            opts->force = true;
        } else if (strcmp(arg, "--unique") == 0) {
            opts->unique = true;
        } else if (strcmp(arg, "--index") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "shibadb: --index requires name=value\n");
                return -1;
            }
            if (opts->index_count >= 16U) {
                fprintf(stderr, "shibadb: too many --index terms (max 16)\n");
                return -1;
            }
            opts->index_specs[opts->index_count++] = argv[++i];
        } else if (strncmp(arg, "--", 2) == 0 && arg[2] != '\0') {
            fprintf(stderr, "shibadb: unknown option '%s'\n", arg);
            return -1;
        } else {
            out_argv[positional++] = argv[i];
        }
    }
    *out_argc = positional;
    return 0;
}

/*
 * Resolve the effective password: explicit --password wins, else the
 * SHIBADB_PASSWORD environment variable, else none. The returned pointer is
 * owned elsewhere (argv or the environment) and stays valid for the process.
 */
static const char *resolve_password(const cli_options *opts) {
    if (opts->password != NULL) {
        return opts->password;
    }
    return getenv("SHIBADB_PASSWORD");
}

static void fill_db_options(sdb_database_options *db_opts,
                            const cli_options *opts) {
    sdb_database_options_init(db_opts);
    const char *pw = resolve_password(opts);
    if (pw != NULL && pw[0] != '\0') {
        db_opts->password = (const uint8_t *)pw;
        db_opts->password_size = strlen(pw);
    }
    if (opts->page_size != 0U) {
        db_opts->page_size = opts->page_size;
    }
    if (opts->kdf_iterations != 0U) {
        db_opts->kdf_iterations = opts->kdf_iterations;
    }
    db_opts->synchronous = opts->synchronous;
}

/* Map an sdb_status to a small non-zero process exit code (2..) . */
static int exit_code_for(sdb_status status) {
    if (status == SDB_OK) {
        return 0;
    }
    return 2;
}

static int fail(const char *context, sdb_status status) {
    fprintf(stderr, "shibadb: %s: %s (%d)\n", context,
            sdb_status_string(status), (int)status);
    return exit_code_for(status);
}

/* ------------------------------------------------------------------ */
/* Commands                                                           */
/* ------------------------------------------------------------------ */

static int cmd_version(void) {
    printf("shibadb %s (ABI %" PRIu32 ", engine API %" PRIu32
           ", format v%u)\n",
           sdb_version_string(), sdb_abi_version(),
           SDB_ENGINE_API_VERSION, (unsigned)SDB_FORMAT_VERSION_CURRENT);
    return 0;
}

/*
 * info reads the mirrored superblock directly, so it works on encrypted
 * databases WITHOUT the password and never opens the engine (no WAL replay,
 * no locking). It is the safe first thing to run against an unknown file.
 */
static int cmd_info(const char *path) {
    sdb_superblock_read_result res;
    memset(&res, 0, sizeof(res));
    sdb_status st = sdb_superblock_store_read(path, &res);
    if (st != SDB_OK) {
        return fail("info", st);
    }
    const sdb_superblock_v1 *sb = &res.superblock;
    bool encrypted = (sb->flags & SDB_FLAG_ENCRYPTED) != 0U;
    printf("path:           %s\n", path);
    printf("format_version: %u\n", (unsigned)res.format_version);
    printf("page_size:      %" PRIu32 "\n", sb->page_size);
    printf("encrypted:      %s\n", encrypted ? "yes" : "no");
    printf("header_auth:    %s\n",
           (sb->flags & SDB_FLAG_HEADER_AUTH) != 0U ? "yes" : "no");
    printf("generation:     %" PRIu64 "\n", sb->generation);
    printf("checkpoint_lsn: %" PRIu64 "\n", sb->checkpoint_lsn);
    printf("next_page_id:   %" PRIu64 "\n", sb->next_page_id);
    if (encrypted) {
        printf("kdf_iterations: %" PRIu32 "\n", sb->kdf_iterations);
    }
    printf("valid_mirrors:  %u of %u\n", (unsigned)res.valid_mirror_count,
           (unsigned)SDB_SUPERBLOCK_SLOT_COUNT);
    return 0;
}

static int cmd_create(const char *path, const cli_options *opts) {
    sdb_database_options db_opts;
    fill_db_options(&db_opts, opts);
    sdb_database *db = NULL;
    sdb_status st = sdb_database_create(path, &db_opts, &db);
    if (st != SDB_OK) {
        return fail("create", st);
    }
    st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("create (close)", st);
    }
    printf("created %s\n", path);
    return 0;
}

/* Open helper shared by mutating/reading commands. */
static sdb_status open_db(const char *path, const cli_options *opts,
                          sdb_database **db_out) {
    sdb_database_options db_opts;
    fill_db_options(&db_opts, opts);
    return sdb_database_open(path, &db_opts, db_out);
}

static int cmd_put(const char *path, const char *ns, const char *key,
                   const char *value, const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    st = sdb_kv_put(db, (const uint8_t *)ns, strlen(ns),
                    (const uint8_t *)key, strlen(key),
                    (const uint8_t *)value, strlen(value));
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("put", st);
    }
    if (close_st != SDB_OK) {
        return fail("put (close)", close_st);
    }
    return 0;
}

static int cmd_get(const char *path, const char *ns, const char *key,
                   const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    /* Probe size first, then allocate exactly. */
    size_t needed = 0;
    st = sdb_kv_get(db, (const uint8_t *)ns, strlen(ns),
                    (const uint8_t *)key, strlen(key), NULL, 0, &needed);
    if (st != SDB_OK && st != SDB_E_BUFFER_TOO_SMALL) {
        (void)sdb_database_close(db);
        return fail("get", st);
    }
    uint8_t *buf = NULL;
    if (needed > 0) {
        buf = (uint8_t *)malloc(needed);
        if (buf == NULL) {
            (void)sdb_database_close(db);
            fprintf(stderr, "shibadb: get: out of memory\n");
            return 2;
        }
    }
    size_t got = 0;
    st = sdb_kv_get(db, (const uint8_t *)ns, strlen(ns),
                    (const uint8_t *)key, strlen(key), buf, needed, &got);
    if (st == SDB_OK && got > 0) {
        fwrite(buf, 1, got, stdout);
    }
    free(buf);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("get", st);
    }
    if (close_st != SDB_OK) {
        return fail("get (close)", close_st);
    }
    return 0;
}

static int cmd_del(const char *path, const char *ns, const char *key,
                   const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    st = sdb_kv_delete(db, (const uint8_t *)ns, strlen(ns),
                       (const uint8_t *)key, strlen(key));
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("del", st);
    }
    if (close_st != SDB_OK) {
        return fail("del (close)", close_st);
    }
    return 0;
}

/* Context + visitor for `scan`. Prints key<TAB>value<LF> per entry. */
typedef struct scan_ctx {
    uint64_t seen;
} scan_ctx;

static bool scan_visitor(void *context, const uint8_t *key, size_t key_size,
                         const uint8_t *value, size_t value_size) {
    scan_ctx *ctx = (scan_ctx *)context;
    ctx->seen++;
    fwrite(key, 1, key_size, stdout);
    fputc('\t', stdout);
    fwrite(value, 1, value_size, stdout);
    fputc('\n', stdout);
    return true;
}

static int cmd_scan(const char *path, const char *ns, const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    sdb_scan_options scan_opts;
    sdb_scan_options_init(&scan_opts);
    scan_opts.limit = opts->limit;
    scan_opts.reverse = opts->reverse;

    const uint8_t *prefix = NULL;
    size_t prefix_size = 0;
    if (opts->prefix != NULL) {
        prefix = (const uint8_t *)opts->prefix;
        prefix_size = strlen(opts->prefix);
    }
    scan_ctx ctx;
    ctx.seen = 0;
    size_t matched = 0;
    st = sdb_kv_scan_prefix(db, (const uint8_t *)ns, strlen(ns),
                            prefix, prefix_size, &scan_opts,
                            scan_visitor, &ctx, &matched);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("scan", st);
    }
    if (close_st != SDB_OK) {
        return fail("scan (close)", close_st);
    }
    fprintf(stderr, "shibadb: %" PRIu64 " entries\n", ctx.seen);
    return 0;
}

/* Context + visitor for `namespaces`. */
static bool namespace_visitor(void *context, uint16_t keyspace_kind,
                              const uint8_t *namespace_name,
                              size_t namespace_size) {
    (void)context;
    const char *kind = "?";
    switch (keyspace_kind) {
        case 1: kind = "kv"; break;
        case 2: kind = "blob"; break;
        case 3: kind = "document"; break;
        default: break;
    }
    printf("%-9s ", kind);
    fwrite(namespace_name, 1, namespace_size, stdout);
    fputc('\n', stdout);
    return true;
}

static int cmd_namespaces(const char *path, const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    size_t count = 0;
    st = sdb_list_namespaces(db, namespace_visitor, NULL, &count);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("namespaces", st);
    }
    if (close_st != SDB_OK) {
        return fail("namespaces (close)", close_st);
    }
    fprintf(stderr, "shibadb: %zu namespaces\n", count);
    return 0;
}

static int cmd_verify(const char *path, const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    sdb_verify_result r;
    memset(&r, 0, sizeof(r));
    st = sdb_database_verify(db, &r);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("verify", st);
    }
    if (close_st != SDB_OK) {
        return fail("verify (close)", close_st);
    }
    printf("allocated_pages: %" PRIu64 "\n", r.allocated_page_count);
    printf("free_pages:      %" PRIu64 "\n", r.free_page_count);
    printf("btree_nodes:     %" PRIu64 "\n", r.btree_node_count);
    printf("btree_leaves:    %" PRIu64 "\n", r.btree_leaf_count);
    printf("btree_height:    %" PRIu32 "\n", r.btree_height);
    printf("raw_entries:     %" PRIu64 "\n", r.raw_entry_count);
    printf("objects:         %" PRIu64 "\n", r.object_count);
    printf("live_chunks:     %" PRIu64 "\n", r.live_chunk_count);
    printf("stale_entries:   %" PRIu64 "\n", r.stale_entry_count);
    printf("logical_bytes:   %" PRIu64 "\n", r.logical_byte_count);
    printf("verify: OK\n");
    return 0;
}

/*
 * health opens the engine (unlike info, which only reads the superblock file)
 * and reports the O(1) configuration snapshot from sdb_database_info. It is the
 * "does this database open cleanly, and what is it?" probe: a non-zero exit
 * means open/recovery failed.
 */
static int cmd_health(const char *path, const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    sdb_info_result info;
    memset(&info, 0, sizeof(info));
    st = sdb_database_info(db, &info);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("health", st);
    }
    if (close_st != SDB_OK) {
        return fail("health (close)", close_st);
    }
    printf("page_size:      %" PRIu32 "\n", info.page_size);
    printf("encrypted:      %s\n", info.encrypted ? "yes" : "no");
    if (info.encrypted) {
        printf("kdf_iterations: %" PRIu32 "\n", info.kdf_iterations);
    }
    printf("generation:     %" PRIu64 "\n", info.generation);
    printf("checkpoint_lsn: %" PRIu64 "\n", info.checkpoint_lsn);
    printf("page_count:     %" PRIu64 "\n", info.page_count);
    printf("health: OK\n");
    return 0;
}

static int cmd_exists(const char *path, const char *ns, const char *key,
                      const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    bool present = false;
    st = sdb_kv_exists(db, (const uint8_t *)ns, strlen(ns),
                       (const uint8_t *)key, strlen(key), &present);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("exists", st);
    }
    if (close_st != SDB_OK) {
        return fail("exists (close)", close_st);
    }
    printf("%s\n", present ? "yes" : "no");
    return present ? 0 : 3; /* 3 = absent (distinct from usage/error codes) */
}

static int cmd_count(const char *path, const char *ns,
                     const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    const uint8_t *prefix = NULL;
    size_t prefix_size = 0;
    if (opts->prefix != NULL) {
        prefix = (const uint8_t *)opts->prefix;
        prefix_size = strlen(opts->prefix);
    }
    uint64_t count = 0;
    st = sdb_kv_count_prefix(db, (const uint8_t *)ns, strlen(ns),
                             prefix, prefix_size, &count);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("count", st);
    }
    if (close_st != SDB_OK) {
        return fail("count (close)", close_st);
    }
    printf("%" PRIu64 "\n", count);
    return 0;
}

static int cmd_incr(const char *path, const char *ns, const char *key,
                    int64_t delta, const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    int64_t new_value = 0;
    st = sdb_kv_increment(db, (const uint8_t *)ns, strlen(ns),
                          (const uint8_t *)key, strlen(key), delta, &new_value);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("incr", st);
    }
    if (close_st != SDB_OK) {
        return fail("incr (close)", close_st);
    }
    printf("%" PRId64 "\n", new_value);
    return 0;
}

static int cmd_mkindex(const char *path, const char *collection,
                       const char *index, const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    st = sdb_index_create(db, (const uint8_t *)collection, strlen(collection),
                          (const uint8_t *)index, strlen(index), opts->unique);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("mkindex", st);
    }
    if (close_st != SDB_OK) {
        return fail("mkindex (close)", close_st);
    }
    printf("index '%s' created on '%s'%s\n", index, collection,
           opts->unique ? " (unique)" : "");
    return 0;
}

static int cmd_docput(const char *path, const char *collection, const char *id,
                      const char *json, const cli_options *opts) {
    sdb_index_term terms[16];
    size_t i;
    sdb_database *db = NULL;
    sdb_status st;
    sdb_status close_st;
    for (i = 0; i < opts->index_count; ++i) {
        const char *spec = opts->index_specs[i];
        const char *eq = strchr(spec, '=');
        if (eq == NULL) {
            fprintf(stderr, "shibadb: --index expects name=value, got '%s'\n",
                    spec);
            return 1;
        }
        terms[i].index_name = (const uint8_t *)spec;
        terms[i].index_name_size = (size_t)(eq - spec);
        terms[i].value = (const uint8_t *)(eq + 1);
        terms[i].value_size = strlen(eq + 1);
    }
    st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    st = sdb_document_put(db, (const uint8_t *)collection, strlen(collection),
                          (const uint8_t *)id, strlen(id),
                          (const uint8_t *)json, strlen(json),
                          opts->index_count > 0U ? terms : NULL,
                          opts->index_count);
    close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("docput", st);
    }
    if (close_st != SDB_OK) {
        return fail("docput (close)", close_st);
    }
    return 0;
}

static int cmd_docget(const char *path, const char *collection, const char *id,
                      const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    size_t needed = 0;
    st = sdb_document_get(db, (const uint8_t *)collection, strlen(collection),
                          (const uint8_t *)id, strlen(id), NULL, 0, &needed);
    if (st != SDB_OK && st != SDB_E_BUFFER_TOO_SMALL) {
        (void)sdb_database_close(db);
        return fail("docget", st);
    }
    uint8_t *buf = NULL;
    if (needed > 0) {
        buf = (uint8_t *)malloc(needed);
        if (buf == NULL) {
            (void)sdb_database_close(db);
            fprintf(stderr, "shibadb: docget: out of memory\n");
            return 2;
        }
    }
    size_t got = 0;
    st = sdb_document_get(db, (const uint8_t *)collection, strlen(collection),
                          (const uint8_t *)id, strlen(id), buf, needed, &got);
    if (st == SDB_OK && got > 0) {
        fwrite(buf, 1, got, stdout);
        fputc('\n', stdout);
    }
    free(buf);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("docget", st);
    }
    if (close_st != SDB_OK) {
        return fail("docget (close)", close_st);
    }
    return 0;
}

static int cmd_docdel(const char *path, const char *collection, const char *id,
                      const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    st = sdb_document_delete(db, (const uint8_t *)collection, strlen(collection),
                             (const uint8_t *)id, strlen(id));
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("docdel", st);
    }
    if (close_st != SDB_OK) {
        return fail("docdel (close)", close_st);
    }
    return 0;
}

static bool cli_find_visitor(void *context, const uint8_t *document_id,
                             size_t document_id_size, const uint8_t *document,
                             size_t document_size) {
    (void)context;
    fwrite(document_id, 1, document_id_size, stdout);
    fputc('\t', stdout);
    fwrite(document, 1, document_size, stdout);
    fputc('\n', stdout);
    return true;
}

static int cmd_find(const char *path, const char *collection, const char *index,
                    const char *value, const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    size_t matched = 0;
    st = sdb_index_query_documents(
        db, (const uint8_t *)collection, strlen(collection),
        (const uint8_t *)index, strlen(index),
        (const uint8_t *)value, strlen(value),
        cli_find_visitor, NULL, &matched);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("find", st);
    }
    if (close_st != SDB_OK) {
        return fail("find (close)", close_st);
    }
    fprintf(stderr, "shibadb: %zu documents\n", matched);
    return 0;
}

static int cmd_backup(const char *path, const char *dest,
                      const cli_options *opts) {
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    sdb_backup_result r;
    memset(&r, 0, sizeof(r));
    st = sdb_database_backup(db, dest, opts->force, &r);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("backup", st);
    }
    if (close_st != SDB_OK) {
        return fail("backup (close)", close_st);
    }
    printf("backup: %s -> %s (%" PRIu64 " bytes, %" PRIu64 " entries)\n",
           path, dest, r.byte_count, r.raw_entry_count);
    return 0;
}

static int cmd_compact(const char *path, const cli_options *opts) {
    /*
     * compact rewrites the database, so it needs a target_options describing
     * the OUTPUT: the same password (mismatching the source's encryption
     * state is rejected with SDB_E_INVALID_ARGUMENT) and, by default, the
     * same page size. Read the current page size from the superblock (no
     * password required) and preserve it unless --page-size overrides.
     */
    uint32_t current_page_size = 0U;
    sdb_superblock_read_result sb;
    memset(&sb, 0, sizeof(sb));
    if (sdb_superblock_store_read(path, &sb) == SDB_OK) {
        current_page_size = sb.superblock.page_size;
    }
    sdb_database *db = NULL;
    sdb_status st = open_db(path, opts, &db);
    if (st != SDB_OK) {
        return fail("open", st);
    }
    sdb_database_options target;
    fill_db_options(&target, opts);
    if (opts->page_size == 0U && current_page_size != 0U) {
        target.page_size = current_page_size;
    }
    sdb_compact_result r;
    memset(&r, 0, sizeof(r));
    st = sdb_database_compact(db, &target, &r);
    sdb_status close_st = sdb_database_close(db);
    if (st != SDB_OK) {
        return fail("compact", st);
    }
    if (close_st != SDB_OK) {
        return fail("compact (close)", close_st);
    }
    printf("compact: %" PRIu64 " -> %" PRIu64 " bytes, "
           "%" PRIu64 " -> %" PRIu64 " entries\n",
           r.byte_count_before, r.byte_count_after,
           r.raw_entries_before, r.raw_entries_after);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

static int usage(FILE *out) {
    fprintf(out,
"shibadb %s — embedded ACID key-value database CLI\n"
"\n"
"Usage: shibadb <command> [args] [options]\n"
"\n"
"Commands:\n"
"  version                         Print version and ABI info\n"
"  info       <file>               Show superblock (no password needed)\n"
"  create     <file>               Create an empty database\n"
"  put        <file> <ns> <k> <v>  Store a KV pair\n"
"  get        <file> <ns> <k>      Fetch a value (raw bytes to stdout)\n"
"  del        <file> <ns> <k>      Delete a key\n"
"  scan       <file> <ns>          Print key<TAB>value lines (--reverse)\n"
"  namespaces <file>               List populated (kind, namespace) pairs\n"
"  verify     <file>               Deep structural verification\n"
"  health     <file>               Open the engine and print a config snapshot\n"
"  exists     <file> <ns> <k>      Print yes/no (exit 3 if the key is absent)\n"
"  count      <file> <ns>          Count keys in a namespace (honours --prefix)\n"
"  incr       <file> <ns> <k> [n]  Atomically add n (default 1) to a counter\n"
"\n"
"Documents:\n"
"  mkindex    <file> <coll> <name>       Create a secondary index (--unique)\n"
"  docput     <file> <coll> <id> <json>  Store a document (repeat --index n=v)\n"
"  docget     <file> <coll> <id>         Print a document body\n"
"  docdel     <file> <coll> <id>         Delete a document\n"
"  find       <file> <coll> <idx> <val>  Print id<TAB>doc for index matches\n"
"  backup     <file> <dest>        Atomic snapshot to a new file\n"
"  compact    <file>               Reclaim stale space in place\n"
"\n"
"Options:\n"
"  -p, --password <pw>   Database password (or set SHIBADB_PASSWORD)\n"
"      --page-size <n>   Page size for create (bytes)\n"
"      --kdf-iterations <n>  PBKDF2 iterations for create\n"
"      --prefix <p>      Restrict scan to keys with this prefix\n"
"      --limit <n>       Max entries for scan (0 = all)\n"
"      --reverse         scan: descending key order (with --limit, the\n"
"                        greatest n keys)\n"
"      --synchronous <m> Durability of auto-commits: full (default) or\n"
"                        normal (faster; may lose the newest commits on\n"
"                        power loss, never corrupts)\n"
"  -f, --force           backup: overwrite an existing destination\n",
    sdb_version_string());
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) {
        return usage(stdout);
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        return cmd_version();
    }

    /* Separate positionals from options for the remaining argv (post-cmd). */
    cli_options opts;
    cli_options_init(&opts);
    int rest_argc = argc - 2;
    char **pos = NULL;
    if (rest_argc > 0) {
        pos = (char **)malloc((size_t)rest_argc * sizeof(char *));
        if (pos == NULL) {
            fprintf(stderr, "shibadb: out of memory\n");
            return 1;
        }
    }
    int npos = 0;
    if (rest_argc > 0 &&
        cli_parse_options(rest_argc, &argv[2], &opts, pos, &npos) != 0) {
        free(pos);
        return 1;
    }

    int rc;
    if (strcmp(cmd, "info") == 0) {
        if (npos != 1) { fprintf(stderr, "shibadb: info <file>\n"); rc = 1; }
        else { rc = cmd_info(pos[0]); }
    } else if (strcmp(cmd, "create") == 0) {
        if (npos != 1) { fprintf(stderr, "shibadb: create <file>\n"); rc = 1; }
        else { rc = cmd_create(pos[0], &opts); }
    } else if (strcmp(cmd, "put") == 0) {
        if (npos != 4) { fprintf(stderr, "shibadb: put <file> <ns> <k> <v>\n"); rc = 1; }
        else { rc = cmd_put(pos[0], pos[1], pos[2], pos[3], &opts); }
    } else if (strcmp(cmd, "get") == 0) {
        if (npos != 3) { fprintf(stderr, "shibadb: get <file> <ns> <k>\n"); rc = 1; }
        else { rc = cmd_get(pos[0], pos[1], pos[2], &opts); }
    } else if (strcmp(cmd, "del") == 0) {
        if (npos != 3) { fprintf(stderr, "shibadb: del <file> <ns> <k>\n"); rc = 1; }
        else { rc = cmd_del(pos[0], pos[1], pos[2], &opts); }
    } else if (strcmp(cmd, "scan") == 0) {
        if (npos != 2) { fprintf(stderr, "shibadb: scan <file> <ns>\n"); rc = 1; }
        else { rc = cmd_scan(pos[0], pos[1], &opts); }
    } else if (strcmp(cmd, "namespaces") == 0) {
        if (npos != 1) { fprintf(stderr, "shibadb: namespaces <file>\n"); rc = 1; }
        else { rc = cmd_namespaces(pos[0], &opts); }
    } else if (strcmp(cmd, "verify") == 0) {
        if (npos != 1) { fprintf(stderr, "shibadb: verify <file>\n"); rc = 1; }
        else { rc = cmd_verify(pos[0], &opts); }
    } else if (strcmp(cmd, "health") == 0) {
        if (npos != 1) { fprintf(stderr, "shibadb: health <file>\n"); rc = 1; }
        else { rc = cmd_health(pos[0], &opts); }
    } else if (strcmp(cmd, "exists") == 0) {
        if (npos != 3) { fprintf(stderr, "shibadb: exists <file> <ns> <k>\n"); rc = 1; }
        else { rc = cmd_exists(pos[0], pos[1], pos[2], &opts); }
    } else if (strcmp(cmd, "count") == 0) {
        if (npos != 2) { fprintf(stderr, "shibadb: count <file> <ns>\n"); rc = 1; }
        else { rc = cmd_count(pos[0], pos[1], &opts); }
    } else if (strcmp(cmd, "incr") == 0) {
        if (npos != 3 && npos != 4) { fprintf(stderr, "shibadb: incr <file> <ns> <k> [delta]\n"); rc = 1; }
        else {
            int64_t delta = (npos == 4) ? (int64_t)strtoll(pos[3], NULL, 10) : 1;
            rc = cmd_incr(pos[0], pos[1], pos[2], delta, &opts);
        }
    } else if (strcmp(cmd, "mkindex") == 0) {
        if (npos != 3) { fprintf(stderr, "shibadb: mkindex <file> <coll> <name>\n"); rc = 1; }
        else { rc = cmd_mkindex(pos[0], pos[1], pos[2], &opts); }
    } else if (strcmp(cmd, "docput") == 0) {
        if (npos != 4) { fprintf(stderr, "shibadb: docput <file> <coll> <id> <json>\n"); rc = 1; }
        else { rc = cmd_docput(pos[0], pos[1], pos[2], pos[3], &opts); }
    } else if (strcmp(cmd, "docget") == 0) {
        if (npos != 3) { fprintf(stderr, "shibadb: docget <file> <coll> <id>\n"); rc = 1; }
        else { rc = cmd_docget(pos[0], pos[1], pos[2], &opts); }
    } else if (strcmp(cmd, "docdel") == 0) {
        if (npos != 3) { fprintf(stderr, "shibadb: docdel <file> <coll> <id>\n"); rc = 1; }
        else { rc = cmd_docdel(pos[0], pos[1], pos[2], &opts); }
    } else if (strcmp(cmd, "find") == 0) {
        if (npos != 4) { fprintf(stderr, "shibadb: find <file> <coll> <idx> <val>\n"); rc = 1; }
        else { rc = cmd_find(pos[0], pos[1], pos[2], pos[3], &opts); }
    } else if (strcmp(cmd, "backup") == 0) {
        if (npos != 2) { fprintf(stderr, "shibadb: backup <file> <dest>\n"); rc = 1; }
        else { rc = cmd_backup(pos[0], pos[1], &opts); }
    } else if (strcmp(cmd, "compact") == 0) {
        if (npos != 1) { fprintf(stderr, "shibadb: compact <file>\n"); rc = 1; }
        else { rc = cmd_compact(pos[0], &opts); }
    } else {
        fprintf(stderr, "shibadb: unknown command '%s'\n", cmd);
        usage(stderr);
        rc = 1;
    }

    free(pos);
    return rc;
}
