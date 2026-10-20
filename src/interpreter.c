#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#ifndef __cplusplus
#ifdef _WIN32
extern __declspec(dllimport) char * __cdecl getenv(const char *);
#endif
#endif
#include <string.h>
#include <math.h>
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"

// ---------------------------------------------------------------------------
// Runtime error reporting
// ---------------------------------------------------------------------------
static void runtime_error(Interpreter *interp, AstNode *node, const char *msg) {
    fprintf(stderr, "[line %d] Runtime error: %s\n", node->line, msg);
    interp->had_runtime_error = 1;
}

// ---------------------------------------------------------------------------
// Environment implementation
// ---------------------------------------------------------------------------
static void env_grow(Environment *env) {
    if (env->count < env->capacity) return;
    env->capacity = env->capacity ? env->capacity * 2 : 8;
    env->entries = realloc(env->entries, sizeof(env->entries[0]) * env->capacity);
}

Environment *env_new(Environment *parent) {
    Environment *env = malloc(sizeof(Environment));
    env->entries = NULL;
    env->count = 0;
    env->capacity = 0;
    env->parent = parent;
    return env;
}

void env_free(Environment *env) {
    if (!env) return;
    for (int i = 0; i < env->count; i++) {
        free((void *)env->entries[i].name);
        value_free(env->entries[i].value);
    }
    free(env->entries);
    free(env);
}

void env_define(Environment *env, const char *name, Value value) {
    // If `name` already exists in THIS exact scope, overwrite it instead of
    // appending a duplicate entry. This matters a lot in practice: Khan
    // doesn't create a new scope per loop iteration, so `let x = ...`
    // re-declared inside a while/for body would otherwise pile up duplicate
    // entries every iteration — and env_get() scans from index 0 upward, so
    // it would always return the OLDEST entry, leaving `x` permanently
    // stuck at its first-ever value no matter how many times the loop runs.
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) {
            value_free(env->entries[i].value);
            env->entries[i].value = value;
            return;
        }
    }
    env_grow(env);
    env->entries[env->count].name = strdup(name);
    env->entries[env->count].value = value;
    env->count++;
}

Value *env_get(Environment *env, const char *name) {
    for (Environment *e = env; e; e = e->parent) {
        for (int i = 0; i < e->count; i++) {
            if (strcmp(e->entries[i].name, name) == 0) {
                return &e->entries[i].value;
            }
        }
    }
    return NULL;
}

void env_assign(Environment *env, const char *name, Value value) {
    for (Environment *e = env; e; e = e->parent) {
        for (int i = 0; i < e->count; i++) {
            if (strcmp(e->entries[i].name, name) == 0) {
                value_free(e->entries[i].value);
                e->entries[i].value = value;
                return;
            }
        }
    }
    // If not found, define in current scope
    env_define(env, name, value);
}

// Forward-declare the import executor so we can forward-reference it
// without having main.c's read_file helper here. We'll read the file inline.
static Value execute_import(Interpreter *interp, const char *path, Environment *env);
static Value execute_from_import(Interpreter *interp, const char *path,
                                  char **names, int name_count, Environment *env);

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static Value evaluate(Interpreter *interp, AstNode *node, Environment *env);
static Value execute_block(Interpreter *interp, AstNodeList *stmts,
                           Environment *env);
static Value *resolve_lvalue_target(Interpreter *interp, AstNode *node, Environment *env);

// ---------------------------------------------------------------------------
// Resolve the target of an index-assignment down to a mutable pointer into
// its actual storage — walking through however many levels of [..][..] the
// left-hand side has (m["a"]["b"] = v, arr[0]["x"] = v, etc.), not just a
// bare identifier. Maps/arrays are mutated in place (map_get/array items
// return pointers into the real storage), so once we reach the innermost
// container, writing through the returned pointer is enough; no separate
// "write back" step is needed at any level of the chain.
// ---------------------------------------------------------------------------
static Value *resolve_lvalue_target(Interpreter *interp, AstNode *node, Environment *env) {
    if (node->type == AST_IDENTIFIER) {
        Value *target = env_get(env, node->data.name);
        if (!target) {
            runtime_error(interp, node, "Undefined variable.");
            return NULL;
        }
        return target;
    }

    if (node->type == AST_INDEX) {
        Value *container = resolve_lvalue_target(interp, node->data.index_expr.object, env);
        if (!container || interp->had_runtime_error) return NULL;

        if (container->type == VAL_MAP) {
            Value key_val = evaluate(interp, node->data.index_expr.index, env);
            if (interp->had_runtime_error) return NULL;
            if (key_val.type != VAL_STRING) {
                runtime_error(interp, node, "Map key must be a string.");
                value_free(key_val);
                return NULL;
            }
            Value *found = map_get(container, key_val.as.string);
            value_free(key_val);
            if (!found) {
                runtime_error(interp, node, "Key not found in map.");
                return NULL;
            }
            return found;
        }

        if (container->type == VAL_ARRAY) {
            Value idx_val = evaluate(interp, node->data.index_expr.index, env);
            if (interp->had_runtime_error) return NULL;
            if (idx_val.type != VAL_NUMBER) {
                runtime_error(interp, node, "Array index must be a number.");
                value_free(idx_val);
                return NULL;
            }
            int idx = (int)idx_val.as.number;
            value_free(idx_val);
            if (idx < 0 || idx >= AS_ARRAY_COUNT(*container)) {
                runtime_error(interp, node, "Array index out of bounds.");
                return NULL;
            }
            return &AS_ARRAY_ITEMS(*container)[idx];
        }

        runtime_error(interp, node, "Can only index into an array or map.");
        return NULL;
    }

    runtime_error(interp, node,
        "Can only assign into a variable, or an index/key of one (e.g. x[i] = v).");
    return NULL;
}

