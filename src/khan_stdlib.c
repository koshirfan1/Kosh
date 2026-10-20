#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include "khan_stdlib.h"

// ===========================================================================
// Helper: check argument count and types
// ===========================================================================
static int check_arg_count(Interpreter *interp, const char *name,
                            int expected, int actual) {
    if (actual != expected) {
        fprintf(stderr, "Runtime error: %s() expects %d argument(s), got %d\n",
                name, expected, actual);
        if (interp) interp->had_runtime_error = 1;
        return 0;
    }
    return 1;
}

static int expect_number(Interpreter *interp, const char *name,
                          int index, Value v, double *out) {
    if (v.type != VAL_NUMBER) {
        fprintf(stderr, "Runtime error: %s() argument %d must be a number\n",
                name, index + 1);
        if (interp) interp->had_runtime_error = 1;
        return 0;
    }
    *out = v.as.number;
    return 1;
}

static int expect_string(Interpreter *interp, const char *name,
                          int index, Value v, const char **out) {
    if (v.type != VAL_STRING) {
        fprintf(stderr, "Runtime error: %s() argument %d must be a string\n",
                name, index + 1);
        if (interp) interp->had_runtime_error = 1;
        return 0;
    }
    *out = v.as.string;
    return 1;
}

// ===========================================================================
// Type conversion functions
// ===========================================================================

static void value_to_str_recursive(Value v, char **buf, int *len, int *cap) {
    if (v.type == VAL_STRING) {
        size_t tlen = strlen(v.as.string);
        if ((size_t)(*len) + tlen + 1 > (size_t)(*cap)) {
            *cap = (*cap) ? (*cap) * 2 : 256;
            if ((size_t)(*len) + tlen + 1 > (size_t)(*cap)) *cap = (int)((size_t)(*len) + tlen + 256);
            *buf = realloc(*buf, (size_t)(*cap));
        }
        memcpy(*buf + *len, v.as.string, tlen);
        *len += (int)tlen;
        (*buf)[*len] = '\0';
        return;
    }

    char tmp[128];
    switch (v.type) {
        case VAL_NUMBER:
            if (v.as.number == (long long)v.as.number)
                snprintf(tmp, sizeof(tmp), "%lld", (long long)v.as.number);
            else snprintf(tmp, sizeof(tmp), "%g", v.as.number);
            break;
        case VAL_BOOL:
            snprintf(tmp, sizeof(tmp), "%s", v.as.boolean ? "true" : "false");
            break;
        case VAL_NIL:
            snprintf(tmp, sizeof(tmp), "nil");
            break;
        case VAL_ARRAY:
            snprintf(tmp, sizeof(tmp), "<array len=%d>", AS_ARRAY_COUNT(v));
            break;
        case VAL_MAP:
            snprintf(tmp, sizeof(tmp), "<map count=%d>", AS_MAP_COUNT(v));
            break;
        case VAL_FUNCTION:
            snprintf(tmp, sizeof(tmp), "<fn %s>", v.as.function.name ? v.as.function.name : "?");
            break;
        case VAL_NATIVE:
            snprintf(tmp, sizeof(tmp), "<native %s>", v.as.native.name ? v.as.native.name : "?");
            break;
        case VAL_TENSOR: {
            /* Reuse value_print's tensor renderer by writing to a temp
               buffer isn't available here (value_print writes to
               stdout), so fall back to a short, honest placeholder;
               str()/print on a tensor mainly matters for debugging and
               `print` already renders the real nested shape. */
            snprintf(tmp, sizeof(tmp), "<tensor>");
            break;
        }
        case VAL_CLASS:
            snprintf(tmp, sizeof(tmp), "<class %s>",
                     (v.as.obj && v.as.obj->class_name) ? v.as.obj->class_name : "?");
            break;
        case VAL_INSTANCE:
            snprintf(tmp, sizeof(tmp), "<%s instance>",
                     (v.as.obj && v.as.obj->class_ref && v.as.obj->class_ref->class_name)
                         ? v.as.obj->class_ref->class_name : "?");
            break;
        default:
            snprintf(tmp, sizeof(tmp), "<unknown>");
            break;
    }
    size_t tlen2 = strlen(tmp);
    if ((size_t)(*len) + tlen2 + 1 > (size_t)(*cap)) {
        *cap = (*cap) ? (*cap) * 2 : 256;
        if ((size_t)(*len) + tlen2 + 1 > (size_t)(*cap)) *cap = (int)((size_t)(*len) + tlen2 + 256);
        *buf = realloc(*buf, (size_t)(*cap));
    }
    memcpy(*buf + *len, tmp, tlen2);
    *len += (int)tlen2;
    (*buf)[*len] = '\0';
}

