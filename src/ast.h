#ifndef KHAN_AST_H
#define KHAN_AST_H

#include "token.h"

// Forward declarations
typedef struct AstNode AstNode;
typedef struct AstNodeList AstNodeList;

// ---------------------------------------------------------------------------
// Node types
// ---------------------------------------------------------------------------
typedef enum {
    // Expressions
    AST_NUMBER,
    AST_STRING,
    AST_BOOL,
    AST_NIL,
    AST_IDENTIFIER,
    AST_BINARY,
    AST_UNARY,
    AST_GROUPING,
    AST_ASSIGNMENT,
    AST_CALL,
    AST_ARRAY,
    AST_INDEX,
    AST_INDEX_ASSIGN,
    AST_MAP,
    AST_MAP_ENTRY,

    // Statements
    AST_EXPR_STMT,
    AST_PRINT_STMT,
    AST_LET_STMT,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_FOR_STMT,
    AST_BLOCK,
    AST_FN_DECL,
    AST_RETURN_STMT,
    AST_BREAK_STMT,
    AST_CONTINUE_STMT,
    AST_TRY_STMT,
    AST_THROW_STMT,
    AST_CLASS_STMT,
    AST_GET_ATTR,
    AST_SET_ATTR,
    AST_METHOD_CALL,
    AST_SUPER_CALL,

    // Top-level
    AST_PROGRAM,
    AST_IMPORT_STMT,
    AST_FROM_IMPORT_STMT,
} AstNodeType;

// ---------------------------------------------------------------------------
// Binary / Unary operators
// ---------------------------------------------------------------------------
typedef enum {
    OP_PLUS, OP_MINUS, OP_STAR, OP_SLASH, OP_PERCENT,
    OP_EQUAL_EQUAL, OP_BANG_EQUAL,
    OP_LESS, OP_LESS_EQUAL, OP_GREATER, OP_GREATER_EQUAL,
    OP_AND, OP_OR,
    OP_NOT, OP_NEGATE,
} AstOp;

// ---------------------------------------------------------------------------
// Linked list of nodes (used for statement lists, argument lists, etc.)
// ---------------------------------------------------------------------------
struct AstNodeList {
    AstNode *node;
    AstNodeList *next;
    // Only meaningful on the HEAD element (the one every caller actually
    // holds a reference to, via `list = ast_list_append(list, node)`) —
    // caches the current tail so repeated appends don't have to walk the
    // whole list from the head every time. Without this, building an
    // N-element list (a function's statement list, a large array/map
    // literal, etc.) was O(n^2): each append walked everything appended
    // so far. See docs/ast-audit.md.
    AstNodeList *tail_cache;
};

// ---------------------------------------------------------------------------
// The node itself — tagged union via an anonymous union (C11)
// ---------------------------------------------------------------------------
struct AstNode {
    AstNodeType type;
    int line;

    union {
        // AST_NUMBER
        double number_value;

        // AST_BOOL
        int bool_value;

        // AST_STRING
        const char *string_value;

        // AST_IDENTIFIER / AST_LET_STMT / AST_FN_DECL name / AST_CALL callee
        const char *name;

        // AST_BINARY
        struct {
            AstNode *left;
            AstOp op;
            AstNode *right;
        } binary;

        // AST_UNARY
        struct {
            AstOp op;
            AstNode *right;
        } unary;

        // AST_GROUPING
        AstNode *grouping;

        // AST_ASSIGNMENT
        struct {
            const char *var_name;
            AstNode *value;
        } assignment;

        // AST_CALL
        struct {
            const char *callee;       // set when callee is a bare identifier (fast path)
            AstNode    *callee_expr;  // set when callee is a general expression (e.g. arr[i], obj["fn"])
                                       // exactly one of callee / callee_expr is non-NULL
            AstNodeList *arguments;
        } call;

        // AST_ARRAY — list of element expression nodes
        AstNodeList *array_elements;

        // AST_INDEX
        struct {
            AstNode *object;
            AstNode *index;
        } index_expr;

        // AST_INDEX_ASSIGN — object[index] = value
        struct {
            AstNode *object;
            AstNode *index;
            AstNode *value;
        } index_assign;

        // AST_MAP — list of AST_MAP_ENTRY nodes
        AstNodeList *map_entries;

        // AST_MAP_ENTRY — one "key: value" pair inside a map literal
        struct {
            AstNode *key;
            AstNode *value;
        } map_entry;

        // AST_PRINT_STMT / AST_RETURN_STMT / AST_EXPR_STMT / AST_THROW_STMT
        AstNode *expr;

        // AST_LET_STMT
        struct {
            const char *let_name;
            AstNode *let_initializer;
        } let_decl;

        // AST_IF_STMT
        struct {
            AstNode *condition;
            AstNode *then_branch;
            AstNode *else_branch;   // may be NULL
        } if_stmt;

        // AST_WHILE_STMT
        struct {
            AstNode *while_condition;
            AstNode *while_body;
        } while_stmt;

        // AST_FOR_STMT — for <var> in <iterable>: <body>
        struct {
            const char *for_var;
            AstNode *for_iterable;
            AstNode *for_body;
        } for_stmt;

        // AST_TRY_STMT — try: <try_block> catch [name]: <catch_block>
        struct {
            AstNode *try_block;
            const char *catch_var;   // may be NULL for bare "catch:"
            AstNode *catch_block;
        } try_stmt;
        // AST_THROW_STMT reuses `expr` below (same pattern as print/return)

