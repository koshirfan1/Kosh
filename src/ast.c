#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

AstNode *ast_new_node(AstNodeType type, int line) {
    AstNode *node = malloc(sizeof(AstNode));
    node->type = type;
    node->line = line;
    // Zero-out the union so uninitialised fields are NULL/0
    memset(&node->data, 0, sizeof(node->data));
    return node;
}

AstNode *ast_new_number(double value, int line) {
    AstNode *node = ast_new_node(AST_NUMBER, line);
    node->data.number_value = value;
    return node;
}

AstNode *ast_new_bool(int value, int line) {
    AstNode *node = ast_new_node(AST_BOOL, line);
    node->data.bool_value = value;
    return node;
}

AstNode *ast_new_nil(int line) {
    return ast_new_node(AST_NIL, line);
}

AstNode *ast_new_string(const char *value, int line) {
    AstNode *node = ast_new_node(AST_STRING, line);
    node->data.string_value = strdup(value);
    return node;
}

AstNode *ast_new_identifier(const char *name, int line) {
    AstNode *node = ast_new_node(AST_IDENTIFIER, line);
    node->data.name = strdup(name);
    return node;
}

AstNode *ast_new_binary(AstNode *left, AstOp op, AstNode *right, int line) {
    AstNode *node = ast_new_node(AST_BINARY, line);
    node->data.binary.left = left;
    node->data.binary.op = op;
    node->data.binary.right = right;
    return node;
}

AstNode *ast_new_unary(AstOp op, AstNode *right, int line) {
    AstNode *node = ast_new_node(AST_UNARY, line);
    node->data.unary.op = op;
    node->data.unary.right = right;
    return node;
}

AstNode *ast_new_grouping(AstNode *expr, int line) {
    AstNode *node = ast_new_node(AST_GROUPING, line);
    node->data.grouping = expr;
    return node;
}

AstNode *ast_new_assignment(const char *name, AstNode *value, int line) {
    AstNode *node = ast_new_node(AST_ASSIGNMENT, line);
    node->data.assignment.var_name = strdup(name);
    node->data.assignment.value = value;
    return node;
}

AstNode *ast_new_call(const char *callee, AstNodeList *args, int line) {
    AstNode *node = ast_new_node(AST_CALL, line);
    node->data.call.callee = strdup(callee);
    node->data.call.callee_expr = NULL;
    node->data.call.arguments = args;
    return node;
}

AstNode *ast_new_call_expr(AstNode *callee_expr, AstNodeList *args, int line) {
    AstNode *node = ast_new_node(AST_CALL, line);
    node->data.call.callee = NULL;
    node->data.call.callee_expr = callee_expr;
    node->data.call.arguments = args;
    return node;
}


AstNode *ast_new_array(AstNodeList *elements, int line) {
    AstNode *node = ast_new_node(AST_ARRAY, line);
    node->data.array_elements = elements;
    return node;
}

AstNode *ast_new_index(AstNode *object, AstNode *index, int line) {
    AstNode *node = ast_new_node(AST_INDEX, line);
    node->data.index_expr.object = object;
    node->data.index_expr.index = index;
    return node;
}

AstNode *ast_new_index_assign(AstNode *object, AstNode *index, AstNode *value, int line) {
    AstNode *node = ast_new_node(AST_INDEX_ASSIGN, line);
    node->data.index_assign.object = object;
    node->data.index_assign.index = index;
    node->data.index_assign.value = value;
    return node;
}

AstNode *ast_new_map(AstNodeList *entries, int line) {
    AstNode *node = ast_new_node(AST_MAP, line);
    node->data.map_entries = entries;
    return node;
}

AstNode *ast_new_map_entry(AstNode *key, AstNode *value, int line) {
    AstNode *node = ast_new_node(AST_MAP_ENTRY, line);
    node->data.map_entry.key = key;
    node->data.map_entry.value = value;
    return node;
}

AstNode *ast_new_expr_stmt(AstNode *expr, int line) {
    AstNode *node = ast_new_node(AST_EXPR_STMT, line);
    node->data.expr = expr;
    return node;
}

AstNode *ast_new_print_stmt(AstNode *expr, int line) {
    AstNode *node = ast_new_node(AST_PRINT_STMT, line);
    node->data.expr = expr;
    return node;
}

AstNode *ast_new_let_stmt(const char *name, AstNode *initializer, int line) {
    AstNode *node = ast_new_node(AST_LET_STMT, line);
    node->data.let_decl.let_name = strdup(name);
    node->data.let_decl.let_initializer = initializer;
    return node;
}