void fn_str(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "str", 1, argc)) { *result = value_nil(); return; }
    char *buf = NULL;
    int len = 0, cap = 0;
    value_to_str_recursive(args[0], &buf, &len, &cap);
    *result = value_string(buf ? buf : "");
    free(buf);
}

void fn_num(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "num", 1, argc)) { *result = value_nil(); return; }
    const char *s;
    if (expect_string(interp, "num", 0, args[0], &s)) {
        char *end;
        double d = strtod(s, &end);
        if (end == s) { *result = value_nil(); return; }
        *result = value_number(d);
        return;
    }
    *result = value_nil();
}

void fn_type(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "type", 1, argc)) { *result = value_nil(); return; }
    switch (args[0].type) {
        case VAL_NUMBER:   *result = value_string("number"); return;
        case VAL_STRING:   *result = value_string("string"); return;
        case VAL_BOOL:     *result = value_string("bool"); return;
        case VAL_NIL:      *result = value_string("nil"); return;
        case VAL_FUNCTION: *result = value_string("function"); return;
        case VAL_NATIVE:   *result = value_string("native"); return;
        case VAL_ARRAY:    *result = value_string("array"); return;
        case VAL_MAP:      *result = value_string("map"); return;
        case VAL_TENSOR:   *result = value_string("tensor"); return;
        case VAL_CLASS:    *result = value_string("class"); return;
        case VAL_INSTANCE: *result = value_string("instance"); return;
    }
    *result = value_string("unknown");
}

// ===========================================================================
// String functions
// ===========================================================================
void fn_len(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "len", 1, argc)) { *result = value_nil(); return; }
    if (args[0].type == VAL_STRING) {
        *result = value_number((double)strlen(args[0].as.string));
        return;
    }
    if (args[0].type == VAL_ARRAY) {
        *result = value_number((double)AS_ARRAY_COUNT(args[0]));
        return;
    }
    if (args[0].type == VAL_MAP) {
        *result = value_number((double)AS_MAP_COUNT(args[0]));
        return;
    }
    if (args[0].type == VAL_NUMBER) {
        char buf[64];
        if (args[0].as.number == (long long)args[0].as.number) {
            snprintf(buf, sizeof(buf), "%lld", (long long)args[0].as.number);
        } else {
            snprintf(buf, sizeof(buf), "%g", args[0].as.number);
        }
        *result = value_number((double)strlen(buf));
        return;
    }
    fprintf(stderr, "Runtime error: len() argument must be a string, number, array, or map\n");
    if (interp) interp->had_runtime_error = 1;
    *result = value_nil();
}

void fn_substring(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "substring", 3, argc)) { *result = value_nil(); return; }
    const char *s;
    double start, length;
    if (!expect_string(interp, "substring", 0, args[0], &s)) { *result = value_nil(); return; }
    if (!expect_number(interp, "substring", 1, args[1], &start)) { *result = value_nil(); return; }
    if (!expect_number(interp, "substring", 2, args[2], &length)) { *result = value_nil(); return; }

    int len = (int)strlen(s);
    int si = (int)start;
    int li = (int)length;

    if (si < 0) si = 0;
    if (si > len) si = len;
    if (li < 0) li = 0;
    if (si + li > len) li = len - si;

    char *tmp = malloc(li + 1);
    strncpy(tmp, s + si, li);
    tmp[li] = '\0';
    *result = value_string(tmp);
    free(tmp);
}

void fn_upper(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "upper", 1, argc)) { *result = value_nil(); return; }
    const char *s;
    if (!expect_string(interp, "upper", 0, args[0], &s)) { *result = value_nil(); return; }

    char *tmp = malloc(strlen(s) + 1);
    for (int i = 0; s[i]; i++) tmp[i] = toupper((unsigned char)s[i]);
    tmp[strlen(s)] = '\0';
    *result = value_string(tmp);
    free(tmp);
}

