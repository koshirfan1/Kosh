#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

// strndup is a POSIX/GNU extension, not standard C, and its
// availability is NOT reliably signaled by _POSIX_C_SOURCE on every
// platform — this used to gate on `_POSIX_C_SOURCE < 200809L` and
// skip providing a fallback whenever that macro was defined at
// 200809L or higher, which the makefile does UNCONDITIONALLY on every
// platform (Linux, macOS, AND Windows) as one of its standard CFLAGS.
// That's fine on glibc, where _POSIX_C_SOURCE genuinely gates
// strndup's declaration — but MinGW-w64's <string.h> does not follow
// the same convention, so real MinGW builds hit "makes pointer from
// integer without a cast" (strndup silently falling back to an
// implicit, wrong-return-type declaration) with this exact flag
// combination. Found via an actual Windows CI failure, not by
// inspection — the fix here is to stop trying to detect whether the
// platform provides strndup at all, since that detection was already
// wrong once and gating logic like it is easy to get subtly wrong
// again for the next platform/toolchain combination. Providing our
// own implementation unconditionally, under a name that can't
// collide with any system declaration, sidesteps the whole class of
// "does this libc expose this GNU extension under these flags"
// uncertainty for good.
static char *khan_strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

// Processes backslash escape sequences in a string literal's raw content
// (the text between the quotes, before any unescaping). Supported escapes:
// \n \t \r \\ \" \0 \xHH — anything else is passed through literally.
static char *unescape_string(const char *raw, int len) {
    char *out = malloc(len + 1); // unescaped result is never longer than input
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (raw[i] == '\\' && i + 1 < len) {
            char next = raw[i + 1];
            switch (next) {
                case 'n':  out[j++] = '\n'; i++; break;
                case 't':  out[j++] = '\t'; i++; break;
                case 'r':  out[j++] = '\r'; i++; break;
                case '\\': out[j++] = '\\'; i++; break;
                case '"':  out[j++] = '"';  i++; break;
                case '0':  out[j++] = '\0'; i++; break;
                case 'x': {
                    // \xHH — two hex digits
                    if (i + 3 < len) {
                        char hex[3] = { raw[i+2], raw[i+3], '\0' };
                        // validate both are hex digits
                        int valid = 1;
                        for (int k = 0; k < 2; k++) {
                            char c = hex[k];
                            if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))
                                valid = 0;
                        }
                        if (valid) {
                            out[j++] = (char)strtol(hex, NULL, 16);
                            i += 3;
                            break;
                        }
                    }
                    // fallthrough if invalid
                    out[j++] = raw[i];
                    break;
                }
                default:
                    // Unknown escape — keep the backslash literally
                    out[j++] = raw[i];
                    break;
            }
        } else {
            out[j++] = raw[i];
        }
    }
    out[j] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------
static void report_syntax_error(Parser *parser, int line, int column,
                                 const char *token_text, int token_len,
                                 const char *message) {
    fprintf(stderr, "SyntaxError: %s\n", message);
    fprintf(stderr, "  File:   %s\n", parser->filename ? parser->filename : "<script>");
    fprintf(stderr, "  Line:   %d\n", line);
    fprintf(stderr, "  Column: %d\n", column);
    if (token_len > 0) {
        // Escape control characters so e.g. a bare newline token doesn't
        // print as a literal line break that looks like a formatting bug.
        char buf[64];
        int bi = 0;
        for (int i = 0; i < token_len && bi < (int)sizeof(buf) - 2; i++) {
            char c = token_text[i];
            if (c == '\n')      { buf[bi++] = '\\'; buf[bi++] = 'n'; }
            else if (c == '\t') { buf[bi++] = '\\'; buf[bi++] = 't'; }
            else if (c == '\r') { buf[bi++] = '\\'; buf[bi++] = 'r'; }
            else                { buf[bi++] = c; }
        }
        buf[bi] = '\0';
        fprintf(stderr, "  Near:   '%s'\n", buf);
    }
    parser->had_error = 1;
}

static void error_at_current(Parser *parser, const char *message) {
    const char *text = parser->current.length > 0 ? parser->current.start : "end";
    int len = parser->current.length > 0 ? parser->current.length : 3;
    report_syntax_error(parser, parser->current.line, parser->current.column, text, len, message);
}

static void error(Parser *parser, const char *message) {
    report_syntax_error(parser, parser->previous.line, parser->previous.column,
                         parser->previous.start, parser->previous.length, message);
}

