#ifndef KHAN_INTERPRETER_H
#define KHAN_INTERPRETER_H

#include <stddef.h> /* size_t */
#include "ast.h"

typedef struct Environment Environment;
typedef struct Interpreter Interpreter;

typedef enum {
    VAL_NUMBER,
    VAL_STRING,
    VAL_BOOL,
    VAL_NIL,
    VAL_FUNCTION,
    VAL_NATIVE,
    VAL_ARRAY,
    VAL_MAP,
    VAL_TENSOR,    /* a native n-dimensional array of doubles — real
                       contiguous storage (data/shape/ndim/size below),
                       not a nested-array simulation. See tensor_lib.c. */
    VAL_CLASS,     /* a `class Name: ...` value — its Obj's as.map holds
                       method_name -> VAL_FUNCTION entries, keyed and
                       stored exactly like a VAL_MAP (see map_get/map_set
                       in value.c). Obj.class_name is this class's name. */
    VAL_INSTANCE,  /* an instantiated object — its Obj's as.map holds
                       this instance's own fields (set via `self.x = v`),
                       stored the same way a VAL_MAP is. Obj.class_ref
                       is a retained pointer to the VAL_CLASS Obj it was
                       built from, used to resolve method calls. */
} ValueType;

struct Value;

typedef struct {
    const char *key;
    struct Value *value; // Pointers to Value to break circularity
} MapEntry;

typedef void (*NativeFn)(struct Value *result, Interpreter *interp, int argc, struct Value *args);

typedef struct Obj {
    ValueType type;
    int ref_count;
    /* VAL_CLASS only: strdup'd class name (for `str()`/`type()`/error
       messages). NULL for every other object type. */
    char *class_name;
    /* VAL_CLASS only: a retained pointer to the immediate superclass's
       Obj, from `class Name(Base):`, or NULL with no `(Base)` clause.
       Method lookup walks this chain — see class_method_lookup in
       vm.c. Single inheritance only. */
    struct Obj *super_ref;
    /* VAL_INSTANCE only: a retained (ref_count-bumped) pointer to the
       VAL_CLASS Obj this instance was built from — how a method call
       (`obj.method(args)`) finds the method to run. NULL for every
       other object type. Not itself walked by the cycle collector (see
       value_free's VAL_CLASS/VAL_INSTANCE branch) — same documented
       "known limitation" class as closures not being cycle-collected:
       a reference cycle through instance fields will leak, plain
       refcounting still reclaims everything else. */
    struct Obj *class_ref;
    /* Cycle-collector bookkeeping (see value.c's "Cycle collector"
       section for the full algorithm writeup). `color` holds the
       Bacon-Rajan trial-deletion trace state (black=live, gray=being
       traced, white=confirmed garbage, purple=possible root) and is
       ONLY ever set by gc_mark_gray()/gc_scan()/gc_scan_black() — the
       collection (free) phase deliberately never touches it, so a
       white verdict stays trustworthy for every edge in the object
       graph right up until the object is actually freed. `buffered`
       serves TWO separate, non-overlapping purposes at different
       times: while an object sits in the roots buffer it just means
       "already queued, don't add twice"; once collection reaches the
       free phase it's reused as a reentrancy guard against visiting
       the same object twice in one pass (a self-reference or a
       multi-object cycle would otherwise revisit it and recurse
       forever — this is exactly the bug this comment is warning
       about, found via an actual stack-overflow crash on `a[0] = a`
       during development; see the free-phase function's own comment
       for why `color` can't safely serve as that guard too). Both
       fields are 0/unused for the entire lifetime of any object that
       never has a decrement leave it above zero — i.e. the
       overwhelming majority of arrays/maps in a typical program never
       touch this machinery at all. */
    int color;
    int buffered;
    union {
        struct {
            struct Value *items;
            int count;
            int capacity;
        } array;
        struct {
            MapEntry *entries;   /* dense array, insertion order preserved */
            int count;
            int capacity;
            int *hash_index;     /* open-addressing index -> entries[] slot, -1 = empty */
            int hash_capacity;   /* power of 2, 0 = not yet allocated */
        } map;
        struct {
            double *data;   /* flat, row-major, owned, length == size */
            int    *shape;  /* owned, length == ndim; ndim==0 is a scalar
                                tensor (size == 1, shape == NULL) */
            int     ndim;
            int     size;   /* product of shape[], total element count */
        } tensor;
    } as;
} Obj;

typedef struct Value {
    ValueType type;
    union {
        double number;
        int boolean;
        const char *string;
        Obj *obj;
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
    } as;
} Value;

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

Value value_number(double n);
Value value_string(const char *s);
Value value_string_concat(const char *a, size_t la, const char *b, size_t lb);
Value value_bool(int b);
Value value_nil(void);
Value value_function(const char *name, Environment *closure,
                     AstNode *body, AstNodeList *params);
Value value_native(const char *name, NativeFn fn);
Value value_array(Value *items, int count);
Value value_map_empty(void);
Value value_class_new(const char *name);
Value value_instance_new(Value klass); /* klass.type must be VAL_CLASS */
Value value_tensor(const double *data, const int *shape, int ndim); /* copies both */
#define AS_TENSOR_DATA(v)  ((v).as.obj->as.tensor.data)
#define AS_TENSOR_SHAPE(v) ((v).as.obj->as.tensor.shape)
#define AS_TENSOR_NDIM(v)  ((v).as.obj->as.tensor.ndim)
#define AS_TENSOR_SIZE(v)  ((v).as.obj->as.tensor.size)
void  gc_collect_cycles(void); /* see value.c's "Cycle collector" section */

void map_set(Value *map, const char *key, Value value);
Value *map_get(Value *map, const char *key);

Value value_copy(Value v);
void value_free(Value v);
void value_print(Value v);

// ---------------------------------------------------------------------------
// Accessors for shared objects
// ---------------------------------------------------------------------------
#define AS_ARRAY_COUNT(v) ((v).as.obj->as.array.count)
#define AS_ARRAY_ITEMS(v) ((v).as.obj->as.array.items)
#define AS_MAP_COUNT(v)   ((v).as.obj->as.map.count)
#define AS_MAP_ENTRIES(v) ((v).as.obj->as.map.entries)

struct Interpreter {
    int had_runtime_error;
    const char *base_path;
    int is_returning;
    Value return_value;
    int is_breaking;
    int is_continuing;
    Environment *base_env;
    char current_import_dir[1024];
};

void interpreter_init(Interpreter *interp, const char *base_path);
Value interpreter_execute(Interpreter *interp, AstNode *node, Environment *env);

Value khan_call_fn(Interpreter *interp, Environment *env,
                   const char *fn_name, int argc, Value *argv);

#endif
