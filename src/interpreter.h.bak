#ifndef KHAN_INTERPRETER_H
#define KHAN_INTERPRETER_H

#include "ast.h"

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
typedef struct Environment Environment;
typedef struct Interpreter Interpreter;

// ---------------------------------------------------------------------------
// A native C function: takes interpreter, arg count, arg array, returns Value
// We use a struct wrapper to break the circular dependency between
// NativeFn's return type (Value) and Value's member (NativeFn pointer).
// ---------------------------------------------------------------------------
// Value types that the interpreter works with at runtime
typedef enum {
    VAL_NUMBER,
    VAL_STRING,
    VAL_BOOL,
    VAL_NIL,
    VAL_FUNCTION,
    VAL_NATIVE,
    VAL_ARRAY,
    VAL_MAP,
} ValueType;

// Forward-declare Value struct for the native function pointer
struct Value;
typedef struct MapEntry MapEntry;

// Native function signature
typedef void (*NativeFn)(struct Value *result, Interpreter *interp, int argc, struct Value *args);

// Now define Value
struct Value {
    ValueType type;
    union {
        double number;
        const char *string;
        int boolean;
        struct {
            const char *name;
            Environment *closure;
            AstNode *body;
            AstNodeList *params;
        } function;
        struct {
            const char *name;
            NativeFn function;
        } native;
        struct {
            struct Value *items;
            int count;
            int capacity;
        } array;
        struct {
            MapEntry *entries;
            int count;
            int capacity;
        } map;
    } as;
};

// Typedef for convenience
typedef struct Value Value;

// One key-value pair inside a map. Defined here (after Value) since each
// entry owns a full Value, not just a pointer to one.
struct MapEntry {
    const char *key;
    Value value;
};

// ---------------------------------------------------------------------------
// Environment (scope chain)
// ---------------------------------------------------------------------------
struct Environment {
    struct EnvEntry {
        const char *name;
        Value value;
    } *entries;
    int count;
    int capacity;
    struct Environment *parent;
};

Environment *env_new(Environment *parent);
void env_free(Environment *env);
void env_define(Environment *env, const char *name, Value value);
Value *env_get(Environment *env, const char *name);
void env_assign(Environment *env, const char *name, Value value);

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------
Value value_number(double n);
Value value_string(const char *s);
Value value_bool(int b);
Value value_nil(void);
Value value_function(const char *name, Environment *closure,
                     AstNode *body, AstNodeList *params);
Value value_native(const char *name, NativeFn fn);
Value value_array(Value *items, int count);
Value value_map_empty(void);
// Sets key to value in the map (overwrites if key already exists). Takes
// ownership of `value` — pass a value_copy() if the caller still needs it.
void map_set(Value *map, const char *key, Value value);
// Returns a pointer to the stored Value for `key`, or NULL if not present.
Value *map_get(Value *map, const char *key);
Value value_copy(Value v);
void value_free(Value v);
void value_print(Value v);

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------
struct Interpreter {
    int had_runtime_error;
    const char *base_path;  // directory of the main source file, for resolving imports

    // Early-return signaling. When a `return` statement executes, it sets
    // is_returning and stashes its value here. Every block-executing
    // function (execute_block, if/while/for bodies) must check this flag
    // after running each statement and stop immediately if it's set, so
    // the signal correctly unwinds all the way out to the enclosing
    // function call, instead of just stopping the innermost block.
    int is_returning;
    Value return_value;
    int is_breaking;
    int is_continuing;
};

void interpreter_init(Interpreter *interp, const char *base_path);
Value interpreter_execute(Interpreter *interp, AstNode *node, Environment *env);

#endif