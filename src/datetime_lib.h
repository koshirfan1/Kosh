#ifndef KHAN_DATETIME_LIB_H
#define KHAN_DATETIME_LIB_H

#include "interpreter.h"

// Registers datetime functions:
//   now()               -> map with year/month/day/hour/minute/second/weekday/timestamp
//   date_format(map, fmt) -> string (strftime-style)
//   date_parse(str, fmt)  -> map (parses a date string)
//   date_diff(map1, map2) -> number (difference in seconds)
//   timestamp()          -> number (Unix timestamp as float)
void datetime_register_all(Environment *env);

/* Export function pointers for VM registration */
void fn_now(Value *result, Interpreter *interp, int argc, Value *args);
void fn_utcnow(Value *result, Interpreter *interp, int argc, Value *args);
void fn_timestamp(Value *result, Interpreter *interp, int argc, Value *args);
void fn_date_format(Value *result, Interpreter *interp, int argc, Value *args);
void fn_date_parse(Value *result, Interpreter *interp, int argc, Value *args);
void fn_date_diff(Value *result, Interpreter *interp, int argc, Value *args);
void fn_date_add(Value *result, Interpreter *interp, int argc, Value *args);

#endif