void fn_lower(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "lower", 1, argc)) { *result = value_nil(); return; }
    const char *s;
    if (!expect_string(interp, "lower", 0, args[0], &s)) { *result = value_nil(); return; }

    char *tmp = malloc(strlen(s) + 1);
    for (int i = 0; s[i]; i++) tmp[i] = tolower((unsigned char)s[i]);
    tmp[strlen(s)] = '\0';
    *result = value_string(tmp);
    free(tmp);
}

void fn_contains(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "contains", 2, argc)) { *result = value_nil(); return; }
    const char *haystack, *needle;
    if (!expect_string(interp, "contains", 0, args[0], &haystack)) { *result = value_nil(); return; }
    if (!expect_string(interp, "contains", 1, args[1], &needle)) { *result = value_nil(); return; }
    *result = value_bool(strstr(haystack, needle) != NULL);
}

void fn_trim(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "trim", 1, argc)) { *result = value_nil(); return; }
    const char *s;
    if (!expect_string(interp, "trim", 0, args[0], &s)) { *result = value_nil(); return; }

    while (*s && isspace((unsigned char)*s)) s++;
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;

    char *tmp = malloc(len + 1);
    strncpy(tmp, s, len);
    tmp[len] = '\0';
    *result = value_string(tmp);
    free(tmp);
}

void fn_split(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "split", 2, argc)) { *result = value_nil(); return; }
    const char *s, *delim;
    if (!expect_string(interp, "split", 0, args[0], &s)) { *result = value_nil(); return; }
    if (!expect_string(interp, "split", 1, args[1], &delim)) { *result = value_nil(); return; }

    size_t delim_len = strlen(delim);
    if (delim_len == 0) {
        Value *items = malloc(sizeof(Value));
        items[0] = value_string(s);
        *result = value_array(items, 1);
        return;
    }

    int count = 1;
    for (const char *p = s; (p = strstr(p, delim)) != NULL; p += delim_len) {
        count++;
    }

    Value *items = malloc(sizeof(Value) * count);
    int i = 0;
    const char *seg_start = s;
    const char *pos;
    while ((pos = strstr(seg_start, delim)) != NULL) {
        int seg_len = (int)(pos - seg_start);
        char *tmp = malloc(seg_len + 1);
        memcpy(tmp, seg_start, seg_len);
        tmp[seg_len] = '\0';
        items[i++] = value_string(tmp);
        free(tmp);
        seg_start = pos + delim_len;
    }
    items[i++] = value_string(seg_start);

    *result = value_array(items, count);
}

void fn_str_replace(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "str_replace", 3, argc)) { *result = value_nil(); return; }
    const char *s, *old, *newv;
    if (!expect_string(interp, "str_replace", 0, args[0], &s))    { *result = value_nil(); return; }
    if (!expect_string(interp, "str_replace", 1, args[1], &old))  { *result = value_nil(); return; }
    if (!expect_string(interp, "str_replace", 2, args[2], &newv)) { *result = value_nil(); return; }

    size_t old_len = strlen(old);
    if (old_len == 0) {
        *result = value_string(s);
        return;
    }

    size_t s_len = strlen(s);
    size_t new_len = strlen(newv);

    size_t count = 0;
    for (size_t i = 0; i + old_len <= s_len; ) {
        if (memcmp(s + i, old, old_len) == 0) {
            count++;
            i += old_len;
        } else {
            i++;
        }
    }

    if (count == 0) {
        *result = value_string(s);
        return;
    }

    size_t out_len = s_len - (count * old_len) + (count * new_len);
    char *out = malloc(out_len + 1);
    size_t oi = 0;
    for (size_t i = 0; i < s_len; ) {
        if (i + old_len <= s_len && memcmp(s + i, old, old_len) == 0) {
            memcpy(out + oi, newv, new_len);
            oi += new_len;
            i += old_len;
        } else {
            out[oi++] = s[i++];
        }
    }
    out[oi] = '\0';

    *result = value_string(out);
    free(out);
}

// ===========================================================================
// Array functions
// ===========================================================================
void fn_push(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "push", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_ARRAY) {
        fprintf(stderr, "Runtime error: push() first argument must be an array\n");
        if (interp) interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }
    int old_count = AS_ARRAY_COUNT(args[0]);
    Value *items = malloc(sizeof(Value) * (old_count + 1));
    for (int i = 0; i < old_count; i++) {
        items[i] = value_copy(AS_ARRAY_ITEMS(args[0])[i]);
    }
    items[old_count] = value_copy(args[1]);
    *result = value_array(items, old_count + 1);
}