// ---------------------------------------------------------------------------
// Expression evaluation
// ---------------------------------------------------------------------------
static Value evaluate(Interpreter *interp, AstNode *node, Environment *env) {
    if (interp->had_runtime_error) return value_nil();

    switch (node->type) {
        case AST_NUMBER:
            return value_number(node->data.number_value);

        case AST_BOOL:
            return value_bool(node->data.bool_value);

        case AST_NIL:
            return value_nil();

        case AST_STRING:
            return value_string(node->data.string_value);

        case AST_IDENTIFIER: {
            Value *v = env_get(env, node->data.name);
            if (!v) {
                runtime_error(interp, node, "Undefined variable.");
                return value_nil();
            }
            return value_copy(*v);
        }

        case AST_BINARY: {
            // AND / OR must short-circuit: for `a and b`, if `a` is
            // falsy, `b` is never evaluated (and vice versa for `or`).
            // This matters for more than just performance — it's the
            // standard "guard" pattern (`has(map, k) and map[k] != nil`)
            // that safely checks a key exists before indexing it. Without
            // short-circuiting, the right-hand side always runs too, so
            // that guard would still throw "Key not found in map" on a
            // missing key — the exact case it's written to prevent.
            if (node->data.binary.op == OP_AND || node->data.binary.op == OP_OR) {
                Value left = evaluate(interp, node->data.binary.left, env);
                if (interp->had_runtime_error) return value_nil();
                int lb = (left.type == VAL_BOOL) ? left.as.boolean :
                         (left.type == VAL_NUMBER && left.as.number != 0);

                if (node->data.binary.op == OP_AND && !lb) return value_bool(0);
                if (node->data.binary.op == OP_OR  &&  lb) return value_bool(1);

                Value right = evaluate(interp, node->data.binary.right, env);
                if (interp->had_runtime_error) return value_nil();
                int rb = (right.type == VAL_BOOL) ? right.as.boolean :
                         (right.type == VAL_NUMBER && right.as.number != 0);
                return value_bool(rb);
            }

            Value left = evaluate(interp, node->data.binary.left, env);
            if (interp->had_runtime_error) return value_nil();
            Value right = evaluate(interp, node->data.binary.right, env);
            if (interp->had_runtime_error) return value_nil();

            // Number-only arithmetic operators
            if (left.type == VAL_NUMBER && right.type == VAL_NUMBER) {
                double a = left.as.number;
                double b = right.as.number;
                switch (node->data.binary.op) {
                    case OP_PLUS:           return value_number(a + b);
                    case OP_MINUS:          return value_number(a - b);
                    case OP_STAR:           return value_number(a * b);
                    case OP_SLASH:
                        if (b == 0) {
                            runtime_error(interp, node, "Division by zero.");
                            return value_nil();
                        }
                        return value_number(a / b);
                    case OP_PERCENT:
                        if (b == 0) {
                            runtime_error(interp, node, "Division by zero.");
                            return value_nil();
                        }
                        return value_number(fmod(a, b));
                    case OP_LESS:           return value_bool(a < b);
                    case OP_LESS_EQUAL:     return value_bool(a <= b);
                    case OP_GREATER:        return value_bool(a > b);
                    case OP_GREATER_EQUAL:  return value_bool(a >= b);
                    case OP_EQUAL_EQUAL:    return value_bool(a == b);
                    case OP_BANG_EQUAL:     return value_bool(a != b);
                    default: break;
                }
            }

            // String concatenation with '+'
            if (left.type == VAL_STRING && right.type == VAL_STRING &&
                node->data.binary.op == OP_PLUS) {
                char *result = malloc(strlen(left.as.string) +
                                      strlen(right.as.string) + 1);
                strcpy(result, left.as.string);
                strcat(result, right.as.string);
                Value v = value_string(result);
                free(result);
                return v;
            }

            // Equality can compare any two values of the same type
            if (node->data.binary.op == OP_EQUAL_EQUAL ||
                node->data.binary.op == OP_BANG_EQUAL) {
                int eq = 0;
                if (left.type == right.type) {
                    switch (left.type) {
                        case VAL_NUMBER:
                            eq = left.as.number == right.as.number;
                            break;
                        case VAL_STRING:
                            eq = strcmp(left.as.string, right.as.string) == 0;
                            break;
                        case VAL_BOOL:
                            eq = left.as.boolean == right.as.boolean;
                            break;
                        case VAL_NIL:
                            eq = 1;
                            break;
                        default:
                            eq = 0;
                            break;
                    }
                }
                return value_bool(node->data.binary.op == OP_EQUAL_EQUAL ? eq : !eq);
            }

            runtime_error(interp, node, "Type mismatch in binary expression.");
            return value_nil();
        }

        case AST_UNARY: {
            Value right = evaluate(interp, node->data.unary.right, env);
            if (interp->had_runtime_error) return value_nil();

            if (node->data.unary.op == OP_NEGATE) {
                if (right.type != VAL_NUMBER) {
                    runtime_error(interp, node, "Negation requires a number.");
                    return value_nil();
                }
                return value_number(-right.as.number);
            }

            if (node->data.unary.op == OP_NOT) {
                int truthy = (right.type == VAL_NUMBER && right.as.number != 0) ||
                             (right.type == VAL_BOOL && right.as.boolean) ||
                             (right.type == VAL_STRING && strlen(right.as.string) > 0);
                return value_bool(!truthy);
            }

            runtime_error(interp, node, "Unknown unary operator.");
            return value_nil();
        }

        case AST_GROUPING:
            return evaluate(interp, node->data.grouping, env);

        case AST_IMPORT_STMT:
            return execute_import(interp, node->data.import_path, env);

        case AST_FROM_IMPORT_STMT:
            return execute_from_import(interp, node->data.from_import.path,
                                        node->data.from_import.names,
                                        node->data.from_import.name_count, env);

        case AST_ASSIGNMENT: {
            Value v = evaluate(interp, node->data.assignment.value, env);
            if (!interp->had_runtime_error) {
                env_assign(env, node->data.assignment.var_name, v);
            }
            return v;
        }

        case AST_CALL: {
            // Look up the function — either by name (fast path) or by
            // evaluating an arbitrary expression (e.g. arr[i], map["fn"]).
            Value callee_storage;   // holds the evaluated value when callee_expr is used
            Value *callee;

            if (node->data.call.callee) {
                callee = env_get(env, node->data.call.callee);
                if (!callee) {
                    runtime_error(interp, node, "Undefined function.");
                    return value_nil();
                }
            } else {
                callee_storage = evaluate(interp, node->data.call.callee_expr, env);
                if (interp->had_runtime_error) {
                    value_free(callee_storage);
                    return value_nil();
                }
                callee = &callee_storage;
            }

            // Evaluate arguments
            int argc = 0;
            for (AstNodeList *a = node->data.call.arguments; a; a = a->next)
                argc++;

            Value *argv = malloc(sizeof(Value) * argc);
            int i = 0;
            for (AstNodeList *a = node->data.call.arguments; a; a = a->next) {
                argv[i] = evaluate(interp, a->node, env);
                if (interp->had_runtime_error) {
                    free(argv);
                    return value_nil();
                }
                i++;
            }

            // Handle native function calls
            if (callee->type == VAL_NATIVE) {
                Value result;
                callee->as.native.function(&result, interp, argc, argv);
                free(argv);
                return result;
            }

            if (callee->type != VAL_FUNCTION) {
                runtime_error(interp, node, "Can only call functions.");
                free(argv);
                return value_nil();
            }

            // Check arity
            int param_count = 0;
            for (AstNodeList *p = callee->as.function.params; p; p = p->next)
                param_count++;

            if (argc != param_count) {
                runtime_error(interp, node, "Argument count mismatch.");
                free(argv);
                return value_nil();
            }

            // Create function call environment
            Environment *call_env = env_new(callee->as.function.closure);

            // Bind parameters
            AstNodeList *p = callee->as.function.params;
            for (int j = 0; j < argc; j++, p = p->next) {
                env_define(call_env, p->node->data.name, argv[j]);
            }
            free(argv);

            // Execute function body
            Value result = execute_block(interp,
                callee->as.function.body->data.statements, call_env);

            // If a `return` fired anywhere inside the body (even nested
            // inside if/while/for), use its value explicitly, then clear
            // the signal — it's now been "handled" by this call, and must
            // not keep propagating into whatever loop/block called this
            // function in the first place.
            if (interp->is_returning) {
                value_free(result);
                result = interp->return_value;
                interp->is_returning = 0;
                interp->return_value = value_nil();
            }

            env_free(call_env);
            return result;
        }

        case AST_ARRAY: {
            int count = 0;
            for (AstNodeList *e = node->data.array_elements; e; e = e->next) count++;

            Value *items = count > 0 ? malloc(sizeof(Value) * count) : NULL;
            int i = 0;
            for (AstNodeList *e = node->data.array_elements; e; e = e->next) {
                items[i] = evaluate(interp, e->node, env);
                if (interp->had_runtime_error) {
                    for (int j = 0; j < i; j++) value_free(items[j]);
                    free(items);
                    return value_nil();
                }
                i++;
            }
            return value_array(items, count);
        }

        case AST_MAP: {
            Value map = value_map_empty();
            for (AstNodeList *e = node->data.map_entries; e; e = e->next) {
                AstNode *entry = e->node;

                Value key_val = evaluate(interp, entry->data.map_entry.key, env);
                if (interp->had_runtime_error) { value_free(map); return value_nil(); }
                if (key_val.type != VAL_STRING) {
                    runtime_error(interp, entry, "Map keys must be strings.");
                    value_free(key_val);
                    value_free(map);
                    return value_nil();
                }

                Value val = evaluate(interp, entry->data.map_entry.value, env);
                if (interp->had_runtime_error) {
                    value_free(key_val);
                    value_free(map);
                    return value_nil();
                }

                map_set(&map, key_val.as.string, val);
                value_free(key_val);
            }
            return map;
        }

        case AST_INDEX: {
            Value obj = evaluate(interp, node->data.index_expr.object, env);
            if (interp->had_runtime_error) return value_nil();

            if (obj.type == VAL_MAP) {
                Value key_val = evaluate(interp, node->data.index_expr.index, env);
                if (interp->had_runtime_error) { value_free(obj); return value_nil(); }
                if (key_val.type != VAL_STRING) {
                    runtime_error(interp, node, "Map key must be a string.");
                    value_free(obj);
                    value_free(key_val);
                    return value_nil();
                }
                Value *found = map_get(&obj, key_val.as.string);
                if (!found) {
                    runtime_error(interp, node, "Key not found in map.");
                    value_free(obj);
                    value_free(key_val);
                    return value_nil();
                }
                Value result = value_copy(*found);
                value_free(key_val);
                value_free(obj);
                return result;
            }

            if (obj.type != VAL_ARRAY) {
                runtime_error(interp, node, "Can only index into an array or map.");
                value_free(obj);
                return value_nil();
            }

            Value idx_val = evaluate(interp, node->data.index_expr.index, env);
            if (interp->had_runtime_error) {
                value_free(obj);
                return value_nil();
            }
            if (idx_val.type != VAL_NUMBER) {
                runtime_error(interp, node, "Array index must be a number.");
                value_free(obj);
                value_free(idx_val);
                return value_nil();
            }

            int idx = (int)idx_val.as.number;
            value_free(idx_val);

            if (!obj.as.obj || idx < 0 || idx >= obj.as.obj->as.array.count) {
                runtime_error(interp, node, "Array index out of bounds.");
                value_free(obj);
                return value_nil();
            }

            Value result = value_copy(obj.as.obj->as.array.items[idx]);
            value_free(obj);
            return result;
        }

        case AST_INDEX_ASSIGN: {
            Value *target = resolve_lvalue_target(interp, node->data.index_assign.object, env);
            if (!target || interp->had_runtime_error) return value_nil();

            if (target->type == VAL_MAP) {
                Value key_val = evaluate(interp, node->data.index_assign.index, env);
                if (interp->had_runtime_error) return value_nil();
                if (key_val.type != VAL_STRING) {
                    runtime_error(interp, node, "Map key must be a string.");
                    value_free(key_val);
                    return value_nil();
                }

                Value new_val = evaluate(interp, node->data.index_assign.value, env);
                if (interp->had_runtime_error) { value_free(key_val); return value_nil(); }

                map_set(target, key_val.as.string, value_copy(new_val));
                value_free(key_val);
                return new_val;
            }

            if (target->type != VAL_ARRAY) {
                runtime_error(interp, node, "Can only index-assign into an array or map.");
                return value_nil();
            }
            Value *arr_ptr = target;

            Value idx_val = evaluate(interp, node->data.index_assign.index, env);
            if (interp->had_runtime_error) return value_nil();
            if (idx_val.type != VAL_NUMBER) {
                runtime_error(interp, node, "Array index must be a number.");
                value_free(idx_val);
                return value_nil();
            }
            int idx = (int)idx_val.as.number;
            value_free(idx_val);

            if (!arr_ptr->as.obj || idx < 0 || idx >= arr_ptr->as.obj->as.array.count) {
                runtime_error(interp, node, "Array index out of bounds.");
                return value_nil();
            }

            Value new_val = evaluate(interp, node->data.index_assign.value, env);
            if (interp->had_runtime_error) return value_nil();

            value_free(arr_ptr->as.obj->as.array.items[idx]);
            arr_ptr->as.obj->as.array.items[idx] = value_copy(new_val);
            return new_val;
        }

        default:
            runtime_error(interp, node, "Unexpected node type in expression.");
            return value_nil();
    }
}

