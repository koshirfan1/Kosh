#ifndef KOSH_JSON_LIB_H
#define KOSH_JSON_LIB_H

#include "interpreter.h"

// Registers json_encode() and json_decode() as native builtins.
void json_register_all(Environment *env);

/* Export function pointers for VM registration */
void fn_json_encode(Value *result, Interpreter *interp, int argc, Value *args);
void fn_json_decode(Value *result, Interpreter *interp, int argc, Value *args);

#endif