AstNode *ast_new_if_stmt(AstNode *cond, AstNode *then_branch, AstNode *else_branch, int line) {
    AstNode *node = ast_new_node(AST_IF_STMT, line);
    node->data.if_stmt.condition = cond;
    node->data.if_stmt.then_branch = then_branch;
    node->data.if_stmt.else_branch = else_branch;
    return node;
}

AstNode *ast_new_while_stmt(AstNode *cond, AstNode *body, int line) {
    AstNode *node = ast_new_node(AST_WHILE_STMT, line);
    node->data.while_stmt.while_condition = cond;
    node->data.while_stmt.while_body = body;
    return node;
}

AstNode *ast_new_for_stmt(const char *var_name, AstNode *iterable, AstNode *body, int line) {
    AstNode *node = ast_new_node(AST_FOR_STMT, line);
    node->data.for_stmt.for_var = strdup(var_name);
    node->data.for_stmt.for_iterable = iterable;
    node->data.for_stmt.for_body = body;
    return node;
}

AstNode *ast_new_block(AstNodeList *stmts, int line) {
    AstNode *node = ast_new_node(AST_BLOCK, line);
    node->data.statements = stmts;
    return node;
}

AstNode *ast_new_fn_decl(const char *name, AstNodeList *params, AstNode *body, int line) {
    AstNode *node = ast_new_node(AST_FN_DECL, line);
    node->data.fn_decl.fn_name = strdup(name);
    node->data.fn_decl.params = params;
    node->data.fn_decl.fn_body = body;
    return node;
}

AstNode *ast_new_return_stmt(AstNode *value, int line) {
    AstNode *node = ast_new_node(AST_RETURN_STMT, line);
    node->data.expr = value;
    return node;
}

AstNode *ast_new_break_stmt(int line) {
    return ast_new_node(AST_BREAK_STMT, line);
}

AstNode *ast_new_continue_stmt(int line) {
    return ast_new_node(AST_CONTINUE_STMT, line);
}

AstNode *ast_new_try_stmt(AstNode *try_block, const char *catch_var, AstNode *catch_block, int line) {
    AstNode *node = ast_new_node(AST_TRY_STMT, line);
    node->data.try_stmt.try_block = try_block;
    node->data.try_stmt.catch_var = catch_var ? strdup(catch_var) : NULL;
    node->data.try_stmt.catch_block = catch_block;
    return node;
}

AstNode *ast_new_throw_stmt(AstNode *expr, int line) {
    AstNode *node = ast_new_node(AST_THROW_STMT, line);
    node->data.expr = expr;
    return node;
}

AstNode *ast_new_class_stmt(const char *class_name, const char *superclass_name,
                             AstNodeList *methods, int line) {
    AstNode *node = ast_new_node(AST_CLASS_STMT, line);
    node->data.class_stmt.class_name = strdup(class_name);
    node->data.class_stmt.superclass_name = superclass_name ? strdup(superclass_name) : NULL;
    node->data.class_stmt.methods = methods;
    return node;
}

AstNode *ast_new_get_attr(AstNode *object, const char *attr_name, int line) {
    AstNode *node = ast_new_node(AST_GET_ATTR, line);
    node->data.get_attr.object = object;
    node->data.get_attr.attr_name = strdup(attr_name);
    return node;
}

AstNode *ast_new_set_attr(AstNode *object, const char *attr_name, AstNode *value, int line) {
    AstNode *node = ast_new_node(AST_SET_ATTR, line);
    node->data.set_attr.object = object;
    node->data.set_attr.attr_name = strdup(attr_name);
    node->data.set_attr.value = value;
    return node;
}

AstNode *ast_new_method_call(AstNode *object, const char *method_name, AstNodeList *arguments, int line) {
    AstNode *node = ast_new_node(AST_METHOD_CALL, line);
    node->data.method_call.object = object;
    node->data.method_call.method_name = strdup(method_name);
    node->data.method_call.arguments = arguments;
    return node;
}

AstNode *ast_new_super_call(const char *method_name, AstNodeList *arguments, int line) {
    AstNode *node = ast_new_node(AST_SUPER_CALL, line);
    node->data.super_call.method_name = strdup(method_name);
    node->data.super_call.arguments = arguments;
    return node;
}

AstNode *ast_new_import_stmt(const char *path, int line) {
    AstNode *node = ast_new_node(AST_IMPORT_STMT, line);
    node->data.import_path = strdup(path);
    return node;
}

