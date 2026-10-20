#ifndef KHAN_CHUNK_H
#define KHAN_CHUNK_H

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "value.h"

/* ─────────────────────────────────────────────
   Opcode table
   ───────────────────────────────────────────── */
typedef enum {
    /* Literals */
    OP_CONST,           /* push constants[READ_BYTE()] */
    OP_CONST_WIDE,      /* push constants[READ_SHORT()] */
    OP_NIL,
    OP_TRUE,
    OP_FALSE,

    /* Arithmetic */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEGATE_NUM,

    /* Comparison */
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,

    /* Logical */
    OP_NOT_BOOL,

    /* Variables */
    OP_GET_LOCAL,       /* operand = stack-slot index (1 byte) */
    OP_SET_LOCAL,
    OP_GET_GLOBAL,      /* operand = constants index of name string (1 byte) */
    OP_GET_GLOBAL_WIDE, /* 2 bytes */
    OP_SET_GLOBAL,
    OP_SET_GLOBAL_WIDE,
    OP_DEF_GLOBAL,
    OP_DEF_GLOBAL_WIDE,

    /* Control flow  — 2-byte big-endian offset operands */
    OP_JUMP,            /* unconditional forward jump  */
    OP_JUMP_IF_FALSE,   /* pop + jump forward if false */
    OP_LOOP,            /* unconditional backward jump */

    /* Break / continue helpers */
    OP_BREAK,
    OP_CONTINUE,

    /* try / catch / throw — see docs/ (or vm.c's OP_TRY_BEGIN comment)
       for the full unwind design. OP_TRY_BEGIN/OP_TRY_END bracket the
       try-block's bytecode: BEGIN registers a handler (2-byte forward
       offset to the catch block, same jump-offset encoding as OP_JUMP),
       END pops it off on normal (non-throwing) completion. OP_THROW
       pops a value and either unwinds to the nearest active handler
       (rewinding the VM stack/frames and jumping to its catch block,
       pushing the thrown value for the catch block to bind or discard)
       or, with no handler active, prints it and terminates — same
       fatal-uncaught-error behavior every other runtime error already
       had before try/catch existed. */
    OP_TRY_BEGIN,       /* operand = 2-byte offset to catch block start */
    OP_TRY_END,
    OP_THROW,

    /* Functions */
    OP_CALL,            /* operand = arg count */
    OP_RETURN,

    /* Closures */
    OP_GET_UPVALUE,     /* operand = upvalue-slot index (1 byte) */
    OP_SET_UPVALUE,

    /* Collections */
    OP_MAKE_ARRAY,      /* operand = element count (1 byte, max 255) */
    OP_MAKE_ARRAY_WIDE, /* operand = element count (2 bytes, big-endian) —
                           used once a literal has more than 255 elements;
                           without this, the count silently wrapped via a
                           uint8_t truncation, producing a wrong-sized
                           array with no error at all. See docs/
                           ast-audit.md for how this was found. */
    OP_MAKE_MAP,        /* operand = pair count (1 byte, max 255) */
    OP_MAKE_MAP_WIDE,   /* operand = pair count (2 bytes, big-endian) */
    OP_GET_INDEX,
    OP_SET_INDEX,

    /* I/O */
    OP_PRINT,

    OP_POP,

    /* ── Classes / instances ── */
    OP_MAKE_CLASS,       /* operand = method count (1 byte). Stack: [name][mname][fn_idx]... */
    OP_MAKE_CLASS_WIDE,  /* operand = method count (2 bytes, big-endian) */
    OP_CALL_METHOD,      /* operands = name const idx (1 byte), arg count (1 byte). Stack: [obj][args...] */
    OP_CALL_METHOD_WIDE, /* operands = name const idx (2 bytes), arg count (1 byte) */
    OP_GET_ATTR,         /* operand = name const idx (1 byte). Stack: [obj] -> [value] */
    OP_GET_ATTR_WIDE,    /* operand = name const idx (2 bytes) */
    OP_SET_ATTR,         /* operand = name const idx (1 byte). Stack: [obj][value] -> [value] */
    OP_SET_ATTR_WIDE,    /* operand = name const idx (2 bytes) */
    OP_CALL_SUPER,       /* operands = superclass name const idx (2 bytes),
                             method name const idx (2 bytes), arg count (1
                             byte). Stack: [self][args...], self pushed by
                             the compiler via OP_GET_LOCAL 0. Resolves the
                             method starting at the *named* superclass's
                             own chain — static, not the receiver's
                             dynamic class. Always wide (2-byte name
                             indices): super calls are rare enough that a
                             narrow variant isn't worth a second opcode. */

    OP_COUNT /* sentinel: always the total number of opcodes, must stay
                last in this enum. Used by run_loop's computed-goto
                dispatch to bounds-check a byte before indexing its
                dispatch table with it — see vm.c's DISPATCH() macro. */
} OpCode;

/* ─────────────────────────────────────────────
   A Chunk holds all bytecode for one function
   ───────────────────────────────────────────── */
typedef struct Chunk {
    uint8_t  *code;
    int       count;
    int       capacity;

    int      *lines;       /* parallel to code — source line numbers */

    Value    *constants;
    int       const_count;
    int       const_cap;
} Chunk;

void chunk_init   (Chunk *c);
void chunk_free   (Chunk *c);
void chunk_write  (Chunk *c, uint8_t byte, int line);
int  chunk_add_const(Chunk *c, Value v);  /* returns constant index */

/* Describes one variable a nested function captures from its enclosing
   function: either straight from the enclosing frame's local stack slot
   (is_local = 1, index = slot), or forwarded from the enclosing
   function's own captured upvalues (is_local = 0, index = upvalue slot)
   — the latter is what makes capturing correct more than one nesting
   level deep. */
typedef struct {
    int is_local;
    int index;
} UpvalueDesc;

/* ─────────────────────────────────────────────
   KhanFunction wraps a Chunk with metadata.
   Stored as a Value (VAL_FUNCTION).
   ───────────────────────────────────────────── */
typedef struct KhanFunction {
    char  *name;
    char  *source_dir;     /* Path directory of the .kh file this fn was defined in */
    int    arity;
    Chunk  chunk;
    int    is_native;      /* 0 = Khan bytecode, 1 = C native */

    UpvalueDesc *upvalues;
    int          upvalue_count;
} KhanFunction;

KhanFunction *khanfn_new(const char *name, int arity);
void          khanfn_free(KhanFunction *fn);

/* ─────────────────────────────────────────────
   KhanClosure — the runtime, ref-counted bundle of
   captured values for one closure *instance*. A
   VAL_FUNCTION Value points at one of these via its
   (repurposed) `as.function.closure` field when the
   function it wraps captures variables from an
   enclosing function. Plain (non-capturing) functions
   leave `closure` NULL, same as before.
   ───────────────────────────────────────────── */
typedef struct KhanClosure {
    int    ref_count;
    int    count;
    Value *values;
} KhanClosure;

KhanClosure *khanclosure_new(int count);
void         khanclosure_retain(KhanClosure *cl);
void         khanclosure_release(KhanClosure *cl);

#endif /* KHAN_CHUNK_H */