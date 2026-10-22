#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include "chunk.h"

/* ── Chunk ── */

void chunk_init(Chunk *c) {
    c->code = NULL;  c->count = c->capacity = 0;
    c->lines = NULL;
    c->constants = NULL;  c->const_count = c->const_cap = 0;
}

void chunk_free(Chunk *c) {
    free(c->code);
    free(c->lines);
    free(c->constants);
    chunk_init(c);
}

void chunk_write(Chunk *c, uint8_t byte, int line) {
    if (c->count >= c->capacity) {
        c->capacity = c->capacity < 8 ? 8 : c->capacity * 2;
        c->code  = realloc(c->code,  (size_t)c->capacity);
        c->lines = realloc(c->lines, (size_t)c->capacity * sizeof(int));
    }
    c->code[c->count]  = byte;
    c->lines[c->count] = line;
    c->count++;
}

int chunk_add_const(Chunk *c, Value v) {
    if (c->const_count >= c->const_cap) {
        c->const_cap = c->const_cap < 8 ? 8 : c->const_cap * 2;
        c->constants = realloc(c->constants, (size_t)c->const_cap * sizeof(Value));
    }
    c->constants[c->const_count] = v;
    return c->const_count++;
}

/* ── KoshFunction ── */

KoshFunction *koshfn_new(const char *name, int arity) {
    KoshFunction *fn = malloc(sizeof(KoshFunction));
    fn->name       = name ? strdup(name) : NULL;
    fn->source_dir = NULL;
    fn->arity      = arity;
    fn->is_native  = 0;
    fn->upvalues       = NULL;
    fn->upvalue_count  = 0;
    chunk_init(&fn->chunk);
    return fn;
}

void koshfn_free(KoshFunction *fn) {
    if (!fn) return;
    free(fn->name);
    if (fn->source_dir) free(fn->source_dir);
    if (fn->upvalues) free(fn->upvalues);
    chunk_free(&fn->chunk);
    free(fn);
}

/* ── KoshClosure ── */

KoshClosure *koshclosure_new(int count) {
    KoshClosure *cl = malloc(sizeof(KoshClosure));
    cl->ref_count = 1;
    cl->count     = count;
    cl->values    = count > 0 ? malloc(sizeof(Value) * count) : NULL;
    return cl;
}

void koshclosure_retain(KoshClosure *cl) {
    if (cl) cl->ref_count++;
}

void koshclosure_release(KoshClosure *cl) {
    if (!cl) return;
    cl->ref_count--;
    if (cl->ref_count <= 0) {
        for (int i = 0; i < cl->count; i++) value_free(cl->values[i]);
        free(cl->values);
        free(cl);
    }
}