// ---------------------------------------------------------------------------
// Token consumption helpers
// ---------------------------------------------------------------------------
static void advance(Parser *parser) {
    parser->previous = parser->current;
    for (;;) {
        parser->current = lexer_next_token(parser->lexer);
        if (parser->current.type != TOKEN_ERROR) break;
        // Report lexical errors but keep going. A TOKEN_ERROR's
        // start/length hold the lexer's own descriptive message text
        // (e.g. "Unexpected character.", set in error_token()) — not
        // the offending source snippet — so that's the message itself,
        // not "near" text.
        {
            char msgbuf[128];
            int n = parser->current.length < (int)sizeof(msgbuf) - 1
                        ? parser->current.length : (int)sizeof(msgbuf) - 1;
            memcpy(msgbuf, parser->current.start, n);
            msgbuf[n] = '\0';
            report_syntax_error(parser, parser->current.line, parser->current.column, NULL, 0, msgbuf);
        }
        parser->had_error = 1;
    }
}

static int check(Parser *parser, TokenKind type) {
    return parser->current.type == type;
}

static int match(Parser *parser, TokenKind type) {
    if (!check(parser, type)) return 0;
    advance(parser);
    return 1;
}

static void consume(Parser *parser, TokenKind type, const char *message) {
    if (check(parser, type)) {
        advance(parser);
        return;
    }
    error_at_current(parser, message);
}

// ---------------------------------------------------------------------------
// Forward declarations for recursive descent
// ---------------------------------------------------------------------------
static AstNode *statement(Parser *parser);
static AstNode *declaration(Parser *parser);
static AstNode *expression(Parser *parser);
static AstNode *assignment(Parser *parser);
static AstNode *logic_or(Parser *parser);
static AstNode *logic_and(Parser *parser);
static AstNode *equality(Parser *parser);
static AstNode *comparison(Parser *parser);
static AstNode *term(Parser *parser);
static AstNode *factor(Parser *parser);
static AstNode *unary(Parser *parser);
static AstNode *call(Parser *parser);
static AstNode *primary(Parser *parser);

// ---------------------------------------------------------------------------
// Parse a block: expects INDENT, statements, DEDENT
// ---------------------------------------------------------------------------
static AstNode *block(Parser *parser) {
    int line = parser->current.line;
    // The lexer emits a NEWLINE before INDENT, so skip it
    if (check(parser, TOKEN_NEWLINE)) advance(parser);
    consume(parser, TOKEN_INDENT, "Expected indented block after ':'.");

    AstNodeList *stmts = NULL;
    while (!check(parser, TOKEN_DEDENT) && !check(parser, TOKEN_EOF)) {
        stmts = ast_list_append(stmts, declaration(parser));
        // Statement separator: zero or more NEWLINEs. Compound statements
        // (if/while/for/fn) already consumed their own closing DEDENT while
        // parsing their nested block, so the token right after them is the
        // next statement directly — there's no NEWLINE token to require.
        while (check(parser, TOKEN_NEWLINE)) advance(parser);
    }
    consume(parser, TOKEN_DEDENT, "Expected dedent to close block.");
    return ast_new_block(stmts, line);
}

// ---------------------------------------------------------------------------
// Statement parsing
// ---------------------------------------------------------------------------
static AstNode *print_statement(Parser *parser) {
    int line = parser->previous.line;
    AstNode *expr = expression(parser);
    return ast_new_print_stmt(expr, line);
}

static AstNode *let_statement(Parser *parser) {
    int line = parser->previous.line;
    consume(parser, TOKEN_IDENTIFIER, "Expected variable name after 'let'.");
    const char *name = khan_strndup(parser->previous.start, parser->previous.length);
    AstNode *initializer = NULL;
    if (match(parser, TOKEN_EQUAL)) {
        initializer = expression(parser);
    }
    return ast_new_let_stmt(name, initializer, line);
}

static AstNode *if_statement(Parser *parser) {
    int line = parser->previous.line;
    AstNode *cond = expression(parser);
    consume(parser, TOKEN_COLON, "Expected ':' after if condition.");
    AstNode *then_branch = block(parser);

    AstNode *else_branch = NULL;
    if (match(parser, TOKEN_ELIF)) {
        // elif becomes a nested if in the else branch
        else_branch = if_statement(parser);
    } else if (match(parser, TOKEN_ELSE)) {
        consume(parser, TOKEN_COLON, "Expected ':' after else.");
        else_branch = block(parser);
    }
    return ast_new_if_stmt(cond, then_branch, else_branch, line);
}

