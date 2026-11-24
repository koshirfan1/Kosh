#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <sqlite3.h>
#include "sqlite_lib.h"

/*
 * Real SQLite bridge for Kosh, replacing a previous mock (fn_sqlite_exec
 * used to just return true without touching the SQL string or storing
 * anything - found and fixed in this session, see chat).
 *
 * Links against real libsqlite3 (libsqlite3-dev). Same handle-table
 * pattern as ocr_lib.c/llm_lib.c: a Kosh-level map carries a slot index,
 * the real sqlite3* lives in a native-side table.
 */

#define SQLITE_MAX_HANDLES 8
/* Wait this long for another connection's lock instead of failing at once
 * with "database is locked". Change per connection with sqlite_busy_timeout(). */
#define SQLITE_DEFAULT_BUSY_MS 5000
/* Kosh numbers are doubles: integers beyond +/-2^53 cannot be held exactly. */
#define KOSH_MAX_EXACT_INT 9007199254740992.0
static sqlite3 *g_db_handles[SQLITE_MAX_HANDLES];

static int sqlite_alloc_slot(void) {
    for (int i = 0; i < SQLITE_MAX_HANDLES; i++) if (!g_db_handles[i]) return i;
    return -1;
}

static int sqlite_resolve_slot(Value *m) {
    if (m->type != VAL_MAP) return -1;
    Value *idv = map_get(m, "__sqlite_id");
    if (!idv || idv->type != VAL_NUMBER) return -1;
    int slot = (int)idv->as.number;
    if (slot < 0 || slot >= SQLITE_MAX_HANDLES || !g_db_handles[slot]) return -1;
    return slot;
}

static void fn_sqlite_open(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc != 1 || args[0].type != VAL_STRING) return;

    int slot = sqlite_alloc_slot();
    if (slot < 0) {
        fprintf(stderr, "Runtime error: sqlite_open() - too many open connections (max %d)\n", SQLITE_MAX_HANDLES);
        return;
    }

    sqlite3 *db;
    if (sqlite3_open(args[0].as.string, &db) != SQLITE_OK) {
        fprintf(stderr, "Runtime error: sqlite_open(\"%s\") - %s\n", args[0].as.string, sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    sqlite3_busy_timeout(db, SQLITE_DEFAULT_BUSY_MS);
    g_db_handles[slot] = db;
    Value m = value_map_empty();
    map_set(&m, "__sqlite_id", value_number(slot));
    map_set(&m, "path", value_copy(args[0]));
    map_set(&m, "type", value_string("sqlite_connection"));
    *result = m;
}

/* True if only whitespace, ';' and SQL comments remain: i.e. the text the
 * statement parser stopped at holds NO second statement. */
static int sqlite_tail_is_empty(const char *t) {
    while (t && *t) {
        if (isspace((unsigned char)*t) || *t == ';') { t++; continue; }
        if (t[0] == '-' && t[1] == '-') { while (*t && *t != '\n') t++; continue; }
        if (t[0] == '/' && t[1] == '*') {
            t += 2;
            while (*t && !(t[0] == '*' && t[1] == '/')) t++;
            if (*t) t += 2;
            continue;
        }
        return 0;
    }
    return 1;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Prepares exactly one statement. On failure prints an error naming `fname`
 * and returns NULL. Rejects: SQL errors, empty SQL, and MORE THAN ONE
 * statement (sqlite3_prepare_v2 silently ignores everything after the first). */
static sqlite3_stmt *sqlite_prepare_one(sqlite3 *db, const char *fname, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    const char *tail = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, &tail) != SQLITE_OK) {
        fprintf(stderr, "Runtime error: %s() - %s\n", fname, sqlite3_errmsg(db));
        return NULL;
    }
    if (!stmt) {
        fprintf(stderr, "Runtime error: %s() - the SQL contains no statement\n", fname);
        return NULL;
    }
    if (!sqlite_tail_is_empty(tail)) {
        fprintf(stderr, "Runtime error: %s() - only ONE SQL statement is allowed per call (everything after the first ';' would be ignored); call it once per statement\n", fname);
        sqlite3_finalize(stmt);
        return NULL;
    }
    return stmt;
}

/* Binds Kosh values to the statement's ? placeholders, in order.
 *   number  -> INTEGER when it is a whole number within +/-2^53, else REAL
 *   string  -> TEXT      bool -> 0/1      nil -> NULL
 *   {"type":"blob","hex":"4100..."} -> BLOB (binary-safe)
 * The NUMBER of values must equal the number of placeholders (a missing
 * value used to become NULL silently: "DELETE ... WHERE id = ?" with no
 * params deleted nothing and reported success). `params_given` is 0 when
 * the caller passed no third argument. Returns 1 on success. */