void fn_range(Value *result, Interpreter *interp, int argc, Value *args) {
    if (argc != 1 && argc != 2) {
        fprintf(stderr, "Runtime error: range() expects 1 or 2 arguments, got %d\n", argc);
        if (interp) interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }
    double start = 0, end;
    if (argc == 1) {
        if (!expect_number(interp, "range", 0, args[0], &end)) { *result = value_nil(); return; }
    } else {
        if (!expect_number(interp, "range", 0, args[0], &start)) { *result = value_nil(); return; }
        if (!expect_number(interp, "range", 1, args[1], &end)) { *result = value_nil(); return; }
    }

    int istart = (int)start, iend = (int)end;
    int count = iend > istart ? iend - istart : 0;
    Value *items = count > 0 ? malloc(sizeof(Value) * count) : NULL;
    for (int i = 0; i < count; i++) {
        items[i] = value_number(istart + i);
    }
    *result = value_array(items, count);
}


void fn_abs(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "abs", 1, argc)) { *result = value_nil(); return; }
    double n;
    if (!expect_number(interp, "abs", 0, args[0], &n)) { *result = value_nil(); return; }
    *result = value_number(fabs(n));
}

void fn_min(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "min", 2, argc)) { *result = value_nil(); return; }
    double a, b;
    if (!expect_number(interp, "min", 0, args[0], &a)) { *result = value_nil(); return; }
    if (!expect_number(interp, "min", 1, args[1], &b)) { *result = value_nil(); return; }
    *result = value_number(a < b ? a : b);
}

void fn_max(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "max", 2, argc)) { *result = value_nil(); return; }
    double a, b;
    if (!expect_number(interp, "max", 0, args[0], &a)) { *result = value_nil(); return; }
    if (!expect_number(interp, "max", 1, args[1], &b)) { *result = value_nil(); return; }
    *result = value_number(a > b ? a : b);
}

void fn_sqrt(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "sqrt", 1, argc)) { *result = value_nil(); return; }
    double n;
    if (!expect_number(interp, "sqrt", 0, args[0], &n)) { *result = value_nil(); return; }
    if (n < 0) {
        fprintf(stderr, "Runtime error: sqrt() of negative number\n");
        if (interp) interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }
    *result = value_number(sqrt(n));
}

void fn_round(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "round", 1, argc)) { *result = value_nil(); return; }
    double n;
    if (!expect_number(interp, "round", 0, args[0], &n)) { *result = value_nil(); return; }
    *result = value_number(round(n));
}

void fn_floor(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "floor", 1, argc)) { *result = value_nil(); return; }
    double n;
    if (!expect_number(interp, "floor", 0, args[0], &n)) { *result = value_nil(); return; }
    *result = value_number(floor(n));
}

void fn_ceil(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "ceil", 1, argc)) { *result = value_nil(); return; }
    double n;
    if (!expect_number(interp, "ceil", 0, args[0], &n)) { *result = value_nil(); return; }
    *result = value_number(ceil(n));
}

void fn_random(Value *result, Interpreter *interp, int argc, Value *args) {
#ifndef RAND_MAX
#  define RAND_MAX 32767
#endif
    if (argc == 0) {
        *result = value_number((double)rand() / RAND_MAX);
        return;
    }
    double max;
    if (!expect_number(interp, "random", 0, args[0], &max)) { *result = value_nil(); return; }
    *result = value_number((double)rand() / RAND_MAX * max);
}

void fn_pow(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "pow", 2, argc)) { *result = value_nil(); return; }
    double base, exp;
    if (!expect_number(interp, "pow", 0, args[0], &base)) { *result = value_nil(); return; }
    if (!expect_number(interp, "pow", 1, args[1], &exp)) { *result = value_nil(); return; }
    *result = value_number(pow(base, exp));
}

// ===========================================================================
// I/O functions
// ===========================================================================
void fn_input(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    if (argc > 0 && args[0].type == VAL_STRING) {
        printf("%s", args[0].as.string);
    }

    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) {
        *result = value_string("");
        return;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    *result = value_string(buf);
}

void fn_read_file(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "read_file", 1, argc)) { *result = value_nil(); return; }
    const char *path;
    if (!expect_string(interp, "read_file", 0, args[0], &path)) { *result = value_nil(); return; }

    FILE *f = fopen(path, "rb");
    if (!f) {
        *result = value_nil();
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buffer = malloc(size + 1);
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    *result = value_string(buffer);
    free(buffer);
}