static AstNode *while_statement(Parser *parser) {
    int line = parser->previous.line;
    AstNode *cond = expression(parser);
    consume(parser, TOKEN_COLON, "Expected ':' after while condition.");
    AstNode *body = block(parser);
    return ast_new_while_stmt(cond, body, line);
}

static AstNode *for_statement(Parser *parser) {
    int line = parser->previous.line;
    consume(parser, TOKEN_IDENTIFIER, "Expected loop variable name after 'for'.");
    const char *var_name = khan_strndup(parser->previous.start, parser->previous.length);
    consume(parser, TOKEN_IN, "Expected 'in' after loop variable.");
    AstNode *iterable = expression(parser);
    consume(parser, TOKEN_COLON, "Expected ':' after for-loop iterable.");
    AstNode *body = block(parser);
    AstNode *node = ast_new_for_stmt(var_name, iterable, body, line);
    free((void *)var_name); // ast_new_for_stmt strdup'd it
    return node;
}

// try: <block> catch [name]: <block>
// The catch clause is required — there's no bare "try" without a
// catch in this grammar (matches how the VM's OP_TRY_BEGIN/OP_TRY_END
// pair is emitted: every try always has exactly one catch target).
// Binding a name is optional ("catch:" vs "catch e:") — see AST_TRY_STMT
// in ast.h for how catch_var == NULL is used to discard the caught
// value at compile time instead of binding it.
static AstNode *try_statement(Parser *parser) {
    int line = parser->previous.line;
    consume(parser, TOKEN_COLON, "Expected ':' after 'try'.");
    AstNode *try_block = block(parser);

    consume(parser, TOKEN_CATCH, "Expected 'catch' after try block.");
    const char *catch_var = NULL;
    if (check(parser, TOKEN_IDENTIFIER)) {
        advance(parser);
        catch_var = khan_strndup(parser->previous.start, parser->previous.length);
    }
    consume(parser, TOKEN_COLON, "Expected ':' after 'catch'.");
    AstNode *catch_block = block(parser);

    AstNode *node = ast_new_try_stmt(try_block, catch_var, catch_block, line);
    if (catch_var) free((void *)catch_var); // ast_new_try_stmt strdup'd it
    return node;
}

static AstNode *throw_statement(Parser *parser) {
    int line = parser->previous.line;
    AstNode *expr = expression(parser);
    return ast_new_throw_stmt(expr, line);
}

static AstNode *fn_declaration(Parser *parser) {
    int line = parser->previous.line;
    consume(parser, TOKEN_IDENTIFIER, "Expected function name after 'fn'.");
    const char *name = khan_strndup(parser->previous.start, parser->previous.length);

    AstNodeList *params = NULL;
    if (match(parser, TOKEN_LPAREN)) {
        // Parameter list
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                consume(parser, TOKEN_IDENTIFIER, "Expected parameter name.");
                params = ast_list_append(params,
                    ast_new_identifier(
                        khan_strndup(parser->previous.start, parser->previous.length),
                        parser->previous.line));
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after parameters.");
    }

    consume(parser, TOKEN_COLON, "Expected ':' after function declaration.");
    AstNode *body = block(parser);
    return ast_new_fn_decl(name, params, body, line);
}

