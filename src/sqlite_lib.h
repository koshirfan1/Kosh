#ifndef KHAN_SQLITE_LIB_H
#define KHAN_SQLITE_LIB_H

#include "interpreter.h"
#include "vm.h"

/*
 * Real bridge to libsqlite3 (was a mock - fn_sqlite_exec used to ignore
 * the SQL entirely and always return true; see chat/README for how this
 * was found and fixed).
 *
 * Registers SQLite native functions:
 *   sqlite_open(path)                  -> db connection object
 *   sqlite_exec(db, sql, [params])     -> success bool
 *   sqlite_query(db, sql, [params])    -> array of maps
 *   sqlite_close(db)                   -> bool
 *
 * `params` is an optional Khan array bound to the SQL's `?` placeholders
 * in order (via real sqlite3_bind_*, not string concatenation) - always
 * use this for any value that didn't come from your own source code.
 * Building SQL by concatenating user input into the string is a real SQL
 * injection vulnerability, not a hypothetical one; parameter binding is
 * the actual fix, not a style preference.
 */
void sqlite_register_all(Environment *env);
void sqlite_register_all_vm(VM *vm);

#endif
