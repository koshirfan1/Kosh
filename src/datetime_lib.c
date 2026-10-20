#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "datetime_lib.h"

// ---------------------------------------------------------------------------
// Helper: build a Khan map from a struct tm + timestamp
// ---------------------------------------------------------------------------
static Value tm_to_map(struct tm *t, time_t ts) {
    Value m = value_map_empty();
    map_set(&m, "year",      value_number(t->tm_year + 1900));
    map_set(&m, "month",     value_number(t->tm_mon + 1));
    map_set(&m, "day",       value_number(t->tm_mday));
    map_set(&m, "hour",      value_number(t->tm_hour));
    map_set(&m, "minute",    value_number(t->tm_min));
    map_set(&m, "second",    value_number(t->tm_sec));
    // weekday: 0=Sunday ... 6=Saturday  (same as tm_wday)
    map_set(&m, "weekday",   value_number(t->tm_wday));
    map_set(&m, "yearday",   value_number(t->tm_yday + 1));
    map_set(&m, "timestamp", value_number((double)ts));
    return m;
}

// ---------------------------------------------------------------------------
// now() -> date map in local time
// ---------------------------------------------------------------------------
void fn_now(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp; (void)argc; (void)args;
    time_t ts = time(NULL);
    struct tm *t = localtime(&ts);
    *result = tm_to_map(t, ts);
}

// ---------------------------------------------------------------------------
// utcnow() -> date map in UTC
// ---------------------------------------------------------------------------
void fn_utcnow(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp; (void)argc; (void)args;
    time_t ts = time(NULL);
    struct tm *t = gmtime(&ts);
    *result = tm_to_map(t, ts);
}

// ---------------------------------------------------------------------------
// timestamp() -> current Unix timestamp as a float
// ---------------------------------------------------------------------------
void fn_timestamp(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp; (void)argc; (void)args;
    *result = value_number((double)time(NULL));
}

// ---------------------------------------------------------------------------
// date_format(date_map, format_string) -> string
// Uses strftime format codes: %Y %m %d %H %M %S %A %B etc.
// ---------------------------------------------------------------------------
void fn_date_format(Value *result, Interpreter *interp, int argc, Value *args) {
    if (argc != 2 || args[0].type != VAL_MAP || args[1].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: date_format(date_map, fmt) expects a map and a string\n");
        interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }

    Value *yr  = map_get(&args[0], "year");
    Value *mo  = map_get(&args[0], "month");
    Value *dy  = map_get(&args[0], "day");
    Value *hr  = map_get(&args[0], "hour");
    Value *mn  = map_get(&args[0], "minute");
    Value *sc  = map_get(&args[0], "second");
    Value *wd  = map_get(&args[0], "weekday");
    Value *yd  = map_get(&args[0], "yearday");

    struct tm t = {0};
    if (yr) t.tm_year = (int)yr->as.number - 1900;
    if (mo) t.tm_mon  = (int)mo->as.number - 1;
    if (dy) t.tm_mday = (int)dy->as.number;
    if (hr) t.tm_hour = (int)hr->as.number;
    if (mn) t.tm_min  = (int)mn->as.number;
    if (sc) t.tm_sec  = (int)sc->as.number;
    if (wd) t.tm_wday = (int)wd->as.number;
    if (yd) t.tm_yday = (int)yd->as.number - 1;
    mktime(&t); // normalize

    char buf[512];
    strftime(buf, sizeof(buf), args[1].as.string, &t);
    *result = value_string(buf);
}

// ---------------------------------------------------------------------------
// date_parse(string, format) -> date map
// ---------------------------------------------------------------------------
void fn_date_parse(Value *result, Interpreter *interp, int argc, Value *args) {
    if (argc != 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: date_parse(str, fmt) expects two strings\n");
        interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }

    struct tm t = {0};
#ifdef _WIN32
    // strptime is not available on Windows — do a best-effort manual parse
    // for common formats like "%Y-%m-%d" and "%Y-%m-%d %H:%M:%S"
    const char *s   = args[0].as.string;
    const char *fmt = args[1].as.string;
    if (strcmp(fmt, "%Y-%m-%d %H:%M:%S") == 0) {
        sscanf(s, "%d-%d-%d %d:%d:%d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec);
        t.tm_year -= 1900; t.tm_mon -= 1;
    } else {
        sscanf(s, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
        t.tm_year -= 1900; t.tm_mon -= 1;
    }
#else
    strptime(args[0].as.string, args[1].as.string, &t);
#endif
    time_t ts = mktime(&t);
    *result = tm_to_map(&t, ts);
}

// ---------------------------------------------------------------------------
// date_diff(map1, map2) -> seconds (map1 - map2), positive if map1 is later
// ---------------------------------------------------------------------------
void fn_date_diff(Value *result, Interpreter *interp, int argc, Value *args) {
    if (argc != 2 || args[0].type != VAL_MAP || args[1].type != VAL_MAP) {
        fprintf(stderr, "Runtime error: date_diff(d1, d2) expects two date maps\n");
        interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }

    Value *ts1 = map_get(&args[0], "timestamp");
    Value *ts2 = map_get(&args[1], "timestamp");
    if (!ts1 || !ts2) {
        *result = value_nil();
        return;
    }
    *result = value_number(ts1->as.number - ts2->as.number);
}

// ---------------------------------------------------------------------------
// date_add(date_map, key, amount) -> new date map
// key: "seconds", "minutes", "hours", "days"
// ---------------------------------------------------------------------------
void fn_date_add(Value *result, Interpreter *interp, int argc, Value *args) {
    if (argc != 3 || args[0].type != VAL_MAP ||
        args[1].type != VAL_STRING || args[2].type != VAL_NUMBER) {
        fprintf(stderr, "Runtime error: date_add(date, unit, amount)\n");
        interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }

    Value *ts_val = map_get(&args[0], "timestamp");
    if (!ts_val) { *result = value_nil(); return; }

    time_t ts = (time_t)ts_val->as.number;
    double amount = args[2].as.number;
    const char *unit = args[1].as.string;

    if      (strcmp(unit, "seconds") == 0) ts += (time_t)amount;
    else if (strcmp(unit, "minutes") == 0) ts += (time_t)(amount * 60);
    else if (strcmp(unit, "hours")   == 0) ts += (time_t)(amount * 3600);
    else if (strcmp(unit, "days")    == 0) ts += (time_t)(amount * 86400);
    else if (strcmp(unit, "weeks")   == 0) ts += (time_t)(amount * 604800);

    struct tm *t = localtime(&ts);
    *result = tm_to_map(t, ts);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void datetime_register_all(Environment *env) {
    env_define(env, "now",         value_native("now",         fn_now));
    env_define(env, "utcnow",      value_native("utcnow",      fn_utcnow));
    env_define(env, "timestamp",   value_native("timestamp",   fn_timestamp));
    env_define(env, "date_format", value_native("date_format", fn_date_format));
    env_define(env, "date_parse",  value_native("date_parse",  fn_date_parse));
    env_define(env, "date_diff",   value_native("date_diff",   fn_date_diff));
    env_define(env, "date_add",    value_native("date_add",    fn_date_add));
}