static int bind_params(sqlite3_stmt *stmt, const char *fname, int params_given, Value params) {
    int expected = sqlite3_bind_parameter_count(stmt);
    int n = 0;
    Value *items = NULL;
    if (params_given && params.type != VAL_NIL) {
        if (params.type != VAL_ARRAY) {
            fprintf(stderr, "Runtime error: %s() - params must be an array (got something else, which used to be ignored silently)\n", fname);
            return 0;
        }
        n = AS_ARRAY_COUNT(params);
        items = AS_ARRAY_ITEMS(params);
    }
    if (n != expected) {
        fprintf(stderr, "Runtime error: %s() - the statement has %d placeholder(s) but %d value(s) were given\n", fname, expected, n);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        int rc;
        if (items[i].type == VAL_NUMBER) {
            double d = items[i].as.number;
            if (!isfinite(d)) {
                fprintf(stderr, "Runtime error: %s() - param %d is NaN/Infinity, which SQLite cannot store\n", fname, i);
                return 0;
            }
            if (d == floor(d) && fabs(d) <= KOSH_MAX_EXACT_INT) rc = sqlite3_bind_int64(stmt, i + 1, (sqlite3_int64)d);
            else rc = sqlite3_bind_double(stmt, i + 1, d);
        } else if (items[i].type == VAL_STRING) {
            rc = sqlite3_bind_text(stmt, i + 1, items[i].as.string, -1, SQLITE_TRANSIENT);
        } else if (items[i].type == VAL_NIL) {
            rc = sqlite3_bind_null(stmt, i + 1);
        } else if (items[i].type == VAL_BOOL) {
            rc = sqlite3_bind_int(stmt, i + 1, items[i].as.boolean ? 1 : 0);
        } else if (items[i].type == VAL_MAP) {
            Value *ty = map_get(&items[i], "type");
            Value *hx = map_get(&items[i], "hex");
            if (!ty || ty->type != VAL_STRING || strcmp(ty->as.string, "blob") != 0 || !hx || hx->type != VAL_STRING) {
                fprintf(stderr, "Runtime error: %s() - param %d: a map must be {\"type\": \"blob\", \"hex\": \"...\"}\n", fname, i);
                return 0;
            }
            const char *h = hx->as.string;
            size_t hl = strlen(h);
            if (hl % 2) { fprintf(stderr, "Runtime error: %s() - param %d: blob hex has an odd length\n", fname, i); return 0; }
            unsigned char *buf = malloc(hl / 2 + 1);
            if (!buf) return 0;
            for (size_t k = 0; k < hl / 2; k++) {
                int hi = hex_val(h[2*k]), lo = hex_val(h[2*k+1]);
                if (hi < 0 || lo < 0) { free(buf); fprintf(stderr, "Runtime error: %s() - param %d: blob hex has a non-hex character\n", fname, i); return 0; }
                buf[k] = (unsigned char)(hi * 16 + lo);
            }
            rc = sqlite3_bind_blob64(stmt, i + 1, buf, (sqlite3_uint64)(hl / 2), SQLITE_TRANSIENT);
            free(buf);
        } else {
            fprintf(stderr, "Runtime error: %s() - param %d: unsupported type for binding\n", fname, i);
            return 0;
        }
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Runtime error: %s() - could not bind param %d\n", fname, i);
            return 0;
        }
    }
    return 1;
}

static void fn_sqlite_exec(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 2 || args[1].type != VAL_STRING) return;
    int slot = sqlite_resolve_slot(&args[0]);
    if (slot < 0) return;

    sqlite3_stmt *stmt = sqlite_prepare_one(g_db_handles[slot], "sqlite_exec", args[1].as.string);
    if (!stmt) return;
    if (!bind_params(stmt, "sqlite_exec", argc >= 3, argc >= 3 ? args[2] : value_nil())) {
        sqlite3_finalize(stmt);
        return;
    }

    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE && step != SQLITE_ROW) {
        fprintf(stderr, "Runtime error: sqlite_exec() - %s\n", sqlite3_errmsg(g_db_handles[slot]));
        return;
    }
    *result = value_bool(1);
}

static Value sqlite_blob_value(const void *data, int nbytes) {
    static const char HEXD[] = "0123456789abcdef";
    char *hex = malloc((size_t)nbytes * 2 + 1);
    Value m = value_map_empty();
    if (!hex) return m;
    const unsigned char *p = (const unsigned char *)data;
    for (int i = 0; i < nbytes; i++) { hex[2*i] = HEXD[p[i] >> 4]; hex[2*i+1] = HEXD[p[i] & 15]; }
    hex[(size_t)nbytes * 2] = '\0';
    map_set(&m, "type", value_string("blob"));
    map_set(&m, "size", value_number((double)nbytes));
    map_set(&m, "hex", value_string(hex));
    free(hex);
    return m;
}

