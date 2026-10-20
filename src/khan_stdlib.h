#ifndef KHAN_KHANSTDLIB_H
#define KHAN_KHANSTDLIB_H

#include "interpreter.h"

void stdlib_register_all(Environment *env);

/* Export standard library functions for VM registration */
void fn_len(Value *result, Interpreter *interp, int argc, Value *args);
void fn_str(Value *result, Interpreter *interp, int argc, Value *args);
void fn_num(Value *result, Interpreter *interp, int argc, Value *args);
void fn_type(Value *result, Interpreter *interp, int argc, Value *args);
void fn_push(Value *result, Interpreter *interp, int argc, Value *args);
void fn_range(Value *result, Interpreter *interp, int argc, Value *args);
void fn_keys(Value *result, Interpreter *interp, int argc, Value *args);
void fn_has(Value *result, Interpreter *interp, int argc, Value *args);
void fn_abs(Value *result, Interpreter *interp, int argc, Value *args);
void fn_sqrt(Value *result, Interpreter *interp, int argc, Value *args);
void fn_floor(Value *result, Interpreter *interp, int argc, Value *args);
void fn_ceil(Value *result, Interpreter *interp, int argc, Value *args);
void fn_round(Value *result, Interpreter *interp, int argc, Value *args);
void fn_pow(Value *result, Interpreter *interp, int argc, Value *args);
void fn_min(Value *result, Interpreter *interp, int argc, Value *args);
void fn_max(Value *result, Interpreter *interp, int argc, Value *args);
void fn_random(Value *result, Interpreter *interp, int argc, Value *args);
void fn_clock(Value *result, Interpreter *interp, int argc, Value *args);
void fn_gc_collect(Value *result, Interpreter *interp, int argc, Value *args);
void fn_upper(Value *result, Interpreter *interp, int argc, Value *args);
void fn_lower(Value *result, Interpreter *interp, int argc, Value *args);
void fn_trim(Value *result, Interpreter *interp, int argc, Value *args);
void fn_contains(Value *result, Interpreter *interp, int argc, Value *args);
void fn_substring(Value *result, Interpreter *interp, int argc, Value *args);
void fn_split(Value *result, Interpreter *interp, int argc, Value *args);
void fn_str_replace(Value *result, Interpreter *interp, int argc, Value *args);
void fn_input(Value *result, Interpreter *interp, int argc, Value *args);
void fn_read_file(Value *result, Interpreter *interp, int argc, Value *args);
void fn_write_file(Value *result, Interpreter *interp, int argc, Value *args);
void fn_file_exists(Value *result, Interpreter *interp, int argc, Value *args);
void fn_exit(Value *result, Interpreter *interp, int argc, Value *args);
void fn_sleep(Value *result, Interpreter *interp, int argc, Value *args);
void fn_base64_decode_to_file(Value *result, Interpreter *interp, int argc, Value *args);
void fn_remove_file(Value *result, Interpreter *interp, int argc, Value *args);

#endif
