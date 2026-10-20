#include <stdio.h>
#include <stdlib.h>
#include "int8array_lib.h"

void fn_int8_pack(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_ARRAY || args[1].type != VAL_NUMBER) {
        fprintf(stderr, "Runtime error: int8_pack(array, scale) - wrong argument types\n");
        return;
    }
    double scale = args[1].as.number;
    if (scale == 0) scale = 1; /* avoid divide-by-zero below */

    int count = AS_ARRAY_COUNT(args[0]);
    Value *items = AS_ARRAY_ITEMS(args[0]);

    signed char *packed = malloc(count > 0 ? (size_t)count : 1);
    if (!packed) return;

    for (int i = 0; i < count; i++) {
        if (items[i].type != VAL_NUMBER) {
            fprintf(stderr, "Runtime error: int8_pack() - array must contain only numbers (index %d)\n", i);
            free(packed);
            *result = value_nil();
            return;
        }
        double q = items[i].as.number / scale;
        q = (q >= 0) ? (double)(long)(q + 0.5) : (double)(long)(q - 0.5); /* round to nearest */
        if (q > 127) q = 127;
        if (q < -127) q = -127;
        packed[i] = (signed char)q;
    }

    *result = value_int8array(packed, count, scale);
    free(packed); /* value_int8array() makes its own copy, same convention as value_string()'s strdup */
}

void fn_int8_get(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_INT8ARRAY || args[1].type != VAL_NUMBER) return;

    int idx = (int)args[1].as.number;
    int count = AS_INT8ARRAY_COUNT(args[0]);
    if (idx < 0 || idx >= count) {
        fprintf(stderr, "Runtime error: int8_get() - index %d out of range (count %d)\n", idx, count);
        return;
    }
    double scale = AS_INT8ARRAY_SCALE(args[0]);
    signed char b = AS_INT8ARRAY_DATA(args[0])[idx];
    *result = value_number((double)b * scale);
}

void fn_int8_unpack(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_INT8ARRAY) return;

    int count = AS_INT8ARRAY_COUNT(args[0]);
    double scale = AS_INT8ARRAY_SCALE(args[0]);
    signed char *data = AS_INT8ARRAY_DATA(args[0]);

    Value *items = malloc(sizeof(Value) * (count > 0 ? (size_t)count : 1));
    if (!items) return;
    for (int i = 0; i < count; i++) {
        items[i] = value_number((double)data[i] * scale);
    }
    *result = value_array(items, count);
}

void fn_int8_len(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_INT8ARRAY) return;
    *result = value_number((double)AS_INT8ARRAY_COUNT(args[0]));
}

void fn_int8_dot(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_INT8ARRAY || args[1].type != VAL_ARRAY) {
        fprintf(stderr, "Runtime error: int8_dot(qarray, array) - wrong argument types\n");
        return;
    }
    int qcount = AS_INT8ARRAY_COUNT(args[0]);
    int xcount = AS_ARRAY_COUNT(args[1]);
    if (qcount != xcount) {
        fprintf(stderr, "Runtime error: int8_dot() - length mismatch (%d vs %d)\n", qcount, xcount);
        return;
    }
    signed char *data = AS_INT8ARRAY_DATA(args[0]);
    double scale = AS_INT8ARRAY_SCALE(args[0]);
    Value *xitems = AS_ARRAY_ITEMS(args[1]);

    double acc = 0;
    for (int i = 0; i < qcount; i++) {
        if (xitems[i].type != VAL_NUMBER) {
            fprintf(stderr, "Runtime error: int8_dot() - second argument must contain only numbers (index %d)\n", i);
            *result = value_nil();
            return;
        }
        acc += (double)data[i] * xitems[i].as.number;
    }
    *result = value_number(acc * scale);
}

void fn_int8_byte_size(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_INT8ARRAY) return;
    *result = value_number((double)AS_INT8ARRAY_COUNT(args[0]));
}

void int8array_register_all(Environment *env) {
    env_define(env, "int8_pack",      value_native("int8_pack",      fn_int8_pack));
    env_define(env, "int8_get",       value_native("int8_get",       fn_int8_get));
    env_define(env, "int8_unpack",    value_native("int8_unpack",    fn_int8_unpack));
    env_define(env, "int8_len",       value_native("int8_len",       fn_int8_len));
    env_define(env, "int8_byte_size", value_native("int8_byte_size", fn_int8_byte_size));
    env_define(env, "int8_dot",       value_native("int8_dot",       fn_int8_dot));
}

void int8array_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "int8_pack",      fn_int8_pack);
    vm_global_set_native(vm, "int8_get",       fn_int8_get);
    vm_global_set_native(vm, "int8_unpack",    fn_int8_unpack);
    vm_global_set_native(vm, "int8_len",       fn_int8_len);
    vm_global_set_native(vm, "int8_byte_size", fn_int8_byte_size);
    vm_global_set_native(vm, "int8_dot",       fn_int8_dot);
}