// class Name: <one or more `fn method(self, ...): ...` bodies>
// class Name(Base): <...>   — single inheritance
//
// `self` is an ordinary, explicit first parameter — same as Python's own
// actual convention (a Python method's `self` is a real parameter too;
// `d.bark()` is sugar for `Dog.bark(d)`), not compiler magic. This keeps
// method dispatch a straightforward "prepend the receiver as arg 0" rule
// (see finish_call's TOKEN_DOT handling and AST_METHOD_CALL) rather than
// needing a bound-method value type — see docs/classes.md for the fuller
// design writeup and its v1 scope limits (no `let m = obj.method`
// bound-method references, no multiple inheritance).
static AstNode *class_declaration(Parser *parser) {
    int line = parser->previous.line;
    consume(parser, TOKEN_IDENTIFIER, "Expected class name after 'class'.");
    const char *name = khan_strndup(parser->previous.start, parser->previous.length);

    // Optional `(Base)` — single inheritance only, no comma-separated
    // base list. `Base` must already be a defined class by the time this
    // statement runs (ordinary top-down script execution, same
    // requirement Python has) — see docs/classes.md.
    const char *super_name = NULL;
    if (match(parser, TOKEN_LPAREN)) {
        consume(parser, TOKEN_IDENTIFIER, "Expected superclass name after '('.");
        super_name = khan_strndup(parser->previous.start, parser->previous.length);
        consume(parser, TOKEN_RPAREN, "Expected ')' after superclass name.");
    }
    consume(parser, TOKEN_COLON, "Expected ':' after class name.");

    // Reuses block()'s indent/dedent handling exactly like a function body
    // does, then requires every statement inside to be a `fn` declaration
    // — a class body isn't just "a block of statements" the way a function
    // body is, it's specifically a set of methods.
    AstNode *body = block(parser);
    AstNodeList *methods = NULL;
    for (AstNodeList *s = body->data.statements; s; s = s->next) {
        if (s->node->type != AST_FN_DECL) {
            error(parser, "Only 'fn' method declarations are allowed directly inside a class body.");
            continue;
        }
        methods = ast_list_append(methods, s->node);
        s->node = NULL; // ownership transferred to `methods`; don't let
                         // the shell block below free it out from under us
    }
    // Free just the now-emptied AST_BLOCK shell and its list nodes — every
    // method AstNode itself was already detached above.
    ast_free(body);

    AstNode *node = ast_new_class_stmt(name, super_name, methods, line);
    free((void *)name); // ast_new_class_stmt strdup'd it
    if (super_name) free((void *)super_name);
    return node;
}

static AstNode *return_statement(Parser *parser) {
    int line = parser->previous.line;
    AstNode *value = expression(parser);
    return ast_new_return_stmt(value, line);
}

static AstNode *import_statement(Parser *parser) {
    int line = parser->previous.line;
    consume(parser, TOKEN_STRING, "Expected a string literal after 'import'.");
    // Strip surrounding quotes from the token
    const char *raw = parser->previous.start;
    int len = parser->previous.length;
    char *path = malloc(len - 1);
    strncpy(path, raw + 1, len - 2);
    path[len - 2] = '\0';
    AstNode *node = ast_new_import_stmt(path, line);
    free(path);
    return node;
}

// ---------------------------------------------------------------------------
// from_import_statement — "from" <module-name-or-"string"> "import" name (, name)*
//
// The module reference accepts either a bare identifier (matching the
// existing package-name resolution used by `import "name"`, e.g.
// `from webi import route, run`) or a quoted string path (for relative
// files, e.g. `from "./helpers.kh" import my_fn`).
// ---------------------------------------------------------------------------
static AstNode *from_import_statement(Parser *parser) {
    int line = parser->previous.line;

    char *path = NULL;
    if (check(parser, TOKEN_STRING)) {
        advance(parser);
        const char *raw = parser->previous.start;
        int len = parser->previous.length;
        path = malloc(len - 1);
        strncpy(path, raw + 1, len - 2);
        path[len - 2] = '\0';
    } else if (check(parser, TOKEN_IDENTIFIER)) {
        advance(parser);
        int len = parser->previous.length;
        path = malloc(len + 1);
        strncpy(path, parser->previous.start, len);
        path[len] = '\0';
    } else {
        error_at_current(parser, "Expected a module name after 'from'.");
        return ast_new_nil(line);
    }

    consume(parser, TOKEN_IMPORT, "Expected 'import' after module name.");

    char **names = NULL;
    int count = 0, cap = 0;
    do {
        consume(parser, TOKEN_IDENTIFIER, "Expected a name to import.");
        if (count == cap) {
            cap = (cap == 0) ? 4 : cap * 2;
            names = realloc(names, sizeof(char *) * cap);
        }
        int nlen = parser->previous.length;
        char *n = malloc(nlen + 1);
        strncpy(n, parser->previous.start, nlen);
        n[nlen] = '\0';
        names[count++] = n;
    } while (match(parser, TOKEN_COMMA));

    AstNode *node = ast_new_from_import_stmt(path, names, count, line);

    free(path);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);

    return node;
}

