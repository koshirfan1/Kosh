#ifndef KHAN_VM_H
#define KHAN_VM_H

#define _POSIX_C_SOURCE 200809L
#include <string.h>   /* strdup */
#include <stdlib.h>   /* malloc, free */
#include "chunk.h"
#include "value.h"

#define VM_STACK_MAX  16384
#define VM_FRAMES_MAX  1024
#define VM_TRY_MAX     64  /* max nested active try blocks */

/* One activation record on the call stack */
typedef struct {
    KhanFunction *fn;
    uint8_t      *ip;       /* instruction pointer into fn->chunk.code */
    Value        *slots;    /* base of this frame's locals on vm.stack  */
    Value        *upvalues; /* captured values for this closure instance
                                (borrowed from the KhanClosure the callee
                                Value pointed at; NULL for non-capturing
                                functions) — indexed by fn->upvalues[]   */
    int          is_method_call; /* 1 if pushed by OP_CALL_METHOD: `self`
                                (the receiver) sits at slots[0] with NO
                                hidden callee slot below it, so OP_RETURN
                                must truncate the stack to slots, not
                                slots - 1. 0 for every other frame.     */
    int          is_constructor; /* 1 if this frame is a class's __init__
                                run inline by OP_CALL: on OP_RETURN the
                                frame yields `self` (slots[0]) instead of
                                the function's own return value. Always
                                set together with is_method_call.       */
} CallFrame;

/* One active try/catch handler. Registered by OP_TRY_BEGIN, retired by
 * OP_TRY_END on normal completion, or consumed by an unwind (OP_THROW
 * or a caught runtime_error()) which jumps straight to catch_ip after
 * rewinding the VM's stack and call-frame depth back to what they were
 * at OP_TRY_BEGIN time. See vm.c's OP_TRY_BEGIN case for the full design
 * writeup, and the "known limitation" note there about exceptions
 * thrown from inside a native-callback context (e.g. array.map) not
 * being catchable by a handler in an enclosing, separately-recursed
 * run_loop() call. */
typedef struct {
    uint8_t *catch_ip;
    Value   *stack_top;
    int      frame_count;
} TryHandler;

/* ── Global variable store (Hash Table) ── */
typedef struct {
    char  *key;
    Value  val;
} TableEntry;

typedef struct {
    int         count;
    int         capacity;
    TableEntry *entries;
} Table;

/* ── The VM ── */
typedef struct {
    /* Fields shared with Interpreter for native library compatibility */
    int       had_runtime_error;
    const char *base_path;
    int       is_returning;   /* unused in VM loop directly */
    Value     return_value;   /* unused in VM loop directly */
    int       is_breaking;    /* unused in VM loop directly */
    int       is_continuing;  /* unused in VM loop directly */
    void     *base_env;       /* NULL in VM mode */
    char      current_import_dir[1024];

    /* VM specific fields */
    CallFrame frames[VM_FRAMES_MAX];
    int       frame_count;

    Value     stack[VM_STACK_MAX];
    Value    *stack_top;

    Table     globals;

    TryHandler try_handlers[VM_TRY_MAX];
    int        try_handler_count;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
    INTERPRET_CAUGHT,   /* internal to run_loop only — a runtime_error()
                           was caught by an active try handler and control
                           should resume in the loop, not return to the
                           caller of vm_run()/vm_call_fn(). Never appears
                           outside vm.c. */
} InterpretResult;

void            vm_init(VM *vm);
void            vm_free(VM *vm);

/* Register Khan's stdlib native functions into the VM's global table */
void            vm_register_builtins(VM *vm);
void            vm_global_set_native(VM *vm, const char *name, NativeFn fn);
void            vm_global_set(VM *vm, const char *name, Value val);

void json_register_all_vm(VM *vm);
void datetime_register_all_vm(VM *vm);
void requests_register_all_vm(VM *vm);
void webi_register_all_vm(VM *vm);

/* Run a compiled script function */
InterpretResult vm_run(VM *vm, KhanFunction *script);

/* Call a Khan function by name via the VM */
Value           vm_call_fn(VM *vm, const char *name, int argc, Value *args);

/* Used by compiler to register KhanFunction objects */
void khanfn_register(KhanFunction *fn);
int  khanfn_registry_index(void);

#endif /* KHAN_VM_H */