AstNode *ast_new_from_import_stmt(const char *path, char **names, int name_count, int line) {
    AstNode *node = ast_new_node(AST_FROM_IMPORT_STMT, line);
    node->data.from_import.path = strdup(path);
    node->data.from_import.name_count = name_count;
    if (name_count > 0) {
        node->data.from_import.names = malloc(sizeof(char *) * name_count);
        for (int i = 0; i < name_count; i++) {
            node->data.from_import.names[i] = strdup(names[i]);
        }
    } else {
        node->data.from_import.names = NULL;
    }
    return node;
}

AstNode *ast_new_program(AstNodeList *stmts, int line) {
    AstNode *node = ast_new_node(AST_PROGRAM, line);
    node->data.program_stmts = stmts;
    return node;
}

// ---------------------------------------------------------------------------
// List helpers
// ---------------------------------------------------------------------------
AstNodeList *ast_list_append(AstNodeList *list, AstNode *node) {
    AstNodeList *item = malloc(sizeof(AstNodeList));
    item->node = node;
    item->next = NULL;
    item->tail_cache = NULL;

    if (!list) {
        item->tail_cache = item; // new list: item is both head and tail
        return item;
    }

    // list->tail_cache is only maintained on the head; fall back to a
    // full walk if it's ever missing (defensive — shouldn't happen if
    // every append goes through this function, but a list built any
    // other way shouldn't silently misbehave).
    AstNodeList *tail = list->tail_cache;
    if (!tail) {
        tail = list;
        while (tail->next) tail = tail->next;
    }

    tail->next = item;
    list->tail_cache = item;
    return list;
}

// ---------------------------------------------------------------------------
// Free
// ---------------------------------------------------------------------------
static void ast_free_list(AstNodeList *list) {
    while (list) {
        AstNodeList *next = list->next;
        ast_free(list->node);
        free(list);
        list = next;
    }
}

void ast_free(AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case AST_NUMBER:
            break;
        case AST_BOOL:
            break;
        case AST_NIL:
            break;
        case AST_STRING:
            free((void *)node->data.string_value);
            break;
        case AST_IDENTIFIER:
            free((void *)node->data.name);
            break;
        case AST_BINARY:
            ast_free(node->data.binary.left);
            ast_free(node->data.binary.right);
            break;
        case AST_UNARY:
            ast_free(node->data.unary.right);
            break;
        case AST_GROUPING:
            ast_free(node->data.grouping);
            break;
        case AST_ASSIGNMENT:
            free((void *)node->data.assignment.var_name);
            ast_free(node->data.assignment.value);
            break;
        case AST_CALL:
            if (node->data.call.callee)
                free((void *)node->data.call.callee);
            if (node->data.call.callee_expr)
                ast_free(node->data.call.callee_expr);
            ast_free_list(node->data.call.arguments);
            break;
        case AST_ARRAY:
            ast_free_list(node->data.array_elements);
            break;
        case AST_INDEX:
            ast_free(node->data.index_expr.object);
            ast_free(node->data.index_expr.index);
            break;
        case AST_INDEX_ASSIGN:
            ast_free(node->data.index_assign.object);
            ast_free(node->data.index_assign.index);
            ast_free(node->data.index_assign.value);
            break;
        case AST_MAP:
            ast_free_list(node->data.map_entries);
            break;
        case AST_MAP_ENTRY:
            ast_free(node->data.map_entry.key);
            ast_free(node->data.map_entry.value);
            break;
        case AST_EXPR_STMT:
        case AST_PRINT_STMT:
        case AST_RETURN_STMT:
            ast_free(node->data.expr);
            break;
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            break;
        case AST_TRY_STMT:
            ast_free(node->data.try_stmt.try_block);
            free((void *)node->data.try_stmt.catch_var);
            ast_free(node->data.try_stmt.catch_block);
            break;
        case AST_THROW_STMT:
            ast_free(node->data.expr);
            break;
        case AST_CLASS_STMT:
            free((void *)node->data.class_stmt.class_name);
            free((void *)node->data.class_stmt.superclass_name);
            ast_free_list(node->data.class_stmt.methods);
            break;
        case AST_GET_ATTR:
            ast_free(node->data.get_attr.object);
            free((void *)node->data.get_attr.attr_name);
            break;
        case AST_SET_ATTR:
            ast_free(node->data.set_attr.object);
            free((void *)node->data.set_attr.attr_name);
            ast_free(node->data.set_attr.value);
            break;
        case AST_METHOD_CALL:
            ast_free(node->data.method_call.object);
            free((void *)node->data.method_call.method_name);
            ast_free_list(node->data.method_call.arguments);
            break;
        case AST_SUPER_CALL:
            free((void *)node->data.super_call.method_name);
            ast_free_list(node->data.super_call.arguments);
            break;
        case AST_LET_STMT:
            free((void *)node->data.let_decl.let_name);
            ast_free(node->data.let_decl.let_initializer);
            break;
        case AST_IF_STMT:
            ast_free(node->data.if_stmt.condition);
            ast_free(node->data.if_stmt.then_branch);
            ast_free(node->data.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            ast_free(node->data.while_stmt.while_condition);
            ast_free(node->data.while_stmt.while_body);
            break;
        case AST_FOR_STMT:
            free((void *)node->data.for_stmt.for_var);
            ast_free(node->data.for_stmt.for_iterable);
            ast_free(node->data.for_stmt.for_body);
            break;
        case AST_BLOCK:
            ast_free_list(node->data.statements);
            break;
        case AST_FN_DECL:
            free((void *)node->data.fn_decl.fn_name);
            ast_free_list(node->data.fn_decl.params);
            ast_free(node->data.fn_decl.fn_body);
            break;
        case AST_PROGRAM:
            ast_free_list(node->data.program_stmts);
            break;
        case AST_IMPORT_STMT:
            free((void *)node->data.import_path);
            break;
        case AST_FROM_IMPORT_STMT:
            free((void *)node->data.from_import.path);
            for (int i = 0; i < node->data.from_import.name_count; i++) {
                free(node->data.from_import.names[i]);
            }
            free(node->data.from_import.names);
            break;
    }
    free(node);
}