// ---------------------------------------------------------------------------
// Top-level declaration dispatcher
// ---------------------------------------------------------------------------
static AstNode *declaration(Parser *parser) {
    if (match(parser, TOKEN_LET))    return let_statement(parser);
    if (match(parser, TOKEN_FN))     return fn_declaration(parser);
    if (match(parser, TOKEN_CLASS))  return class_declaration(parser);
    if (match(parser, TOKEN_IMPORT)) return import_statement(parser);
    if (match(parser, TOKEN_FROM))   return from_import_statement(parser);

    // Otherwise it's a statement
    return statement(parser);
}

static AstNode *statement(Parser *parser) {
    if (match(parser, TOKEN_PRINT))    return print_statement(parser);
    if (match(parser, TOKEN_IF))       return if_statement(parser);
    if (match(parser, TOKEN_WHILE))    return while_statement(parser);
    if (match(parser, TOKEN_FOR))      return for_statement(parser);
    if (match(parser, TOKEN_TRY))      return try_statement(parser);
    if (match(parser, TOKEN_THROW))    return throw_statement(parser);
    if (match(parser, TOKEN_RETURN))   return return_statement(parser);
    if (match(parser, TOKEN_BREAK)) {
        int line = parser->previous.line;
        while (match(parser, TOKEN_NEWLINE)) {}
        return ast_new_break_stmt(line);
    }
    if (match(parser, TOKEN_CONTINUE)) {
        int line = parser->previous.line;
        while (match(parser, TOKEN_NEWLINE)) {}
        return ast_new_continue_stmt(line);
    }

    // Expression statement
    int line = parser->current.line;
    AstNode *expr = expression(parser);
    return ast_new_expr_stmt(expr, line);
}

// ---------------------------------------------------------------------------
// Expression parsing (Pratt / precedence climbing)
// ---------------------------------------------------------------------------
static AstNode *expression(Parser *parser) {
    return assignment(parser);
}

static AstNode *assignment(Parser *parser) {
    AstNode *left = logic_or(parser);

    if (match(parser, TOKEN_EQUAL)) {
        if (left->type == AST_INDEX) {
            AstNode *value = assignment(parser);
            AstNode *object = left->data.index_expr.object;
            AstNode *index = left->data.index_expr.index;
            int line = left->line;
            free(left); // shell only — children reused, not freed
            return ast_new_index_assign(object, index, value, line);
        }
        if (left->type == AST_GET_ATTR) {
            AstNode *value = assignment(parser);
            AstNode *object = left->data.get_attr.object;
            const char *attr_name = left->data.get_attr.attr_name;
            int line = left->line;
            AstNode *result = ast_new_set_attr(object, attr_name, value, line);
            free((void *)attr_name); // ast_new_set_attr strdup'd its own copy
            free(left); // shell only — object reused, not freed
            return result;
        }
        if (left->type != AST_IDENTIFIER) {
            error(parser, "Left-hand side of assignment must be a variable.");
            // Discard the right-hand side
            expression(parser);
            return left;
        }
        AstNode *value = assignment(parser);
        // Re-purpose as assignment node
        AstNode *assign = ast_new_assignment(left->data.name, value, left->line);
        ast_free(left);  // free the old identifier node — but keep the name strdup'd
        return assign;
    }
    return left;
}

static AstNode *logic_or(Parser *parser) {
    AstNode *left = logic_and(parser);

    while (match(parser, TOKEN_OR)) {
        AstOp op = OP_OR;
        AstNode *right = logic_and(parser);
        left = ast_new_binary(left, op, right, parser->previous.line);
    }
    return left;
}

static AstNode *logic_and(Parser *parser) {
    AstNode *left = equality(parser);

    while (match(parser, TOKEN_AND)) {
        AstOp op = OP_AND;
        AstNode *right = equality(parser);
        left = ast_new_binary(left, op, right, parser->previous.line);
    }
    return left;
}

static AstNode *equality(Parser *parser) {
    AstNode *left = comparison(parser);

    while (match(parser, TOKEN_EQUAL_EQUAL) || match(parser, TOKEN_BANG_EQUAL)) {
        AstOp op = parser->previous.type == TOKEN_EQUAL_EQUAL ? OP_EQUAL_EQUAL : OP_BANG_EQUAL;
        AstNode *right = comparison(parser);
        left = ast_new_binary(left, op, right, parser->previous.line);
    }
    return left;
}