void fn_write_file(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "write_file", 2, argc)) { *result = value_nil(); return; }
    const char *path, *content;
    if (!expect_string(interp, "write_file", 0, args[0], &path)) { *result = value_nil(); return; }
    if (!expect_string(interp, "write_file", 1, args[1], &content)) { *result = value_nil(); return; }

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Runtime error: write_file() could not open '%s'\n", path);
        if (interp) interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }

    fputs(content, f);
    fclose(f);
    *result = value_number((double)strlen(content));
}

void fn_file_exists(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "file_exists", 1, argc)) { *result = value_nil(); return; }
    const char *path;
    if (!expect_string(interp, "file_exists", 0, args[0], &path)) { *result = value_nil(); return; }

    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        *result = value_bool(1);
    } else {
        *result = value_bool(0);
    }
}

/* remove_file(path) -> bool. Rounds out read_file/write_file/file_exists
 * with the one file operation Khan didn't have at all before: deleting
 * one. Added alongside base64_decode_to_file, which writes temp files a
 * webi handler will usually want to clean up afterward. */
void fn_remove_file(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "remove_file", 1, argc)) { *result = value_bool(0); return; }
    const char *path;
    if (!expect_string(interp, "remove_file", 0, args[0], &path)) { *result = value_bool(0); return; }
    *result = value_bool(remove(path) == 0);
}

// ===========================================================================
// Utility functions
// ===========================================================================
void fn_sleep(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "sleep", 1, argc)) { *result = value_nil(); return; }
    double ms;
    if (!expect_number(interp, "sleep", 0, args[0], &ms)) { *result = value_nil(); return; }

    clock_t start = clock();
    while ((double)(clock() - start) / CLOCKS_PER_SEC < ms / 1000.0) {}
    *result = value_nil();
}

void fn_clock(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)args;
    if (!check_arg_count(interp, "clock", 0, argc)) { *result = value_nil(); return; }
    *result = value_number((double)clock() / CLOCKS_PER_SEC);
}

/* Forces one cycle-collector pass right now, instead of waiting for
 * the automatic every-4096-allocations trigger (see value.c's
 * GC_AUTO_INTERVAL). Mainly useful for tests/benchmarks that want a
 * deterministic point to check "did the cycle actually get freed" —
 * ordinary scripts never need to call this themselves. Returns nil. */
void fn_gc_collect(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)args;
    if (!check_arg_count(interp, "gc_collect", 0, argc)) { *result = value_nil(); return; }
    gc_collect_cycles();
    *result = value_nil();
}

void fn_exit(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    int code = 0;
    if (argc > 0 && args[0].type == VAL_NUMBER) {
        code = (int)args[0].as.number;
    }
    exit(code);
    *result = value_nil();
}

// ===========================================================================
// Map functions
// ===========================================================================
void fn_keys(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "keys", 1, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_MAP) {
        fprintf(stderr, "Runtime error: keys() argument must be a map\n");
        if (interp) interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }
    int count = AS_MAP_COUNT(args[0]);
    Value *items = count > 0 ? malloc(sizeof(Value) * count) : NULL;
    for (int i = 0; i < count; i++) {
        items[i] = value_string(AS_MAP_ENTRIES(args[0])[i].key);
    }
    *result = value_array(items, count);
}

void fn_has(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "has", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type == VAL_NIL) {
        /* A nil "map" reasonably means "nothing to look up" — treat it as
           key-not-found rather than a type error. This matters because
           query_get()/header_get()/param_get() (and similar) build their
           default-value fallback on top of has(), and are documented to
           gracefully return the default rather than erroring when the
           underlying map is absent. */
        *result = value_bool(0);
        return;
    }
    if (args[0].type != VAL_MAP) {
        fprintf(stderr, "Runtime error: has() first argument must be a map\n");
        if (interp) interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }
    const char *key;
    if (!expect_string(interp, "has", 1, args[1], &key)) { *result = value_nil(); return; }
    *result = value_bool(map_get(&args[0], key) != NULL);
}