// ---------------------------------------------------------------------------
// Statement execution
// ---------------------------------------------------------------------------
static Value execute_block(Interpreter *interp, AstNodeList *stmts,
                           Environment *env) {
    Value result = value_nil();
    for (AstNodeList *cur = stmts;
         cur && !interp->had_runtime_error && !interp->is_returning
             && !interp->is_breaking && !interp->is_continuing;
         cur = cur->next) {
        result = interpreter_execute(interp, cur->node, env);
    }
    return result;
}

Value interpreter_execute(Interpreter *interp, AstNode *node, Environment *env) {
    if (interp->had_runtime_error) return value_nil();

    switch (node->type) {
        case AST_PROGRAM:
            return execute_block(interp, node->data.program_stmts, env);

        case AST_BLOCK:
            return execute_block(interp, node->data.statements, env);

        case AST_EXPR_STMT:
            return evaluate(interp, node->data.expr, env);

        case AST_PRINT_STMT: {
            Value v = evaluate(interp, node->data.expr, env);
            if (!interp->had_runtime_error) {
                value_print(v);
                printf("\n");
            }
            return value_nil();
        }

        case AST_LET_STMT: {
            const char *name = node->data.let_decl.let_name;
            Value initializer = value_nil();
            if (node->data.let_decl.let_initializer) {
                initializer = evaluate(interp, node->data.let_decl.let_initializer, env);
                if (interp->had_runtime_error) return value_nil();
            }
            env_define(env, name, initializer);
            return value_nil();
        }

        case AST_IF_STMT: {
            Value cond = evaluate(interp, node->data.if_stmt.condition, env);
            if (interp->had_runtime_error) return value_nil();

            int truthy = vm_is_truthy(cond);

            if (truthy) {
                return interpreter_execute(interp, node->data.if_stmt.then_branch, env);
            } else if (node->data.if_stmt.else_branch) {
                return interpreter_execute(interp, node->data.if_stmt.else_branch, env);
            }
            return value_nil();
        }

        case AST_WHILE_STMT: {
            Value result = value_nil();
            for (;;) {
                Value cond = evaluate(interp, node->data.while_stmt.while_condition, env);
                if (interp->had_runtime_error) return value_nil();

                int truthy = (cond.type == VAL_NUMBER && cond.as.number != 0) ||
                             (cond.type == VAL_BOOL && cond.as.boolean) ||
                             (cond.type == VAL_STRING && strlen(cond.as.string) > 0);
                if (!truthy) break;

                result = interpreter_execute(interp, node->data.while_stmt.while_body, env);
                if (interp->had_runtime_error) return value_nil();
                if (interp->is_returning) break;
                if (interp->is_breaking) { interp->is_breaking = 0; break; }
                if (interp->is_continuing) { interp->is_continuing = 0; continue; }
            }
            return result;
        }

        case AST_FOR_STMT: {
            Value iterable = evaluate(interp, node->data.for_stmt.for_iterable, env);
            if (interp->had_runtime_error) return value_nil();

            if (iterable.type != VAL_ARRAY) {
                runtime_error(interp, node, "Can only iterate ('for ... in ...') over an array.");
                value_free(iterable);
                return value_nil();
            }

            Value result = value_nil();
            if (iterable.as.obj) {
                for (int i = 0; i < iterable.as.obj->as.array.count; i++) {
                    env_assign(env, node->data.for_stmt.for_var,
                               value_copy(iterable.as.obj->as.array.items[i]));

                    result = interpreter_execute(interp, node->data.for_stmt.for_body, env);
                    if (interp->had_runtime_error) {
                        value_free(iterable);
                        return value_nil();
                    }
                    if (interp->is_returning) break;
                    if (interp->is_breaking) { interp->is_breaking = 0; break; }
                    if (interp->is_continuing) { interp->is_continuing = 0; continue; }
                }
            }
            value_free(iterable);
            return result;
        }

        case AST_FN_DECL: {
            // Capture current environment as closure
            Environment *closure = env_new(env->parent);
            // Copy current scope entries into closure
            for (int i = 0; i < env->count; i++) {
                env_define(closure, env->entries[i].name, value_copy(env->entries[i].value));
            }

            Value fn = value_function(node->data.fn_decl.fn_name,
                                      closure,
                                      node->data.fn_decl.fn_body,
                                      node->data.fn_decl.params);
            // Inject the function into its own closure so recursive calls work
            env_define(closure, node->data.fn_decl.fn_name, value_copy(fn));
            env_define(env, node->data.fn_decl.fn_name, fn);
            return value_nil();
        }

        case AST_RETURN_STMT: {
            Value v = node->data.expr
                ? evaluate(interp, node->data.expr, env)
                : value_nil();
            if (!interp->had_runtime_error) {
                interp->is_returning = 1;
                interp->return_value = value_copy(v);
            }
            return v;
        }

        case AST_BREAK_STMT:
            interp->is_breaking = 1;
            return value_nil();

        case AST_CONTINUE_STMT:
            interp->is_continuing = 1;
            return value_nil();

        case AST_ASSIGNMENT: {
            Value v = evaluate(interp, node->data.assignment.value, env);
            if (!interp->had_runtime_error) {
                env_assign(env, node->data.assignment.var_name, v);
            }
            return v;
        }

        default: {
            // Try evaluating as expression
            return evaluate(interp, node, env);
        }
    }
}

