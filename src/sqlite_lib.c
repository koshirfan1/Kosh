#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "sqlite_lib.h"

/*
 * Real SQLite bridge for Khan, replacing a previous mock (fn_sqlite_exec
 * used to just return true without touching the SQL string or storing
 * anything - found and fixed in this session, see chat).
 *
 * Links against real libsqlite3 (libsqlite3-dev). Same handle-table
 * pattern as ocr_lib.c/llm_lib.c: a Khan-level map carries a slot index,
 * the real sqlite3* lives in a native-side table.
 */

#define SQLITE_MAX_HANDLES 8
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

    g_db_handles[slot] = db;
    Value m = value_map_empty();
    map_set(&m, "__sqlite_id", value_number(slot));
    map_set(&m, "path", value_copy(args[0]));
    map_set(&m, "type", value_string("sqlite_connection"));
    *result = m;
}

/* Binds a Khan array's elements (numbers/strings/nil) to a prepared
 * statement's ? placeholders, in order. Returns 1 on success, 0 on a
 * type it doesn't know how to bind. */
static int bind_params(sqlite3_stmt *stmt, Value params) {
    if (params.type != VAL_ARRAY) return 1; /* no params given - fine */
    int n = AS_ARRAY_COUNT(params);
    Value *items = AS_ARRAY_ITEMS(params);
    for (int i = 0; i < n; i++) {
        int rc;
        if (items[i].type == VAL_NUMBER) {
            rc = sqlite3_bind_double(stmt, i + 1, items[i].as.number);
        } else if (items[i].type == VAL_STRING) {
            rc = sqlite3_bind_text(stmt, i + 1, items[i].as.string, -1, SQLITE_TRANSIENT);
        } else if (items[i].type == VAL_NIL) {
            rc = sqlite3_bind_null(stmt, i + 1);
        } else if (items[i].type == VAL_BOOL) {
            rc = sqlite3_bind_int(stmt, i + 1, items[i].as.boolean ? 1 : 0);
        } else {
            fprintf(stderr, "Runtime error: sqlite param %d - unsupported type for binding\n", i);
            return 0;
        }
        if (rc != SQLITE_OK) return 0;
    }
    return 1;
}

static void fn_sqlite_exec(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 2 || args[1].type != VAL_STRING) return;
    int slot = sqlite_resolve_slot(&args[0]);
    if (slot < 0) return;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db_handles[slot], args[1].as.string, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Runtime error: sqlite_exec() - %s\n", sqlite3_errmsg(g_db_handles[slot]));
        return;
    }
    if (argc >= 3 && !bind_params(stmt, args[2])) {
        fprintf(stderr, "Runtime error: sqlite_exec() - parameter binding failed\n");
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

static void fn_sqlite_query(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[1].type != VAL_STRING) return;
    int slot = sqlite_resolve_slot(&args[0]);
    if (slot < 0) return;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db_handles[slot], args[1].as.string, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Runtime error: sqlite_query() - %s\n", sqlite3_errmsg(g_db_handles[slot]));
        return;
    }
    if (argc >= 3 && !bind_params(stmt, args[2])) {
        fprintf(stderr, "Runtime error: sqlite_query() - parameter binding failed\n");
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
            if (coltype == SQLITE_INTEGER || coltype == SQLITE_FLOAT) {
                colval = value_number(sqlite3_column_double(stmt, c));
            } else if (coltype == SQLITE_NULL) {
                colval = value_nil();
            } else {
                const unsigned char *txt = sqlite3_column_text(stmt, c);
                colval = value_string(txt ? (const char*)txt : "");
            }
            map_set(&row, colname, colval);
        }
        if (row_count >= row_cap) {
            row_cap = row_cap ? row_cap * 2 : 8;
            rows = realloc(rows, sizeof(Value) * row_cap);
        }
        rows[row_count++] = row;
    }
    sqlite3_finalize(stmt);

    if (step != SQLITE_DONE) {
        fprintf(stderr, "Runtime error: sqlite_query() - %s\n", sqlite3_errmsg(g_db_handles[slot]));
    }

    *result = value_array(rows, row_count);
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
}

void sqlite_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "sqlite_open",  fn_sqlite_open);
    vm_global_set_native(vm, "sqlite_exec",  fn_sqlite_exec);
    vm_global_set_native(vm, "sqlite_query", fn_sqlite_query);
    vm_global_set_native(vm, "sqlite_close", fn_sqlite_close);
}