        // AST_CLASS_STMT — class Name: <methods>  or  class Name(Base): <methods>
        struct {
            const char *class_name;
            const char *superclass_name; // NULL if no `(Base)` clause
            AstNodeList *methods;   // list of AST_FN_DECL nodes (self is an
                                     // explicit first parameter, same as any
                                     // other parameter — see docs/classes.md)
        } class_stmt;

        // AST_GET_ATTR — object.name (field read; NOT used for method calls,
        // which compile through AST_METHOD_CALL below instead)
        struct {
            AstNode *object;
            const char *attr_name;
        } get_attr;

        // AST_SET_ATTR — object.name = value
        struct {
            AstNode *object;
            const char *attr_name;
            AstNode *value;
        } set_attr;

        // AST_METHOD_CALL — object.name(args). Deliberately its own node
        // rather than going through AST_GET_ATTR + AST_CALL_EXPR — see
        // docs/classes.md for why methods aren't first-class values in v1
        // (no `let m = obj.method` support) and this fused shape is what
        // makes `obj.method(args)` compile straightforwardly instead.
        struct {
            AstNode *object;
            const char *method_name;
            AstNodeList *arguments;
        } method_call;
        // AST_SUPER_CALL — super.method(args), only valid inside a method
        // of a class declared with a `(Base)` superclass. Resolves
        // statically against the enclosing class's declared superclass,
        // not the receiver's dynamic class — see docs/classes.md.
        struct {
            const char *method_name;
            AstNodeList *arguments;
        } super_call;

        // AST_BLOCK
        AstNodeList *statements;

        // AST_FN_DECL
        struct {
            const char *fn_name;
            AstNodeList *params;       // list of AST_IDENTIFIER nodes
            AstNode *fn_body;          // AST_BLOCK
        } fn_decl;

    // AST_PROGRAM
    AstNodeList *program_stmts;

        // AST_IMPORT_STMT
        const char *import_path;

        // AST_FROM_IMPORT_STMT — from <path> import <name1>, <name2>, ...
        struct {
            const char *path;
            char **names;     // array of strdup'd names, size name_count
            int name_count;
        } from_import;
    } data;
};

// ---------------------------------------------------------------------------
// Construction helpers (allocated with malloc)
// ---------------------------------------------------------------------------
AstNode *ast_new_node(AstNodeType type, int line);

AstNode *ast_new_number(double value, int line);
AstNode *ast_new_bool(int value, int line);
AstNode *ast_new_nil(int line);
AstNode *ast_new_string(const char *value, int line);
AstNode *ast_new_identifier(const char *name, int line);
AstNode *ast_new_binary(AstNode *left, AstOp op, AstNode *right, int line);
AstNode *ast_new_unary(AstOp op, AstNode *right, int line);
AstNode *ast_new_grouping(AstNode *expr, int line);
AstNode *ast_new_assignment(const char *name, AstNode *value, int line);
AstNode *ast_new_call(const char *callee, AstNodeList *args, int line);
AstNode *ast_new_call_expr(AstNode *callee_expr, AstNodeList *args, int line);
AstNode *ast_new_array(AstNodeList *elements, int line);
AstNode *ast_new_index(AstNode *object, AstNode *index, int line);
AstNode *ast_new_index_assign(AstNode *object, AstNode *index, AstNode *value, int line);
AstNode *ast_new_map(AstNodeList *entries, int line);
AstNode *ast_new_map_entry(AstNode *key, AstNode *value, int line);
AstNode *ast_new_expr_stmt(AstNode *expr, int line);
AstNode *ast_new_print_stmt(AstNode *expr, int line);
AstNode *ast_new_let_stmt(const char *name, AstNode *initializer, int line);
AstNode *ast_new_if_stmt(AstNode *cond, AstNode *then_branch, AstNode *else_branch, int line);
AstNode *ast_new_while_stmt(AstNode *cond, AstNode *body, int line);
AstNode *ast_new_for_stmt(const char *var_name, AstNode *iterable, AstNode *body, int line);
AstNode *ast_new_block(AstNodeList *stmts, int line);
AstNode *ast_new_fn_decl(const char *name, AstNodeList *params, AstNode *body, int line);
AstNode *ast_new_return_stmt(AstNode *value, int line);
AstNode *ast_new_break_stmt(int line);
AstNode *ast_new_continue_stmt(int line);
AstNode *ast_new_try_stmt(AstNode *try_block, const char *catch_var, AstNode *catch_block, int line);
AstNode *ast_new_throw_stmt(AstNode *expr, int line);
AstNode *ast_new_class_stmt(const char *class_name, const char *superclass_name, AstNodeList *methods, int line);
AstNode *ast_new_get_attr(AstNode *object, const char *attr_name, int line);
AstNode *ast_new_set_attr(AstNode *object, const char *attr_name, AstNode *value, int line);
AstNode *ast_new_method_call(AstNode *object, const char *method_name, AstNodeList *arguments, int line);
AstNode *ast_new_super_call(const char *method_name, AstNodeList *arguments, int line);
AstNode *ast_new_import_stmt(const char *path, int line);
AstNode *ast_new_from_import_stmt(const char *path, char **names, int name_count, int line);
AstNode *ast_new_program(AstNodeList *stmts, int line);

AstNodeList *ast_list_append(AstNodeList *list, AstNode *node);
void ast_free(AstNode *node);

// Debug: print an AST tree (for development)
void ast_print(AstNode *node, int indent);

#endif