// ---------------------------------------------------------------------------
// Shared import-path resolution: given a raw `import`/`from` module
// reference, find the file on disk (relative path, path as-is, or an
// installed package under ~/.khan/packages/<name>/<name>.kh), open it,
// and return the parsed AST + full path used.
//
// On failure, sets interp->had_runtime_error and returns NULL (out_program
// is left untouched).
// ---------------------------------------------------------------------------
// Sets interp->current_import_dir to the directory portion of full_path
// (everything before the last '/' or '\'), or "" if full_path has no
// directory component (e.g. a bare filename resolved via cwd).
static void set_import_dir_from_file(Interpreter *interp, const char *full_path) {
    const char *last_slash = strrchr(full_path, '/');
    const char *last_bslash = strrchr(full_path, '\\');
    const char *sep = (last_slash > last_bslash) ? last_slash : last_bslash;
    if (sep) {
        size_t len = (size_t)(sep - full_path);
        if (len >= sizeof(interp->current_import_dir)) len = sizeof(interp->current_import_dir) - 1;
        memcpy(interp->current_import_dir, full_path, len);
        interp->current_import_dir[len] = '\0';
    } else {
        interp->current_import_dir[0] = '\0';
    }
}

// Directory portion of full_path (everything before the last '/' or '\'),
// or "" if there's no directory component. Same rule as
// set_import_dir_from_file, but written into caller-supplied storage
// instead of interp state — used when we need "the directory the module
// we just imported from lives in" *after* interp->current_import_dir has
// already been restored to whatever it was before that import.
static void dirname_of(const char *full_path, char *out, size_t cap) {
    const char *last_slash = strrchr(full_path, '/');
    const char *last_bslash = strrchr(full_path, '\\');
    const char *sep = (last_slash > last_bslash) ? last_slash : last_bslash;
    if (sep) {
        size_t len = (size_t)(sep - full_path);
        if (len >= cap) len = cap - 1;
        memcpy(out, full_path, len);
        out[len] = '\0';
    } else {
        out[0] = '\0';
    }
}