// ---------------------------------------------------------------------------
// Debug print
// ---------------------------------------------------------------------------
static const char *op_name(AstOp op) {
    switch (op) {
        case OP_PLUS:           return "+";
        case OP_MINUS:          return "-";
        case OP_STAR:           return "*";
        case OP_SLASH:          return "/";
        case OP_PERCENT:        return "%";
        case OP_EQUAL_EQUAL:    return "==";
        case OP_BANG_EQUAL:     return "!=";
        case OP_LESS:           return "<";
        case OP_LESS_EQUAL:     return "<=";
        case OP_GREATER:        return ">";
        case OP_GREATER_EQUAL:  return ">=";
        case OP_AND:            return "and";
        case OP_OR:             return "or";
        case OP_NOT:            return "not";
        case OP_NEGATE:         return "-";
    }
    return "???";
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

static void ast_print_list(AstNodeList *list, int indent);

void ast_print(AstNode *node, int indent) {
    if (!node) { print_indent(indent); printf("(null)\n"); return; }
    print_indent(indent);
    switch (node->type) {
        case AST_NUMBER:
            printf("(number %g)\n", node->data.number_value);
            break;
        case AST_BOOL:
            printf("(bool %s)\n", node->data.bool_value ? "true" : "false");
            break;
        case AST_NIL:
            printf("(nil)\n");
            break;
        case AST_STRING:
            printf("(string \"%s\")\n", node->data.string_value);
            break;
        case AST_IDENTIFIER:
            printf("(ident %s)\n", node->data.name);
            break;
        case AST_BINARY:
            printf("(binary %s\n", op_name(node->data.binary.op));
            ast_print(node->data.binary.left, indent + 1);
            ast_print(node->data.binary.right, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_UNARY:
            printf("(unary %s\n", op_name(node->data.unary.op));
            ast_print(node->data.unary.right, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_GROUPING:
            printf("(group\n");
            ast_print(node->data.grouping, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_ASSIGNMENT:
            printf("(assign %s\n", node->data.assignment.var_name);
            ast_print(node->data.assignment.value, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_CALL:
            if (node->data.call.callee) {
                printf("(call %s\n", node->data.call.callee);
            } else {
                printf("(call <expr>\n");
                ast_print(node->data.call.callee_expr, indent + 1);
            }
            ast_print_list(node->data.call.arguments, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_ARRAY:
            printf("(array\n");
            ast_print_list(node->data.array_elements, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_INDEX:
            printf("(index\n");
            ast_print(node->data.index_expr.object, indent + 1);
            ast_print(node->data.index_expr.index, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_INDEX_ASSIGN:
            printf("(index-assign\n");
            ast_print(node->data.index_assign.object, indent + 1);
            ast_print(node->data.index_assign.index, indent + 1);
            ast_print(node->data.index_assign.value, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_MAP:
            printf("(map\n");
            ast_print_list(node->data.map_entries, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_MAP_ENTRY:
            printf("(entry\n");
            ast_print(node->data.map_entry.key, indent + 1);
            ast_print(node->data.map_entry.value, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_EXPR_STMT:
            printf("(expr-stmt\n");
            ast_print(node->data.expr, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_PRINT_STMT:
            printf("(print\n");
            ast_print(node->data.expr, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_LET_STMT:
            printf("(let %s\n", node->data.let_decl.let_name);
            ast_print(node->data.let_decl.let_initializer, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_IF_STMT:
            printf("(if\n");
            ast_print(node->data.if_stmt.condition, indent + 1);
            ast_print(node->data.if_stmt.then_branch, indent + 1);
            if (node->data.if_stmt.else_branch) {
                print_indent(indent + 1); printf("(else\n");
                ast_print(node->data.if_stmt.else_branch, indent + 2);
                print_indent(indent + 1); printf(")\n");
            }
            print_indent(indent); printf(")\n");
            break;
        case AST_WHILE_STMT:
            printf("(while\n");
            ast_print(node->data.while_stmt.while_condition, indent + 1);
            ast_print(node->data.while_stmt.while_body, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_FOR_STMT:
            printf("(for %s\n", node->data.for_stmt.for_var);
            ast_print(node->data.for_stmt.for_iterable, indent + 1);
            ast_print(node->data.for_stmt.for_body, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_TRY_STMT:
            printf("(try\n");
            ast_print(node->data.try_stmt.try_block, indent + 1);
            print_indent(indent + 1);
            printf("(catch %s\n", node->data.try_stmt.catch_var ? node->data.try_stmt.catch_var : "");
            ast_print(node->data.try_stmt.catch_block, indent + 2);
            print_indent(indent + 1); printf(")\n");
            print_indent(indent); printf(")\n");
            break;
        case AST_THROW_STMT:
            printf("(throw\n");
            ast_print(node->data.expr, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_CLASS_STMT:
            if (node->data.class_stmt.superclass_name)
                printf("(class %s(%s)\n", node->data.class_stmt.class_name, node->data.class_stmt.superclass_name);
            else
                printf("(class %s\n", node->data.class_stmt.class_name);
            ast_print_list(node->data.class_stmt.methods, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_GET_ATTR:
            printf("(get-attr %s\n", node->data.get_attr.attr_name);
            ast_print(node->data.get_attr.object, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_SET_ATTR:
            printf("(set-attr %s\n", node->data.set_attr.attr_name);
            ast_print(node->data.set_attr.object, indent + 1);
            ast_print(node->data.set_attr.value, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_METHOD_CALL:
            printf("(method-call %s\n", node->data.method_call.method_name);
            ast_print(node->data.method_call.object, indent + 1);
            ast_print_list(node->data.method_call.arguments, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_SUPER_CALL:
            printf("(super-call %s\n", node->data.super_call.method_name);
            ast_print_list(node->data.super_call.arguments, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_BLOCK:
            printf("(block\n");
            ast_print_list(node->data.statements, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_FN_DECL:
            printf("(fn %s\n", node->data.fn_decl.fn_name);
            if (node->data.fn_decl.params) {
                print_indent(indent + 1); printf("(params\n");
                ast_print_list(node->data.fn_decl.params, indent + 2);
                print_indent(indent + 1); printf(")\n");
            }
            ast_print(node->data.fn_decl.fn_body, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_RETURN_STMT:
            printf("(return\n");
            ast_print(node->data.expr, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_BREAK_STMT:
            print_indent(indent); printf("(break)\n");
            break;
        case AST_CONTINUE_STMT:
            print_indent(indent); printf("(continue)\n");
            break;
        case AST_PROGRAM:
            printf("(program\n");
            ast_print_list(node->data.program_stmts, indent + 1);
            print_indent(indent); printf(")\n");
            break;
        case AST_IMPORT_STMT:
            printf("(import \"%s\")\n", node->data.import_path);
            break;
        case AST_FROM_IMPORT_STMT:
            printf("(from \"%s\" import", node->data.from_import.path);
            for (int i = 0; i < node->data.from_import.name_count; i++) {
                printf(" %s%s", node->data.from_import.names[i],
                       (i < node->data.from_import.name_count - 1) ? "," : "");
            }
            printf(")\n");
            break;
    }
}

static void ast_print_list(AstNodeList *list, int indent) {
    for (AstNodeList *cur = list; cur; cur = cur->next) {
        ast_print(cur->node, indent);
    }
}