/* sqlite_query(db, sql [, params]) -> array of row maps, or nil on ANY
 * failure (a query that fails half way used to return the rows read so
 * far as if it had succeeded).
 * Column types: INTEGER/REAL -> number (an INTEGER beyond +/-2^53 comes
 * back as a decimal STRING so no digits are lost), TEXT -> string
 * (cut at an embedded NUL), NULL -> nil, BLOB -> {"type":"blob","size":n,"hex":"..."}. */
static void fn_sqlite_query(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[1].type != VAL_STRING) return;
    int slot = sqlite_resolve_slot(&args[0]);
    if (slot < 0) return;

    sqlite3_stmt *stmt = sqlite_prepare_one(g_db_handles[slot], "sqlite_query", args[1].as.string);
    if (!stmt) return;
    if (!bind_params(stmt, "sqlite_query", argc >= 3, argc >= 3 ? args[2] : value_nil())) {
        sqlite3_finalize(stmt);
        return;
    }

    Value *rows = NULL;
    int row_count = 0, row_cap = 0;
    int ncols = sqlite3_column_count(stmt);

    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        Value row = value_map_empty();
        for (int c = 0; c < ncols; c++) {
            const char *colname = sqlite3_column_name(stmt, c);
            int coltype = sqlite3_column_type(stmt, c);
            Value colval;
            if (coltype == SQLITE_INTEGER) {
                sqlite3_int64 iv = sqlite3_column_int64(stmt, c);
                if (iv > (sqlite3_int64)KOSH_MAX_EXACT_INT || iv < -(sqlite3_int64)KOSH_MAX_EXACT_INT) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%lld", (long long)iv);
                    colval = value_string(buf);
                } else {
                    colval = value_number((double)iv);
                }
            } else if (coltype == SQLITE_FLOAT) {
                colval = value_number(sqlite3_column_double(stmt, c));
            } else if (coltype == SQLITE_NULL) {
                colval = value_nil();
            } else if (coltype == SQLITE_BLOB) {
                colval = sqlite_blob_value(sqlite3_column_blob(stmt, c), sqlite3_column_bytes(stmt, c));
            } else {
                const unsigned char *txt = sqlite3_column_text(stmt, c);
                colval = value_string(txt ? (const char*)txt : "");
            }
            map_set(&row, colname, colval);
        }
        if (row_count >= row_cap) {
            int ncap = row_cap ? row_cap * 2 : 8;
            Value *nr = realloc(rows, sizeof(Value) * ncap);
            if (!nr) { value_free(row); step = SQLITE_NOMEM; break; }
            rows = nr; row_cap = ncap;
        }
        rows[row_count++] = row;
    }
    sqlite3_finalize(stmt);

    if (step != SQLITE_DONE) {
        fprintf(stderr, "Runtime error: sqlite_query() - %s\n", step == SQLITE_NOMEM ? "out of memory" : sqlite3_errmsg(g_db_handles[slot]));
        for (int i = 0; i < row_count; i++) value_free(rows[i]);
        free(rows);
        return; /* nil */
    }

    *result = value_array(rows, row_count);
}

/* sqlite_busy_timeout(db, ms) -> bool. How long to wait for a lock held by
 * another connection (default 5000 ms; 0 = fail immediately). */
static void fn_sqlite_busy_timeout(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc != 2 || args[1].type != VAL_NUMBER) return;
    int slot = sqlite_resolve_slot(&args[0]);
    if (slot < 0) return;
    double ms = args[1].as.number;
    if (!(ms >= 0 && ms <= 600000)) {
        fprintf(stderr, "Runtime error: sqlite_busy_timeout() - ms must be between 0 and 600000\n");
        return;
    }
    sqlite3_busy_timeout(g_db_handles[slot], (int)ms);
    *result = value_bool(1);
}

static void fn_sqlite_close(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 1) return;
    int slot = sqlite_resolve_slot(&args[0]);
    if (slot < 0) return;
    sqlite3_close(g_db_handles[slot]);
    g_db_handles[slot] = NULL;
    *result = value_bool(1);
}

void sqlite_register_all(Environment *env) {
    env_define(env, "sqlite_open",  value_native("sqlite_open",  fn_sqlite_open));
    env_define(env, "sqlite_exec",  value_native("sqlite_exec",  fn_sqlite_exec));
    env_define(env, "sqlite_query", value_native("sqlite_query", fn_sqlite_query));
    env_define(env, "sqlite_close", value_native("sqlite_close", fn_sqlite_close));
    env_define(env, "sqlite_busy_timeout", value_native("sqlite_busy_timeout", fn_sqlite_busy_timeout));
}

void sqlite_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "sqlite_open",  fn_sqlite_open);
    vm_global_set_native(vm, "sqlite_exec",  fn_sqlite_exec);
    vm_global_set_native(vm, "sqlite_query", fn_sqlite_query);
    vm_global_set_native(vm, "sqlite_close", fn_sqlite_close);
    vm_global_set_native(vm, "sqlite_busy_timeout", fn_sqlite_busy_timeout);
}