// Copies every *public* (non "_"-prefixed) top-level name from a module's
// own environment into a target scope. This is how "from X import Y" is
// able to flatten an entire submodule file (or an entire aggregate
// package, for the self-name case) instead of just pulling out a single
// pre-defined symbol — same mechanism the interpreter already uses to
// snapshot scope into a closure (see AST_FN_DECL), just aimed at a
// different destination and filtered for privacy.
static void flatten_public_names(Environment *target, Environment *module_env) {
    for (int i = 0; i < module_env->count; i++) {
        const char *name = module_env->entries[i].name;
        if (name[0] == '_') continue;
        env_define(target, name, value_copy(module_env->entries[i].value));
    }
}

static AstNode *resolve_and_parse_import(Interpreter *interp, const char *path,
                                          char *full_path_out, size_t full_path_cap) {
    char full_path[1024];
    FILE *file = NULL;

    // If we're currently executing an imported module (current_import_dir
    // is set), look for sibling files next to *that* module first. This is
    // what lets a package split across multiple files (e.g. packages/webi/
    // with webi.kh importing "route", "request", "response", ...) resolve
    // correctly no matter where the user's top-level script lives — without
    // this, a nested `import "sibling"` would incorrectly resolve relative
    // to the top-level script's directory (base_path) instead of the
    // package's own directory.
    if (interp->current_import_dir[0] != '\0') {
        snprintf(full_path, sizeof(full_path), "%s/%s", interp->current_import_dir, path);
        file = fopen(full_path, "rb");
    }

    if (!file) {
        if (interp->base_path) {
            snprintf(full_path, sizeof(full_path), "%s/%s", interp->base_path, path);
        } else {
            snprintf(full_path, sizeof(full_path), "%s", path);
        }
        file = fopen(full_path, "rb");
    }
    if (!file) {
        // Try path as-is
        file = fopen(path, "rb");
        if (file) {
            strncpy(full_path, path, sizeof(full_path) - 1);
            full_path[sizeof(full_path) - 1] = '\0';
        }
    }
    if (!file) {
        // Try as a package name: ~/.khan/packages/<name>/<name>.kh
        // Bare package name: no path separators, no .kh extension
        int is_pkg_name = 1;
        for (const char *p = path; *p; p++) {
            if (*p == '/' || *p == '\\' || *p == '.') { is_pkg_name = 0; break; }
        }
        if (is_pkg_name) {
            char pkg_path[1024];
            char *home = NULL;
#ifdef _WIN32
            home = getenv("USERPROFILE");
            if (!home) home = "C:\\Users\\Default";
            snprintf(pkg_path, sizeof(pkg_path),
                     "%s\\.khan\\packages\\%s\\%s.kh", home, path, path);
#else
            home = getenv("HOME");
            if (!home) home = "/tmp";
            snprintf(pkg_path, sizeof(pkg_path),
                     "%s/.khan/packages/%s/%s.kh", home, path, path);
#endif
            file = fopen(pkg_path, "rb");
            if (file) {
                strncpy(full_path, pkg_path, sizeof(full_path) - 1);
                full_path[sizeof(full_path) - 1] = '\0';
            }
        }
    }
    if (!file) {
        fprintf(stderr, "[line 0] Runtime error: Could not open import '%s'.\n"
                        "  Tip: if \"%s\" is a package, run: kh install %s\n",
                full_path, path, path);
        interp->had_runtime_error = 1;
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char *source = malloc(size + 1);
    if (!source) {
        fprintf(stderr, "[line 0] Runtime error: Out of memory reading import '%s'.\n", full_path);
        fclose(file);
        interp->had_runtime_error = 1;
        return NULL;
    }
    fread(source, 1, size, file);
    source[size] = '\0';
    fclose(file);

    // Lex, parse the imported file
    Lexer lexer;
    lexer_init(&lexer, source);

    Parser parser;
    parser_init(&parser, &lexer, full_path);

    AstNode *program = parser_parse(&parser);

    if (parser.had_error) {
        fprintf(stderr, "[line 0] Runtime error: Syntax error(s) in import '%s'.\n", full_path);
        free(source);
        ast_free(program);
        interp->had_runtime_error = 1;
        return NULL;
    }

    // NOTE: We deliberately do NOT free `program` or `source` here.
    // Functions declared in the imported file hold raw pointers into this
    // AST (body/params aren't copied — see value_function/value_free).
    // Freeing it now would leave any imported function with a dangling
    // body pointer the moment it's called later in the importing script.
    // This matches how the main script's own AST is kept alive for the
    // whole process (see main.c) — a small intentional leak, not a crash.
    if (full_path_out) {
        strncpy(full_path_out, full_path, full_path_cap - 1);
        full_path_out[full_path_cap - 1] = '\0';
    }
    return program;
}

// ---------------------------------------------------------------------------
// Import executor — reads a file, parses it, executes it in the given env
// (plain `import "path"` — everything the module defines lands directly
// in the caller's scope, same as before)
// ---------------------------------------------------------------------------
static Value execute_import(Interpreter *interp, const char *path, Environment *env) {
    char full_path[1024];
    AstNode *program = resolve_and_parse_import(interp, path, full_path, sizeof(full_path));
    if (!program) return value_nil();

    // Push this module's own directory as the import-resolution base for
    // anything *it* imports, then restore whatever was there before once
    // we're done (see resolve_and_parse_import / struct field comment).
    char saved_dir[1024];
    strncpy(saved_dir, interp->current_import_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = '\0';
    set_import_dir_from_file(interp, full_path);

    Value result = interpreter_execute(interp, program, env);

    strncpy(interp->current_import_dir, saved_dir, sizeof(interp->current_import_dir) - 1);
    interp->current_import_dir[sizeof(interp->current_import_dir) - 1] = '\0';

    // A top-level `return` in an imported file's own code (rare, but
    // possible) is meaningless once we're back in the importing script —
    // it shouldn't be left set, or it would incorrectly cut short whatever
    // loop/block the `import` statement itself happens to be inside.
    if (interp->is_returning) {
        interp->is_returning = 0;
        value_free(interp->return_value);
        interp->return_value = value_nil();
    }

    return result;
}

// ---------------------------------------------------------------------------
// From-import executor — "from <path> import a, b, c"
//
// Runs the imported module in its own isolated child scope (a sibling of
// the caller's scope, hanging off the same global/native environment) so
// its internals stay private, then copies only the requested names into
// the caller's scope. This gives real selective/namespaced imports instead
// of `import`'s "dump everything into my scope" behaviour.
// ---------------------------------------------------------------------------
static Value execute_from_import(Interpreter *interp, const char *path,
                                  char **names, int name_count, Environment *env) {
    char full_path[1024];
    AstNode *program = resolve_and_parse_import(interp, path, full_path, sizeof(full_path));
    if (!program) return value_nil();

    // Isolated scope: parent is the global/native env (so json_encode,
    // http_get, etc. are still reachable from inside the module), but its
    // own top-level `let`/`fn` bindings don't leak into the caller.
    Environment *module_env = env_new(interp->base_env ? interp->base_env : env);

    char saved_dir[1024];
    strncpy(saved_dir, interp->current_import_dir, sizeof(saved_dir) - 1);
    saved_dir[sizeof(saved_dir) - 1] = '\0';
    set_import_dir_from_file(interp, full_path);

    interpreter_execute(interp, program, module_env);

    strncpy(interp->current_import_dir, saved_dir, sizeof(interp->current_import_dir) - 1);
    interp->current_import_dir[sizeof(interp->current_import_dir) - 1] = '\0';

    if (interp->is_returning) {
        interp->is_returning = 0;
        value_free(interp->return_value);
        interp->return_value = value_nil();
    }

    if (interp->had_runtime_error) return value_nil();

    // Directory the module we just imported from actually lives in (e.g.
    // ~/.khan/packages/webi), so sibling submodule files can still be
    // found below even though current_import_dir has already been
    // restored to whatever it was before this import.
    char module_dir[1024];
    dirname_of(full_path, module_dir, sizeof(module_dir));

    // Pull the requested names out into the caller's scope. Each name is
    // resolved in order against three possibilities:
    //   1. a plain symbol the module itself defines (original Phase 1
    //      behaviour — e.g. `from webi import webi_app`)
    //   2. the package's own name (`from webi import webi`) — flatten
    //      everything the aggregate module just defined, equivalent to a
    //      plain `import "webi"`
    //   3. a sibling submodule file (`from webi import security`) — parse
    //      and run <module_dir>/security.kh in its own scope, then
    //      flatten its public names in too
    for (int i = 0; i < name_count; i++) {
        Value *found = env_get(module_env, names[i]);
        if (found) {
            env_define(env, names[i], value_copy(*found));
            continue;
        }

        if (strcmp(names[i], path) == 0) {
            flatten_public_names(env, module_env);
            continue;
        }

        char sub_probe_path[1024];
        snprintf(sub_probe_path, sizeof(sub_probe_path), "%s/%s.kh", module_dir, names[i]);
        FILE *probe = fopen(sub_probe_path, "rb");
        if (probe) {
            fclose(probe);

            char sub_rel[300];
            snprintf(sub_rel, sizeof(sub_rel), "%s.kh", names[i]);

            // Temporarily point import resolution at the package
            // directory so resolve_and_parse_import finds the sibling
            // file no matter where the caller's own script lives.
            char saved_dir2[1024];
            strncpy(saved_dir2, interp->current_import_dir, sizeof(saved_dir2) - 1);
            saved_dir2[sizeof(saved_dir2) - 1] = '\0';
            strncpy(interp->current_import_dir, module_dir, sizeof(interp->current_import_dir) - 1);
            interp->current_import_dir[sizeof(interp->current_import_dir) - 1] = '\0';

            char sub_full_path[1024];
            AstNode *sub_program = resolve_and_parse_import(interp, sub_rel, sub_full_path, sizeof(sub_full_path));

            if (sub_program) {
                Environment *sub_env = env_new(interp->base_env ? interp->base_env : env);

                char saved_dir3[1024];
                strncpy(saved_dir3, interp->current_import_dir, sizeof(saved_dir3) - 1);
                saved_dir3[sizeof(saved_dir3) - 1] = '\0';
                set_import_dir_from_file(interp, sub_full_path);

                interpreter_execute(interp, sub_program, sub_env);

                strncpy(interp->current_import_dir, saved_dir3, sizeof(interp->current_import_dir) - 1);
                interp->current_import_dir[sizeof(interp->current_import_dir) - 1] = '\0';

                if (interp->is_returning) {
                    interp->is_returning = 0;
                    value_free(interp->return_value);
                    interp->return_value = value_nil();
                }

                if (!interp->had_runtime_error) {
                    flatten_public_names(env, sub_env);
                }
                // sub_env intentionally not freed — functions pulled out
                // of it may still reference it via their closure chain,
                // same reasoning as module_env below.
            }

            strncpy(interp->current_import_dir, saved_dir2, sizeof(interp->current_import_dir) - 1);
            interp->current_import_dir[sizeof(interp->current_import_dir) - 1] = '\0';

            if (interp->had_runtime_error) return value_nil();
            continue;
        }

        fprintf(stderr,
                "[line 0] Runtime error: cannot import name '%s' from '%s' (%s).\n",
                names[i], path, full_path);
        interp->had_runtime_error = 1;
        return value_nil();
    }

    // NOTE: module_env is intentionally not freed — same reasoning as the
    // AST leak above: functions pulled out of it may still reference it as
    // part of their closure chain.
    return value_nil();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void interpreter_init(Interpreter *interp, const char *base_path) {
    interp->had_runtime_error = 0;
    interp->base_path = base_path;
    interp->is_returning = 0;
    interp->is_breaking = 0;
    interp->is_continuing = 0;
    interp->return_value = value_nil();
    interp->base_env = NULL;
    interp->current_import_dir[0] = '\0';
}
// ---------------------------------------------------------------------------
// khan_call_fn — call a named Khan function from C native code
// ---------------------------------------------------------------------------
Value khan_call_fn(Interpreter *interp, Environment *env,
                   const char *fn_name, int argc, Value *argv) {
    // Look up the function in the environment
    Value *fn_val = env_get(env, fn_name);
    if (!fn_val) {
        fprintf(stderr, "[webi] khan_call_fn: function '%s' not found\n", fn_name);
        interp->had_runtime_error = 1;
        return value_nil();
    }

    if (fn_val->type == VAL_NATIVE) {
        // Native C function — call directly
        Value result;
        fn_val->as.native.function(&result, interp, argc, argv);
        return result;
    }

    if (fn_val->type != VAL_FUNCTION) {
        fprintf(stderr, "[webi] khan_call_fn: '%s' is not a function\n", fn_name);
        interp->had_runtime_error = 1;
        return value_nil();
    }

    // Khan function — bind params and execute body
    int param_count = 0;
    for (AstNodeList *p = fn_val->as.function.params; p; p = p->next)
        param_count++;

    if (argc != param_count) {
        fprintf(stderr, "[webi] khan_call_fn: '%s' expects %d args, got %d\n",
                fn_name, param_count, argc);
        interp->had_runtime_error = 1;
        return value_nil();
    }

    Environment *call_env = env_new(fn_val->as.function.closure);
    AstNodeList *p = fn_val->as.function.params;
    for (int i = 0; i < argc; i++, p = p->next)
        env_define(call_env, p->node->data.name, argv[i]);

    Value result = execute_block(interp,
        fn_val->as.function.body->data.statements, call_env);

    if (interp->is_returning) {
        value_free(result);
        result = interp->return_value;
        interp->is_returning = 0;
        interp->return_value = value_nil();
    }

    env_free(call_env);
    return result;
}