static int b64_char_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* base64_decode_to_file(b64_string, output_path) -> bool
 *
 * Decodes straight to a file rather than returning the raw bytes as a
 * Khan string, deliberately: Khan strings are NUL-terminated (see the
 * "binary-safe string type" roadmap item), so decoded binary data -
 * which routinely contains embedded NUL bytes, e.g. almost any real
 * PNG/JPEG - can't safely round-trip through one. This exists mainly so
 * a webi handler can take a base64 image (posted as JSON, since webi
 * has no multipart/file-upload support yet) and hand vision_load() a
 * real file directly, without ever needing the raw bytes to exist as a
 * Khan value at all. */
void fn_base64_decode_to_file(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "base64_decode_to_file", 2, argc)) { *result = value_bool(0); return; }
    const char *b64, *path;
    if (!expect_string(interp, "base64_decode_to_file", 0, args[0], &b64)) { *result = value_bool(0); return; }
    if (!expect_string(interp, "base64_decode_to_file", 1, args[1], &path)) { *result = value_bool(0); return; }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Runtime error: base64_decode_to_file() could not open '%s'\n", path);
        if (interp) interp->had_runtime_error = 1;
        *result = value_bool(0);
        return;
    }

    int buf[4];
    int n = 0;
    for (const char *p = b64; *p; p++) {
        char c = *p;
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = b64_char_value(c);
        if (v < 0) continue;
        buf[n++] = v;
        if (n == 4) {
            fputc((unsigned char)((buf[0] << 2) | (buf[1] >> 4)), f);
            fputc((unsigned char)(((buf[1] & 0xF) << 4) | (buf[2] >> 2)), f);
            fputc((unsigned char)(((buf[2] & 0x3) << 6) | buf[3]), f);
            n = 0;
        }
    }
    if (n == 2) {
        fputc((unsigned char)((buf[0] << 2) | (buf[1] >> 4)), f);
    } else if (n == 3) {
        fputc((unsigned char)((buf[0] << 2) | (buf[1] >> 4)), f);
        fputc((unsigned char)(((buf[1] & 0xF) << 4) | (buf[2] >> 2)), f);
    }

    fclose(f);
    *result = value_bool(1);
}

// ===========================================================================
// Registration
// ===========================================================================
void stdlib_register_all(Environment *env) {
    srand((unsigned int)time(NULL));

    // Type conversions
    env_define(env, "num", value_native("num", fn_num));
    env_define(env, "str", value_native("str", fn_str));
    env_define(env, "type", value_native("type", fn_type));

    // String functions
    env_define(env, "len", value_native("len", fn_len));
    env_define(env, "substring", value_native("substring", fn_substring));
    env_define(env, "upper", value_native("upper", fn_upper));
    env_define(env, "lower", value_native("lower", fn_lower));
    env_define(env, "contains", value_native("contains", fn_contains));
    env_define(env, "trim", value_native("trim", fn_trim));
    env_define(env, "split", value_native("split", fn_split));
    env_define(env, "str_replace", value_native("str_replace", fn_str_replace));

    // Array functions
    env_define(env, "push", value_native("push", fn_push));
    env_define(env, "range", value_native("range", fn_range));

    // Map functions
    env_define(env, "keys", value_native("keys", fn_keys));
    env_define(env, "has", value_native("has", fn_has));

    // Math functions
    env_define(env, "abs", value_native("abs", fn_abs));
    env_define(env, "min", value_native("min", fn_min));
    env_define(env, "max", value_native("max", fn_max));
    env_define(env, "sqrt", value_native("sqrt", fn_sqrt));
    env_define(env, "round", value_native("round", fn_round));
    env_define(env, "floor", value_native("floor", fn_floor));
    env_define(env, "ceil", value_native("ceil", fn_ceil));
    env_define(env, "random", value_native("random", fn_random));
    env_define(env, "pow", value_native("pow", fn_pow));

    // I/O functions
    env_define(env, "input", value_native("input", fn_input));
    env_define(env, "read_file", value_native("read_file", fn_read_file));
    env_define(env, "write_file", value_native("write_file", fn_write_file));
    env_define(env, "file_exists", value_native("file_exists", fn_file_exists));

    // Utility
    env_define(env, "sleep", value_native("sleep", fn_sleep));
    env_define(env, "clock", value_native("clock", fn_clock));
    env_define(env, "exit", value_native("exit", fn_exit));
    env_define(env, "base64_decode_to_file", value_native("base64_decode_to_file", fn_base64_decode_to_file));
    env_define(env, "remove_file", value_native("remove_file", fn_remove_file));
}