static AstNode *comparison(Parser *parser) {
    AstNode *left = term(parser);

    for (;;) {
        AstOp op;
        if (match(parser, TOKEN_LESS))           op = OP_LESS;
        else if (match(parser, TOKEN_LESS_EQUAL)) op = OP_LESS_EQUAL;
        else if (match(parser, TOKEN_GREATER))    op = OP_GREATER;
        else if (match(parser, TOKEN_GREATER_EQUAL)) op = OP_GREATER_EQUAL;
        else break;

        AstNode *right = term(parser);
        left = ast_new_binary(left, op, right, parser->previous.line);
    }
    return left;
}

static AstNode *term(Parser *parser) {
    AstNode *left = factor(parser);

    while (match(parser, TOKEN_PLUS) || match(parser, TOKEN_MINUS)) {
        AstOp op = parser->previous.type == TOKEN_PLUS ? OP_PLUS : OP_MINUS;
        AstNode *right = factor(parser);
        left = ast_new_binary(left, op, right, parser->previous.line);
    }
    return left;
}

static AstNode *factor(Parser *parser) {
    AstNode *left = unary(parser);

    while (match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH) || match(parser, TOKEN_PERCENT)) {
        AstOp op;
        if (parser->previous.type == TOKEN_STAR)    op = OP_STAR;
        else if (parser->previous.type == TOKEN_SLASH) op = OP_SLASH;
        else op = OP_PERCENT;
        AstNode *right = unary(parser);
        left = ast_new_binary(left, op, right, parser->previous.line);
    }
    return left;
}

static AstNode *unary(Parser *parser) {
    if (match(parser, TOKEN_MINUS)) {
        return ast_new_unary(OP_NEGATE, unary(parser), parser->previous.line);
    }
    if (match(parser, TOKEN_NOT)) {
        return ast_new_unary(OP_NOT, unary(parser), parser->previous.line);
    }
    return call(parser);
}

// ---------------------------------------------------------------------------
// Call and primary
// ---------------------------------------------------------------------------
static AstNode *finish_call(Parser *parser, AstNode *callee) {
    AstNodeList *args = NULL;

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            args = ast_list_append(args, expression(parser));
        } while (match(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after arguments.");

    // Fast path: callee is a bare identifier, e.g. foo(x) — store by name.
    if (callee->type == AST_IDENTIFIER) {
        const char *callee_name = strdup(callee->data.name);
        ast_free(callee);  // Don't need the identifier node anymore
        AstNode *call_node = ast_new_call(callee_name, args, parser->previous.line);
        free((void *)callee_name);  // ast_new_call strdup'd it again
        return call_node;
    }

    // General path: callee is any other expression, e.g. arr[i](x),
    // obj["fn"](x), or a chained call's result. Store the expression
    // itself; the interpreter evaluates it at call time.
    return ast_new_call_expr(callee, args, parser->previous.line);
}

static AstNode *finish_index(Parser *parser, AstNode *object) {
    AstNode *index = expression(parser);
    consume(parser, TOKEN_RBRACKET, "Expected ']' after index.");
    return ast_new_index(object, index, parser->previous.line);
}

// obj.name -> AST_GET_ATTR (field read; caller may later turn this into
// AST_SET_ATTR if a '=' follows, see assignment()'s AST_GET_ATTR case)
// obj.name(args) -> AST_METHOD_CALL directly, skipping AST_GET_ATTR
// entirely — see AST_METHOD_CALL's own doc comment in ast.h for why this
// is fused into one node rather than composed from a GET_ATTR + a call.
static AstNode *finish_dot(Parser *parser, AstNode *object) {
    consume(parser, TOKEN_IDENTIFIER, "Expected property or method name after '.'.");
    const char *name = khan_strndup(parser->previous.start, parser->previous.length);
    int line = parser->previous.line;

    if (match(parser, TOKEN_LPAREN)) {
        AstNodeList *args = NULL;
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                args = ast_list_append(args, expression(parser));
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after method arguments.");
        AstNode *node = ast_new_method_call(object, name, args, line);
        free((void *)name); // ast_new_method_call strdup'd it
        return node;
    }

    AstNode *node = ast_new_get_attr(object, name, line);
    free((void *)name); // ast_new_get_attr strdup'd it
    return node;
}

static AstNode *call(Parser *parser) {
    AstNode *left = primary(parser);

    for (;;) {
        if (match(parser, TOKEN_LPAREN)) {
            left = finish_call(parser, left);
        } else if (match(parser, TOKEN_LBRACKET)) {
            left = finish_index(parser, left);
        } else if (match(parser, TOKEN_DOT)) {
            left = finish_dot(parser, left);
        } else {
            break;
        }
    }
    return left;
}

static AstNode *primary(Parser *parser) {
    if (match(parser, TOKEN_NUMBER)) {
        char *end;
        double value = strtod(parser->previous.start, &end);
        return ast_new_number(value, parser->previous.line);
    }

    if (match(parser, TOKEN_STRING)) {
        // Strip surrounding quotes, then process escape sequences (\n, \t, etc.)
        const char *raw = parser->previous.start;
        int len = parser->previous.length;
        char *str = unescape_string(raw + 1, len - 2); // exclude the quotes
        AstNode *node = ast_new_string(str, parser->previous.line);
        free(str);
        return node;
    }

    if (match(parser, TOKEN_IDENTIFIER)) {
        const char *name = khan_strndup(parser->previous.start, parser->previous.length);
        return ast_new_identifier(name, parser->previous.line);
    }

    // `super.method(args)` — the only form `super` may appear in. Resolved
    // statically against the enclosing class's declared superclass (see
    // AST_SUPER_CALL in ast.h and docs/classes.md), so it's parsed as its
    // own complete node here rather than falling through to call()'s
    // generic '.'/'(' handling.
    if (match(parser, TOKEN_SUPER)) {
        int line = parser->previous.line;
        consume(parser, TOKEN_DOT, "Expected '.' after 'super'.");
        consume(parser, TOKEN_IDENTIFIER, "Expected method name after 'super.'.");
        const char *name = khan_strndup(parser->previous.start, parser->previous.length);
        consume(parser, TOKEN_LPAREN, "Expected '(' after 'super.<method>'.");
        AstNodeList *args = NULL;
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                args = ast_list_append(args, expression(parser));
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after 'super' call arguments.");
        AstNode *node = ast_new_super_call(name, args, line);
        free((void *)name); // ast_new_super_call strdup'd it
        return node;
    }

    if (match(parser, TOKEN_TRUE)) {
        return ast_new_bool(1, parser->previous.line);
    }

    if (match(parser, TOKEN_FALSE)) {
        return ast_new_bool(0, parser->previous.line);
    }

    if (match(parser, TOKEN_NIL)) {
        return ast_new_nil(parser->previous.line);
    }

    if (match(parser, TOKEN_LPAREN)) {
        AstNode *expr = expression(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after expression.");
        return ast_new_grouping(expr, parser->previous.line);
    }

    if (match(parser, TOKEN_LBRACKET)) {
        int line = parser->previous.line;
        AstNodeList *elements = NULL;
        if (!check(parser, TOKEN_RBRACKET)) {
            do {
                elements = ast_list_append(elements, expression(parser));
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RBRACKET, "Expected ']' after array elements.");
        return ast_new_array(elements, line);
    }

    if (match(parser, TOKEN_LBRACE)) {
        int line = parser->previous.line;
        AstNodeList *entries = NULL;
        if (!check(parser, TOKEN_RBRACE)) {
            do {
                AstNode *key = expression(parser);
                consume(parser, TOKEN_COLON, "Expected ':' after map key.");
                AstNode *value = expression(parser);
                entries = ast_list_append(entries, ast_new_map_entry(key, value, line));
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RBRACE, "Expected '}' after map entries.");
        return ast_new_map(entries, line);
    }

    // Error: unexpected token
    error_at_current(parser, "Expected expression.");
    // Advance past it to avoid infinite loops
    advance(parser);
    return ast_new_number(0.0, parser->previous.line); // dummy
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void parser_init(Parser *parser, Lexer *lexer, const char *filename) {
    parser->lexer = lexer;
    parser->had_error = 0;
    parser->current = (Token){0};
    parser->previous = (Token){0};
    parser->filename = filename;
    // Prime the token stream
    advance(parser);
}

AstNode *parser_parse(Parser *parser) {
    AstNodeList *stmts = NULL;

    // Skip leading NEWLINE tokens (blank lines at top level)
    while (match(parser, TOKEN_NEWLINE)) {}

    while (!check(parser, TOKEN_EOF)) {
        stmts = ast_list_append(stmts, declaration(parser));

        // Skip newlines between top-level statements (blank lines are
        // transparent to the lexer, so NEWLINE may or may not be present)
        while (match(parser, TOKEN_NEWLINE)) {}
    }

    int line = 1;
    return ast_new_program(stmts, line);
}