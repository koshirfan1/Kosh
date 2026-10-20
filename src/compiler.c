#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "compiler.h"
#include "ast.h"
#include "chunk.h"
#include "value.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"

/* ══════════════════════════════════════════════════════════════
   Compiler state
   ══════════════════════════════════════════════════════════════ */

#define MAX_LOCALS  256
#define MAX_BREAKS  64
#define MAX_IMPORTED 128

typedef struct {
    char name[256];
    int  depth;
} Local;

/* Per-loop break/continue patch list */
typedef struct {
    int break_patches[MAX_BREAKS];
    int break_count;
    int continue_patches[MAX_BREAKS];
    int continue_count;
    int loop_start;           /* for continue: where to jump back to */
    int scope_depth;          /* scope depth just before the loop's per-iteration
                                  scope begins — break/continue pop back to this */
} LoopCtx;

typedef struct {
    char               *base_path;
    char               *current_import_dir;
    char               *imported_paths[MAX_IMPORTED];
    int                 imported_count;
} ImportCtx;

typedef struct CompilerState {
    KhanFunction       *fn;           /* function being compiled */
    Local               locals[MAX_LOCALS];
    int                 local_count;
    int                 scope_depth;  /* 0 = global */
    struct CompilerState *enclosing;

    /* Compile-time record of free variables this function captures from
       an enclosing function — mirrored at runtime into fn->upvalues so
       the VM knows what to snapshot into each closure instance. */
    UpvalueDesc          upvalues[MAX_LOCALS];
    int                  upvalue_count;

    LoopCtx             loops[16];
    int                 loop_depth;

    int                 had_error;

    /* Import tracking (shared across frames) */
    ImportCtx          *imports;

    /* Set only while compiling a method of a class declared with a
       `(Base)` clause — the declared superclass's NAME (not resolved to
       a value; that happens at runtime). NULL everywhere else, including
       for ordinary functions and for methods of a class with no
       superclass. `super.foo()` compiles by consulting this. */
    const char         *class_super_name;
} CompilerState;

static CompilerState *current = NULL;

/* ── helpers ── */

static void compiler_error(const char *msg, int line) {
    fprintf(stderr, "[line %d] Compile error: %s\n", line, msg);
    if (current) current->had_error = 1;
}

static Chunk *cur_chunk(void) { return &current->fn->chunk; }

static void emit(uint8_t byte, int line) {
    chunk_write(cur_chunk(), byte, line);
}

static void emit2(uint8_t op, uint8_t operand, int line) {
    emit(op, line);
    emit(operand, line);
}

/* emit a 2-byte big-endian jump placeholder; return offset of 1st placeholder */
static int emit_jump(uint8_t op, int line) {
    emit(op, line);
    emit(0xFF, line);
    emit(0xFF, line);
    return cur_chunk()->count - 2;
}

static void patch_jump(int offset) {
    int jump = cur_chunk()->count - offset - 2;
    if (jump > 0xFFFF) {
        compiler_error("Jump too large", 0);
        return;
    }
    cur_chunk()->code[offset]     = (jump >> 8) & 0xFF;
    cur_chunk()->code[offset + 1] = jump & 0xFF;
}

static void emit_loop(int loop_start, int line) {
    emit(OP_LOOP, line);
    int offset = cur_chunk()->count - loop_start + 2;
    emit((offset >> 8) & 0xFF, line);
    emit(offset & 0xFF, line);
}

/* emit a 2-byte big-endian operand */
static void emit_short(uint16_t value, int line) {
    emit((value >> 8) & 0xFF, line);
    emit(value & 0xFF, line);
}

/* Add a value to the constant pool and emit OP_CONST or OP_CONST_WIDE */
static void emit_const(Value v, int line) {
    int idx = chunk_add_const(cur_chunk(), v);
    if (idx <= 255) {
        emit2(OP_CONST, (uint8_t)idx, line);
    } else {
        emit(OP_CONST_WIDE, line);
        emit_short((uint16_t)idx, line);
    }
}

/* ── Scope helpers ── */

static void begin_scope(void) { current->scope_depth++; }

static void end_scope(int line) {
    current->scope_depth--;
    while (current->local_count > 0 &&
           current->locals[current->local_count - 1].depth > current->scope_depth) {
        emit(OP_POP, line);
        current->local_count--;
    }
}

static int resolve_local(CompilerState *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--)
        if (strcmp(c->locals[i].name, name) == 0)
            return i;
    return -1;
}

/* Record (or reuse) a captured-variable slot on function `c`, returning
   its upvalue index. `is_local` = 1 means it comes straight from the
   enclosing frame's local stack slot `index`; = 0 means it's forwarded
   from the enclosing function's own upvalue slot `index` (multi-level
   nesting). */
static int add_upvalue(CompilerState *c, int is_local, int index) {
    for (int i = 0; i < c->upvalue_count; i++) {
        if (c->upvalues[i].is_local == is_local && c->upvalues[i].index == index)
            return i;
    }
    if (c->upvalue_count >= MAX_LOCALS) {
        compiler_error("Too many captured variables in one function", 0);
        return 0;
    }
    c->upvalues[c->upvalue_count].is_local = is_local;
    c->upvalues[c->upvalue_count].index    = index;
    return c->upvalue_count++;
}

/* Walks the chain of enclosing compiler states looking for `name` as a
   local variable (a parameter or `let`) of some ancestor function. If
   found, threads an upvalue capture through every function between here
   and there, so nested functions more than one level deep still resolve
   correctly. Returns the upvalue index in `c`, or -1 if `name` isn't a
   local anywhere in the enclosing chain (i.e. it's a global). */
static int resolve_upvalue(CompilerState *c, const char *name) {
    if (!c->enclosing) return -1;

    int local = resolve_local(c->enclosing, name);
    if (local >= 0) {
        return add_upvalue(c, 1, local);
    }

    int outer_upvalue = resolve_upvalue(c->enclosing, name);
    if (outer_upvalue >= 0) {
        return add_upvalue(c, 0, outer_upvalue);
    }

    return -1;
}

static void add_local(const char *name) {
    if (current->local_count >= MAX_LOCALS) {
        compiler_error("Too many local variables", 0);
        return;
    }
    Local *l = &current->locals[current->local_count++];
    strncpy(l->name, name, 255);
    l->name[255] = '\0';
    l->depth = current->scope_depth;
}

/* Emit name string to constants and GET/SET global */
static void emit_global_get(const char *name, int line) {
    int idx = chunk_add_const(cur_chunk(), vm_val_string(name));
    if (idx <= 255) {
        emit2(OP_GET_GLOBAL, (uint8_t)idx, line);
    } else {
        emit(OP_GET_GLOBAL_WIDE, line);
        emit_short((uint16_t)idx, line);
    }
}

static void emit_global_set(const char *name, int line) {
    int idx = chunk_add_const(cur_chunk(), vm_val_string(name));
    if (idx <= 255) {
        emit2(OP_SET_GLOBAL, (uint8_t)idx, line);
    } else {
        emit(OP_SET_GLOBAL_WIDE, line);
        emit_short((uint16_t)idx, line);
    }
}

static void emit_global_def(const char *name, int line) {
    int idx = chunk_add_const(cur_chunk(), vm_val_string(name));
    if (idx <= 255) {
        emit2(OP_DEF_GLOBAL, (uint8_t)idx, line);
    } else {
        emit(OP_DEF_GLOBAL_WIDE, line);
        emit_short((uint16_t)idx, line);
    }
}

/* ── CompilerState init/end ── */

static void compiler_state_init(CompilerState *c, KhanFunction *fn,
                                CompilerState *enclosing) {
    c->fn           = fn;
    c->local_count  = 0;
    c->scope_depth  = 0;
    c->enclosing    = enclosing;
    c->upvalue_count = 0;
    c->loop_depth   = 0;
    c->had_error    = 0;
    c->class_super_name = NULL;

    if (enclosing) {
        c->imports = enclosing->imports;
    } else {
        c->imports = malloc(sizeof(ImportCtx));
        c->imports->base_path = NULL;
        c->imports->current_import_dir = NULL;
        c->imports->imported_count = 0;
    }

    if (c->imports->current_import_dir) {
        fn->source_dir = strdup(c->imports->current_import_dir);
    }
}

/* ══════════════════════════════════════════════════════════════
   Forward declarations
   ══════════════════════════════════════════════════════════════ */
static void compile_expr(AstNode *node);
static void compile_stmt(AstNode *node);
static void compile_block(AstNodeList *stmts, int line);
static void emit_name_op(uint8_t narrow, uint8_t wide, const char *name, int line);

static char *compiler_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0L, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *buffer = malloc(size + 1);
    size_t n = fread(buffer, 1, size, file);
    buffer[n] = '\0';
    fclose(file);
    return buffer;
}

static void compile_import(const char *path, int line) {
    // Built-in native-only imports (no-op). NOTE: "math" is intentionally
    // NOT here — packages/math/math.kh is a real package (vec2/vec3/mat4/
    // quaternion/etc.) and must be compiled normally like any other import.
    // It used to be special-cased here under the assumption that "math"
    // only meant native functions (sqrt/pow/abs/...), which silently
    // defeated the real package every time it was imported.
    if (strcmp(path, "json") == 0) return;

    // Prevent double imports
    for (int i = 0; i < current->imports->imported_count; i++) {
        if (strcmp(current->imports->imported_paths[i], path) == 0) return;
    }
    if (current->imports->imported_count >= MAX_IMPORTED) {
        compiler_error("Too many imports", line);
        return;
    }

    char full_path[1024];
    FILE *file = NULL;

    // Resolve path (logic from interpreter.c)
    if (current->imports->current_import_dir) {
        snprintf(full_path, sizeof(full_path), "%s/%s", current->imports->current_import_dir, path);
        file = fopen(full_path, "rb");
    }
    if (!file && current->imports->base_path) {
        snprintf(full_path, sizeof(full_path), "%s/%s", current->imports->base_path, path);
        file = fopen(full_path, "rb");
    }
    if (!file) {
        file = fopen(path, "rb");
        if (file) strncpy(full_path, path, sizeof(full_path));
    }
    if (!file) {
        // Try package
        int is_pkg = 1;
        for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\' || *p == '.') { is_pkg = 0; break; }
        if (is_pkg) {
            // 1. Try local packages folder
            snprintf(full_path, sizeof(full_path), "packages/%s/%s.kh", path, path);
            file = fopen(full_path, "rb");

            if (!file) {
                // 2. Try home directory cache
                char *home = getenv("USERPROFILE");
                if (!home) home = getenv("HOME");
                if (home) {
#ifdef _WIN32
                    snprintf(full_path, sizeof(full_path), "%s\\.khan\\packages\\%s\\%s.kh", home, path, path);
#else
                    snprintf(full_path, sizeof(full_path), "%s/.khan/packages/%s/%s.kh", home, path, path);
#endif
                    file = fopen(full_path, "rb");
                }
            }
        }
    }

    if (!file) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not open import '%s'", path);
        compiler_error(msg, line);
        return;
    }
    fclose(file);

    char *source = compiler_read_file(full_path);
    if (!source) {
        compiler_error("Could not read import file", line);
        return;
    }

    Lexer lexer;
    lexer_init(&lexer, source);
    Parser parser;
    parser_init(&parser, &lexer, full_path);
    AstNode *program = parser_parse(&parser);

    if (parser.had_error) {
        compiler_error("Syntax error in import", line);
        free(source);
        return;
    }

    // Mark as imported
    current->imports->imported_paths[current->imports->imported_count++] = strdup(path);

    // Save old import dir
    char *old_dir = current->imports->current_import_dir ? strdup(current->imports->current_import_dir) : NULL;

    // Set new import dir
    char *last_slash = strrchr(full_path, '/');
    char *last_backslash = strrchr(full_path, '\\');
    char *sep = (last_slash > last_backslash) ? last_slash : last_backslash;
    if (sep) {
        int dlen = (int)(sep - full_path);
        if (current->imports->current_import_dir) free(current->imports->current_import_dir);
        current->imports->current_import_dir = malloc(dlen + 1);
        memcpy(current->imports->current_import_dir, full_path, dlen);
        current->imports->current_import_dir[dlen] = '\0';
    }

    // Compile the imported program into the current chunk
    if (program->type == AST_PROGRAM) {
        for (AstNodeList *s = program->data.program_stmts; s; s = s->next)
            compile_stmt(s->node);
    } else {
        compile_stmt(program);
    }

    // Restore old import dir
    if (current->imports->current_import_dir) free(current->imports->current_import_dir);
    current->imports->current_import_dir = old_dir;

    ast_free(program);
    free(source);
}

// Import a sibling submodule file of a package (e.g. "requests" or "json"
// next to "webi.kh") directly by its resolved path, independent of the
// package-name-matches-filename convention that compile_import()'s
// "is_pkg" branch assumes. Used to implement the "submodule flatten" case
// of `from X import Y` (see docs/from-import.md, case 3). Returns 1 if a
// matching submodule file was found and compiled, 0 if no such file exists
// (in which case the caller should treat the name as a plain symbol).
static int compile_submodule_import(const char *pkg, const char *sub, int line) {
    char dedup_key[600];
    snprintf(dedup_key, sizeof(dedup_key), "%s/%s", pkg, sub);

    for (int i = 0; i < current->imports->imported_count; i++) {
        if (strcmp(current->imports->imported_paths[i], dedup_key) == 0) return 1;
    }
    if (current->imports->imported_count >= MAX_IMPORTED) {
        compiler_error("Too many imports", line);
        return 1;
    }

    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "packages/%s/%s.kh", pkg, sub);
    FILE *file = fopen(full_path, "rb");

    if (!file) {
        char *home = getenv("USERPROFILE");
        if (!home) home = getenv("HOME");
        if (home) {
#ifdef _WIN32
            snprintf(full_path, sizeof(full_path), "%s\\.khan\\packages\\%s\\%s.kh", home, pkg, sub);
#else
            snprintf(full_path, sizeof(full_path), "%s/.khan/packages/%s/%s.kh", home, pkg, sub);
#endif
            file = fopen(full_path, "rb");
        }
    }

    if (!file) return 0; // Not a submodule file — treat name as a plain symbol.
    fclose(file);

    char *source = compiler_read_file(full_path);
    if (!source) {
        compiler_error("Could not read import file", line);
        return 1;
    }

    Lexer lexer;
    lexer_init(&lexer, source);
    Parser parser;
    parser_init(&parser, &lexer, full_path);
    AstNode *program = parser_parse(&parser);

    if (parser.had_error) {
        compiler_error("Syntax error in import", line);
        free(source);
        return 1;
    }

    current->imports->imported_paths[current->imports->imported_count++] = strdup(dedup_key);

    char *old_dir = current->imports->current_import_dir ? strdup(current->imports->current_import_dir) : NULL;
    char *last_slash = strrchr(full_path, '/');
    char *last_backslash = strrchr(full_path, '\\');
    char *sep = (last_slash > last_backslash) ? last_slash : last_backslash;
    if (sep) {
        int dlen = (int)(sep - full_path);
        if (current->imports->current_import_dir) free(current->imports->current_import_dir);
        current->imports->current_import_dir = malloc(dlen + 1);
        memcpy(current->imports->current_import_dir, full_path, dlen);
        current->imports->current_import_dir[dlen] = '\0';
    }

    if (program->type == AST_PROGRAM) {
        for (AstNodeList *s = program->data.program_stmts; s; s = s->next)
            compile_stmt(s->node);
    } else {
        compile_stmt(program);
    }

    if (current->imports->current_import_dir) free(current->imports->current_import_dir);
    current->imports->current_import_dir = old_dir;

    ast_free(program);
    free(source);
    return 1;
}

static void compile_from_import(const char *path, char **names, int name_count, int line) {
    int imported_base = 0;

    for (int i = 0; i < name_count; i++) {
        // Case 2: the package's own name — full import of the whole package.
        if (strcmp(names[i], path) == 0) {
            compile_import(path, line);
            imported_base = 1;
            continue;
        }
        // Case 3: a sibling submodule file (e.g. `from webi import requests`
        // pulling in packages/webi/requests.kh).
        compile_submodule_import(path, names[i], line);
    }

    // Case 1 (plain symbols): the VM populates a single global table, so a
    // plain-symbol name becomes available once the base package itself has
    // been compiled somewhere. Ensure that's happened at least once — this
    // also preserves the previous behavior for scripts that only ever used
    // plain-symbol from-imports.
    if (!imported_base) compile_import(path, line);
}

/* ══════════════════════════════════════════════════════════════
   Count items in an AstNodeList
   ══════════════════════════════════════════════════════════════ */
static int list_count(AstNodeList *l) {
    int n = 0;
    while (l) { n++; l = l->next; }
    return n;
}

/* ══════════════════════════════════════════════════════════════
   Expression compilation
   ══════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════
   Constant folding
   ══════════════════════════════════════════════════════════════
   Recursively evaluates a compile-time-constant numeric expression
   tree (number literals, +, -, *, /, %, unary minus, and parenthesized
   groupings of the same) and returns true with the result in *out if
   the whole subtree folds. Deliberately conservative:
     - only numbers in, numbers out (no string constant-folding, to
       avoid any risk of subtly diverging from OP_ADD's runtime
       string-concat semantics)
     - division/modulo by a folded-zero divisor is NOT folded — it
       falls through to normal codegen so the existing runtime error
       path still fires, rather than silently producing NaN/Inf (or a
       different error) at compile time
     - AND/OR are intentionally excluded (already special-cased for
       short-circuit codegen elsewhere; folding them isn't worth the
       risk of interacting with that)
   This lets `compile_expr` emit a single OP_CONST for something like
   `60 * 60 * 24` instead of five constants and four ADD/MUL opcodes —
   the classic first compiler optimization, and a safe one since it
   can only ever produce the same value the unfolded bytecode would
   have computed at runtime. */
static int fold_constant_number(AstNode *node, double *out) {
    if (!node) return 0;

    switch (node->type) {
        case AST_NUMBER:
            *out = node->data.number_value;
            return 1;

        case AST_GROUPING:
            return fold_constant_number(node->data.grouping, out);

        case AST_UNARY: {
            double r;
            if (node->data.unary.op != OP_NEGATE) return 0; /* leave `not` to runtime */
            if (!fold_constant_number(node->data.unary.right, &r)) return 0;
            *out = -r;
            return 1;
        }

        case AST_BINARY: {
            double l, r;
            AstOp op = node->data.binary.op;
            if (op == OP_AND || op == OP_OR) return 0;
            if (!fold_constant_number(node->data.binary.left, &l)) return 0;
            if (!fold_constant_number(node->data.binary.right, &r)) return 0;
            switch (op) {
                case OP_PLUS:  *out = l + r; return 1;
                case OP_MINUS: *out = l - r; return 1;
                case OP_STAR:  *out = l * r; return 1;
                case OP_SLASH:
                    if (r == 0) return 0; /* let the runtime raise its own error */
                    *out = l / r;
                    return 1;
                case OP_PERCENT:
                    if (r == 0) return 0;
                    *out = fmod(l, r);
                    return 1;
                default:
                    return 0; /* comparisons handled separately below, not here */
            }
        }

        default:
            return 0;
    }
}

/* Comparison folding is kept separate from fold_constant_number since
   its result is a bool, not a number — same conservatism otherwise
   (both sides must fold to constants). */
static int fold_constant_comparison(AstOp op, AstNode *L, AstNode *R, int *out) {
    double l, r;
    if (!fold_constant_number(L, &l)) return 0;
    if (!fold_constant_number(R, &r)) return 0;
    switch (op) {
        case OP_EQUAL_EQUAL:   *out = (l == r); return 1;
        case OP_BANG_EQUAL:    *out = (l != r); return 1;
        case OP_LESS:          *out = (l <  r); return 1;
        case OP_LESS_EQUAL:    *out = (l <= r); return 1;
        case OP_GREATER:       *out = (l >  r); return 1;
        case OP_GREATER_EQUAL: *out = (l >= r); return 1;
        default:               return 0;
    }
}

static void compile_expr(AstNode *node) {
    if (!node) { emit(OP_NIL, 0); return; }
    int line = node->line;

    switch (node->type) {

    /* ── Literals ── */
    case AST_NUMBER:
        emit_const(vm_val_number(node->data.number_value), line);
        break;

    case AST_STRING:
        emit_const(vm_val_string(node->data.string_value), line);
        break;

    case AST_BOOL:
        emit(node->data.bool_value ? OP_TRUE : OP_FALSE, line);
        break;

    case AST_NIL:
        emit(OP_NIL, line);
        break;

    /* ── Identifier (variable read) ── */
    case AST_IDENTIFIER: {
        const char *name = node->data.name;
        int slot = resolve_local(current, name);
        if (slot >= 0) {
            emit2(OP_GET_LOCAL, (uint8_t)slot, line);
        } else {
            int up = resolve_upvalue(current, name);
            if (up >= 0)
                emit2(OP_GET_UPVALUE, (uint8_t)up, line);
            else
                emit_global_get(name, line);
        }
        break;
    }

    /* ── Grouping ── */
    case AST_GROUPING:
        compile_expr(node->data.grouping);
        break;

    /* ── Binary operations ── */
    case AST_BINARY: {
        AstOp op    = node->data.binary.op;
        AstNode *L  = node->data.binary.left;
        AstNode *R  = node->data.binary.right;

        /* Constant folding: try arithmetic first, then comparisons.
           Short-circuit AND/OR are excluded inside the fold helpers
           themselves, so this is always safe to attempt before the
           special-cased branches below. */
        {
            double folded;
            if (fold_constant_number(node, &folded)) {
                emit_const(vm_val_number(folded), line);
                break;
            }
            int folded_bool;
            if (fold_constant_comparison(op, L, R, &folded_bool)) {
                emit(folded_bool ? OP_TRUE : OP_FALSE, line);
                break;
            }
        }

        /* Short-circuit AND */
        if (op == OP_AND) {
            compile_expr(L);
            int skip = emit_jump(OP_JUMP_IF_FALSE, line);
            emit(OP_POP, line);
            compile_expr(R);
            patch_jump(skip);
            break;
        }

        /* Short-circuit OR */
        if (op == OP_OR) {
            compile_expr(L);
            int else_j = emit_jump(OP_JUMP_IF_FALSE, line);
            int end_j  = emit_jump(OP_JUMP, line);
            patch_jump(else_j);
            emit(OP_POP, line);
            compile_expr(R);
            patch_jump(end_j);
            break;
        }

        compile_expr(L);
        compile_expr(R);

        switch (op) {
            case OP_PLUS:          emit(OP_ADD, line); break;
            case OP_MINUS:         emit(OP_SUB, line); break;
            case OP_STAR:          emit(OP_MUL, line); break;
            case OP_SLASH:         emit(OP_DIV, line); break;
            case OP_PERCENT:       emit(OP_MOD, line); break;
            case OP_EQUAL_EQUAL:   emit(OP_EQ,  line); break;
            case OP_BANG_EQUAL:    emit(OP_NEQ, line); break;
            case OP_LESS:          emit(OP_LT,  line); break;
            case OP_LESS_EQUAL:    emit(OP_LE,  line); break;
            case OP_GREATER:       emit(OP_GT,  line); break;
            case OP_GREATER_EQUAL: emit(OP_GE,  line); break;
            default:
                compiler_error("Unknown binary operator", line);
        }
        break;
    }

    /* ── Unary operations ── */
    case AST_UNARY: {
        double folded;
        if (node->data.unary.op == OP_NEGATE && fold_constant_number(node, &folded)) {
            emit_const(vm_val_number(folded), line);
            break;
        }
        compile_expr(node->data.unary.right);
        switch (node->data.unary.op) {
            case OP_NEGATE: emit(OP_NEGATE_NUM, line); break;
            case OP_NOT:    emit(OP_NOT_BOOL,   line); break;
            default:        compiler_error("Unknown unary operator", line);
        }
        break;
    }

    /* ── Array literal ── */
    case AST_ARRAY: {
        int count = 0;
        for (AstNodeList *el = node->data.array_elements; el; el = el->next) {
            compile_expr(el->node);
            count++;
        }
        /* Bug found and fixed while verifying the test suite actually
         * runs clean end-to-end (tests/suites/arrays_maps.kh's own
         * 300-element regression test — deliberately over the 255
         * boundary — caught this): this always emitted the narrow
         * OP_MAKE_ARRAY with `(uint8_t)count`, silently wrapping any
         * literal with more than 255 elements (300 became 300 % 256 =
         * 44) instead of ever emitting OP_MAKE_ARRAY_WIDE, which the
         * VM has always correctly supported (see vm.c's OP_MAKE_ARRAY
         * / OP_MAKE_ARRAY_WIDE case — that side was fine; this was
         * purely a compiler-side gap). Confirmed present in the
         * original, unmodified project archive — not a regression
         * from this session's other changes. Mirrors emit_const()'s
         * existing narrow/wide selection immediately above in this
         * file. */
        if (count <= 255) {
            emit2(OP_MAKE_ARRAY, (uint8_t)count, line);
        } else {
            emit(OP_MAKE_ARRAY_WIDE, line);
            emit_short((uint16_t)count, line);
        }
        break;
    }

    /* ── Map literal ── */
    case AST_MAP: {
        int pairs = 0;
        for (AstNodeList *el = node->data.map_entries; el; el = el->next) {
            AstNode *entry = el->node;   /* AST_MAP_ENTRY */
            compile_expr(entry->data.map_entry.key);
            compile_expr(entry->data.map_entry.value);
            pairs++;
        }
        /* Same bug, same fix as AST_ARRAY just above — see that case's
         * comment. A map literal with more than 255 key/value pairs
         * would have silently wrapped `pairs` the same way. */
        if (pairs <= 255) {
            emit2(OP_MAKE_MAP, (uint8_t)pairs, line);
        } else {
            emit(OP_MAKE_MAP_WIDE, line);
            emit_short((uint16_t)pairs, line);
        }
        break;
    }

    /* ── Index read  arr[i] ── */
    case AST_INDEX:
        compile_expr(node->data.index_expr.object);
        compile_expr(node->data.index_expr.index);
        emit(OP_GET_INDEX, line);
        break;

    /* ── Function call ── */
    case AST_CALL: {
        /* Push callee — check local first (a parameter/local holding a
           function value), then fall back to global, exactly like a
           plain AST_IDENTIFIER read. Without this, calling a function
           through a parameter or local variable (the whole point of
           higher-order functions: filter(arr, pred), callback(), a
           middleware hook_fn, etc.) always incorrectly did a global
           lookup instead and failed with "Undefined variable". */
        if (node->data.call.callee) {
            int slot = resolve_local(current, node->data.call.callee);
            if (slot >= 0) {
                emit2(OP_GET_LOCAL, (uint8_t)slot, line);
            } else {
                int up = resolve_upvalue(current, node->data.call.callee);
                if (up >= 0)
                    emit2(OP_GET_UPVALUE, (uint8_t)up, line);
                else
                    emit_global_get(node->data.call.callee, line);
            }
        } else {
            compile_expr(node->data.call.callee_expr);
        }

        int argc = 0;
        for (AstNodeList *arg = node->data.call.arguments; arg; arg = arg->next) {
            compile_expr(arg->node);
            argc++;
        }
        emit2(OP_CALL, (uint8_t)argc, line);
        break;
    }

    /* ── Assignment used as expression  (x = value) ── */
    case AST_ASSIGNMENT: {
        compile_expr(node->data.assignment.value);
        const char *name = node->data.assignment.var_name;
        int slot = resolve_local(current, name);
        if (slot >= 0) {
            emit2(OP_SET_LOCAL, (uint8_t)slot, line);
        } else {
            int up = resolve_upvalue(current, name);
            if (up >= 0)
                emit2(OP_SET_UPVALUE, (uint8_t)up, line);
            else
                emit_global_set(name, line);
        }
        /* leave value on stack so it can be used as an expression */
        break;
    }

    /* ── Index assignment used as expression  arr[i] = v ── */
    case AST_INDEX_ASSIGN:
        compile_expr(node->data.index_assign.object);
        compile_expr(node->data.index_assign.index);
        compile_expr(node->data.index_assign.value);
        emit(OP_SET_INDEX, line);
        break;

    /* ── obj.field  (field read) ── */
    case AST_GET_ATTR:
        compile_expr(node->data.get_attr.object);
        emit_name_op(OP_GET_ATTR, OP_GET_ATTR_WIDE, node->data.get_attr.attr_name, line);
        break;

    /* ── obj.field = value  used as an expression ── */
    case AST_SET_ATTR:
        compile_expr(node->data.set_attr.object);
        compile_expr(node->data.set_attr.value);
        emit_name_op(OP_SET_ATTR, OP_SET_ATTR_WIDE, node->data.set_attr.attr_name, line);
        break;

    /* ── obj.method(args) ──
       Stack at dispatch: [receiver][arg1]...[argN]. OP_CALL_METHOD looks
       the method up on the receiver's class and runs it with the
       receiver already in place as `self` (slot 0). */
    case AST_METHOD_CALL: {
        compile_expr(node->data.method_call.object);
        int argc = 0;
        for (AstNodeList *a = node->data.method_call.arguments; a; a = a->next) {
            compile_expr(a->node);
            argc++;
        }
        if (argc > 255) {
            compiler_error("Too many arguments in method call (max 255)", line);
            argc = 255;
        }
        int idx = chunk_add_const(cur_chunk(), vm_val_string(node->data.method_call.method_name));
        if (idx <= 255) {
            emit(OP_CALL_METHOD, line);
            emit((uint8_t)idx, line);
        } else {
            emit(OP_CALL_METHOD_WIDE, line);
            emit_short((uint16_t)idx, line);
        }
        emit((uint8_t)argc, line);
        break;
    }

    /* ── super.method(args) — see docs/classes.md ── */
    case AST_SUPER_CALL: {
        if (!current->class_super_name) {
            compiler_error("'super' used outside a method of a class with a superclass", line);
            break;
        }
        emit2(OP_GET_LOCAL, 0, line);   /* self is always local slot 0 in a method */
        int argc = 0;
        for (AstNodeList *a = node->data.super_call.arguments; a; a = a->next) {
            compile_expr(a->node);
            argc++;
        }
        if (argc > 255) {
            compiler_error("Too many arguments in 'super' call (max 255)", line);
            argc = 255;
        }
        int super_idx = chunk_add_const(cur_chunk(), vm_val_string(current->class_super_name));
        int mname_idx = chunk_add_const(cur_chunk(), vm_val_string(node->data.super_call.method_name));
        emit(OP_CALL_SUPER, line);
        emit_short((uint16_t)super_idx, line);
        emit_short((uint16_t)mname_idx, line);
        emit((uint8_t)argc, line);
        break;
    }

    default:
        compiler_error("Unhandled expression node", line);
        emit(OP_NIL, line);
        break;
    }
}

/* ══════════════════════════════════════════════════════════════
   Statement compilation
   ══════════════════════════════════════════════════════════════ */

/* Emits `narrow <1-byte idx>` or `wide <2-byte idx>` for an opcode whose
   operand is a string-constant (attribute/method) name. */
static void emit_name_op(uint8_t narrow, uint8_t wide, const char *name, int line) {
    int idx = chunk_add_const(cur_chunk(), vm_val_string(name));
    if (idx <= 255) {
        emit2(narrow, (uint8_t)idx, line);
    } else {
        emit(wide, line);
        emit_short((uint16_t)idx, line);
    }
}

/* Compiles one class method (an AST_FN_DECL) into a registered
   KhanFunction and returns its registry index — same body-compilation
   as a top-level `fn`, minus the trailing "define a global" step, since
   a method lives in its class's method table instead. `self` is just
   the first ordinary parameter. */
static int compile_class_method(AstNode *node, const char *super_name) {
    int line = node->line;
    const char *fname = node->data.fn_decl.fn_name;
    int arity = list_count(node->data.fn_decl.params);

    KhanFunction *fn = khanfn_new(fname, arity);

    CompilerState fn_state;
    compiler_state_init(&fn_state, fn, current);
    fn_state.class_super_name = super_name;
    current = &fn_state;
    current->scope_depth = 1;

    for (AstNodeList *p = node->data.fn_decl.params; p; p = p->next)
        add_local(p->node->data.name);

    AstNode *body = node->data.fn_decl.fn_body;
    if (body->type == AST_BLOCK) {
        for (AstNodeList *st = body->data.statements; st; st = st->next)
            compile_stmt(st->node);
    } else {
        compile_stmt(body);
    }

    emit(OP_NIL, line);
    emit(OP_RETURN, line);

    int fn_had_error = current->had_error;
    if (fn_state.upvalue_count > 0) {
        fn->upvalue_count = fn_state.upvalue_count;
        fn->upvalues = malloc(sizeof(UpvalueDesc) * fn_state.upvalue_count);
        memcpy(fn->upvalues, fn_state.upvalues, sizeof(UpvalueDesc) * fn_state.upvalue_count);
    }

    current = fn_state.enclosing;
    if (fn_had_error) current->had_error = 1;

    khanfn_register(fn);
    return khanfn_registry_index();
}

static void compile_block(AstNodeList *stmts, int line) {
    begin_scope();
    for (AstNodeList *s = stmts; s; s = s->next)
        compile_stmt(s->node);
    end_scope(line);
}

static void compile_stmt(AstNode *node) {
    if (!node) return;
    int line = node->line;

    switch (node->type) {

    /* ── let x = expr ── */
    case AST_LET_STMT: {
        const char *name = node->data.let_decl.let_name;
        if (node->data.let_decl.let_initializer)
            compile_expr(node->data.let_decl.let_initializer);
        else
            emit(OP_NIL, line);

        if (current->scope_depth > 0) {
            add_local(name);
            /* value on stack IS the local — no extra emit needed */
        } else {
            emit_global_def(name, line);
        }
        break;
    }

    /* ── x = expr (standalone assignment statement) ── */
    case AST_ASSIGNMENT: {
        compile_expr(node->data.assignment.value);
        const char *name = node->data.assignment.var_name;
        int slot = resolve_local(current, name);
        if (slot >= 0) {
            emit2(OP_SET_LOCAL, (uint8_t)slot, line);
        } else {
            int up = resolve_upvalue(current, name);
            if (up >= 0)
                emit2(OP_SET_UPVALUE, (uint8_t)up, line);
            else
                emit_global_set(name, line);
        }
        emit(OP_POP, line);
        break;
    }

    /* ── arr[i] = expr (standalone) ── */
    case AST_INDEX_ASSIGN:
        compile_expr(node->data.index_assign.object);
        compile_expr(node->data.index_assign.index);
        compile_expr(node->data.index_assign.value);
        emit(OP_SET_INDEX, line);
        emit(OP_POP, line);
        break;

    /* ── obj.field = expr (standalone) ── */
    case AST_SET_ATTR:
        compile_expr(node->data.set_attr.object);
        compile_expr(node->data.set_attr.value);
        emit_name_op(OP_SET_ATTR, OP_SET_ATTR_WIDE, node->data.set_attr.attr_name, line);
        emit(OP_POP, line);
        break;

    /* ── class Name: fn method(self, ...): ... ──
       Stack built for OP_MAKE_CLASS: [class name] then, per method,
       [method name][registry index]. The VM turns each index into a
       real function value and stores it in the new class's method
       table; the class is then bound to a global named after itself,
       so `Name(args)` is an ordinary call that OP_CALL recognizes. */
    case AST_CLASS_STMT: {
        const char *cname = node->data.class_stmt.class_name;
        const char *super_name = node->data.class_stmt.superclass_name;
        emit_const(vm_val_string(cname), line);
        if (super_name) {
            emit_global_get(super_name, line);   /* runtime error if undefined */
        } else {
            emit(OP_NIL, line);
        }
        int method_count = 0;
        for (AstNodeList *m = node->data.class_stmt.methods; m; m = m->next) {
            AstNode *fn_node = m->node;
            int fn_idx = compile_class_method(fn_node, super_name);
            emit_const(vm_val_string(fn_node->data.fn_decl.fn_name), line);
            emit_const(value_number((double)fn_idx), line);
            method_count++;
        }
        if (method_count <= 255) {
            emit2(OP_MAKE_CLASS, (uint8_t)method_count, line);
        } else {
            emit(OP_MAKE_CLASS_WIDE, line);
            emit_short((uint16_t)method_count, line);
        }
        emit_global_def(cname, line);
        break;
    }

    /* ── print expr ── */
    case AST_PRINT_STMT:
        compile_expr(node->data.expr);
        emit(OP_PRINT, line);
        break;

    /* ── expr; ── */
    case AST_EXPR_STMT:
        compile_expr(node->data.expr);
        emit(OP_POP, line);
        break;

    /* ── if / elif / else ── */
    case AST_IF_STMT: {
        compile_expr(node->data.if_stmt.condition);
        int then_jump = emit_jump(OP_JUMP_IF_FALSE, line);
        emit(OP_POP, line);

        compile_stmt(node->data.if_stmt.then_branch);

        int else_jump = emit_jump(OP_JUMP, line);
        patch_jump(then_jump);
        emit(OP_POP, line);

        if (node->data.if_stmt.else_branch)
            compile_stmt(node->data.if_stmt.else_branch);

        patch_jump(else_jump);
        break;
    }

    /* ── while condition: body ── */
    case AST_WHILE_STMT: {
        /* Register loop context for break/continue */
        LoopCtx *ctx = &current->loops[current->loop_depth++];
        ctx->break_count    = 0;
        ctx->continue_count = 0;
        ctx->scope_depth    = current->scope_depth;

        int loop_start = cur_chunk()->count;
        ctx->loop_start = loop_start;

        compile_expr(node->data.while_stmt.while_condition);
        int exit_jump = emit_jump(OP_JUMP_IF_FALSE, line);
        emit(OP_POP, line);

        compile_stmt(node->data.while_stmt.while_body);

        /* Patch continue jumps → top of loop */
        for (int i = 0; i < ctx->continue_count; i++) {
            patch_jump(ctx->continue_patches[i]);
        }

        emit_loop(loop_start, line);
        patch_jump(exit_jump);
        emit(OP_POP, line);

        /* Patch break jumps → after loop */
        for (int i = 0; i < ctx->break_count; i++)
            patch_jump(ctx->break_patches[i]);

        current->loop_depth--;
        break;
    }

    /* ── for var in iterable: body ── */
    case AST_FOR_STMT: {
        /*
         * Desugar to:
         *   let __arr = iterable
         *   let __i   = 0
         *   while __i < len(__arr):
         *       let var = __arr[__i]
         *       body
         *       __i = __i + 1
         */
        begin_scope();

        /* __arr = iterable */
        compile_expr(node->data.for_stmt.for_iterable);
        add_local("__for_arr");

        /* __i = 0 */
        emit_const(vm_val_number(0), line);
        add_local("__for_i");

        /* Register loop context */
        LoopCtx *ctx = &current->loops[current->loop_depth++];
        ctx->break_count    = 0;
        ctx->continue_count = 0;
        ctx->scope_depth    = current->scope_depth;

        int loop_start = cur_chunk()->count;
        ctx->loop_start = loop_start;

        /* condition: __i < len(__arr) */
        int i_slot   = resolve_local(current, "__for_i");
        int arr_slot = resolve_local(current, "__for_arr");

        emit2(OP_GET_LOCAL, (uint8_t)i_slot,   line);
        emit_global_get("len", line);
        emit2(OP_GET_LOCAL, (uint8_t)arr_slot, line);
        emit2(OP_CALL, 1, line);
        emit(OP_LT, line);

        int exit_jump = emit_jump(OP_JUMP_IF_FALSE, line);
        emit(OP_POP, line);

        /* var = __arr[__i] — in an inner scope */
        begin_scope();
        emit2(OP_GET_LOCAL, (uint8_t)arr_slot, line);
        emit2(OP_GET_LOCAL, (uint8_t)i_slot,   line);
        emit(OP_GET_INDEX, line);
        add_local(node->data.for_stmt.for_var);

        /* compile body (it's a block node) */
        compile_stmt(node->data.for_stmt.for_body);

        end_scope(line);   /* pops loop var */

        /* patch continues */
        for (int i = 0; i < ctx->continue_count; i++) {
            patch_jump(ctx->continue_patches[i]);
        }

        /* __i = __i + 1 */
        emit2(OP_GET_LOCAL, (uint8_t)i_slot, line);
        emit_const(vm_val_number(1), line);
        emit(OP_ADD, line);
        emit2(OP_SET_LOCAL, (uint8_t)i_slot, line);
        emit(OP_POP, line);

        emit_loop(loop_start, line);
        patch_jump(exit_jump);
        emit(OP_POP, line);

        /* patch breaks */
        for (int i = 0; i < ctx->break_count; i++)
            patch_jump(ctx->break_patches[i]);

        current->loop_depth--;
        end_scope(line);   /* pops __arr and __i */
        break;
    }

    /* ── break ── */
    case AST_BREAK_STMT: {
        if (current->loop_depth == 0) {
            compiler_error("'break' outside loop", line);
            break;
        }
        LoopCtx *ctx = &current->loops[current->loop_depth - 1];
        /* Pop locals that belong to the loop's inner scopes */
        int pops = 0;
        for (int i = current->local_count - 1; i >= 0; i--) {
            if (current->locals[i].depth > ctx->scope_depth) pops++;
            else break;
        }
        for (int i = 0; i < pops; i++) emit(OP_POP, line);
        int jp = emit_jump(OP_JUMP, line);
        ctx->break_patches[ctx->break_count++] = jp;
        break;
    }

    /* ── continue ── */
    case AST_CONTINUE_STMT: {
        if (current->loop_depth == 0) {
            compiler_error("'continue' outside loop", line);
            break;
        }
        LoopCtx *ctx = &current->loops[current->loop_depth - 1];
        int pops = 0;
        for (int i = current->local_count - 1; i >= 0; i--) {
            if (current->locals[i].depth > ctx->scope_depth) pops++;
            else break;
        }
        for (int i = 0; i < pops; i++) emit(OP_POP, line);
        /* Jump to a placeholder; we patch it after the increment step */
        int jp = emit_jump(OP_JUMP, line);
        ctx->continue_patches[ctx->continue_count++] = jp;
        break;
    }

    /* ── Block ── */
    case AST_BLOCK:
        compile_block(node->data.statements, line);
        break;

    /* ── try: <block> catch [name]: <block> ──
       OP_TRY_BEGIN's operand is a forward jump-style offset (reusing
       emit_jump/patch_jump exactly as OP_JUMP does) pointing at the
       catch block's bytecode — the VM doesn't jump there itself on
       entry, it just records it as the unwind target. The try-block's
       own bytecode runs immediately after OP_TRY_BEGIN, normally,
       falling through. If it completes without throwing, OP_TRY_END
       retires the handler and a plain OP_JUMP skips over the catch
       block entirely. If something throws, the VM (see vm.c's
       runtime_error()/OP_THROW) jumps straight to the catch block
       itself, mid-loop, without ever executing OP_TRY_END or the
       skip-jump — both of those only run on the non-throwing path. */
    case AST_TRY_STMT: {
        int try_begin = emit_jump(OP_TRY_BEGIN, line);
        compile_stmt(node->data.try_stmt.try_block);
        emit(OP_TRY_END, line);
        int after_catch = emit_jump(OP_JUMP, line);

        patch_jump(try_begin); /* catch block starts here */
        begin_scope();
        if (node->data.try_stmt.catch_var) {
            /* Mirrors AST_LET_STMT: the caught value is already sitting
               on top of the stack (pushed by the VM's unwind) when
               execution reaches here — add_local() just claims that
               slot as the bound name, no extra emit needed. */
            add_local(node->data.try_stmt.catch_var);
        } else {
            emit(OP_POP, line); /* bare "catch:" — discard the caught value */
        }
        compile_stmt(node->data.try_stmt.catch_block);
        end_scope(line); /* pops the bound catch var, if any */

        patch_jump(after_catch);
        break;
    }

    /* ── throw expr ── */
    case AST_THROW_STMT:
        compile_expr(node->data.expr);
        emit(OP_THROW, line);
        break;

    /* ── fn name(params): body ── */
    case AST_FN_DECL: {
        const char *fname = node->data.fn_decl.fn_name;
        int arity = list_count(node->data.fn_decl.params);

        KhanFunction *fn = khanfn_new(fname, arity);

        /* compile function body into a nested compiler state */
        CompilerState fn_state;
        compiler_state_init(&fn_state, fn, current);
        current = &fn_state;

        /* scope_depth = 1 so params are locals */
        current->scope_depth = 1;

        for (AstNodeList *p = node->data.fn_decl.params; p; p = p->next)
            add_local(p->node->data.name);

        /* body is an AST_BLOCK */
        AstNode *body = node->data.fn_decl.fn_body;
        if (body->type == AST_BLOCK) {
            for (AstNodeList *s = body->data.statements; s; s = s->next)
                compile_stmt(s->node);
        } else {
            compile_stmt(body);
        }

        /* implicit nil return */
        emit(OP_NIL, line);
        emit(OP_RETURN, line);

        int fn_had_error = current->had_error;

        /* Copy the free-variable capture list this function needs (if any)
           from the compile-time scratch state into the KhanFunction itself,
           so it survives after fn_state goes out of scope. */
        if (fn_state.upvalue_count > 0) {
            fn->upvalue_count = fn_state.upvalue_count;
            fn->upvalues = malloc(sizeof(UpvalueDesc) * fn_state.upvalue_count);
            memcpy(fn->upvalues, fn_state.upvalues, sizeof(UpvalueDesc) * fn_state.upvalue_count);
        }

        current = fn_state.enclosing;
        if (fn_had_error) current->had_error = 1;

        /* Register compiled function; emit its index so VM finds it */
        khanfn_register(fn);
        int fn_idx = khanfn_registry_index();

        emit_const(value_number((double)fn_idx), line);
        emit_global_def(fname, line);
        break;
    }

    /* ── return ── */
    case AST_RETURN_STMT:
        if (node->data.expr)
            compile_expr(node->data.expr);
        else
            emit(OP_NIL, line);
        emit(OP_RETURN, line);
        break;

    /* ── import — handled at parse time; skip in VM mode ── */
    case AST_IMPORT_STMT:
        compile_import(node->data.import_path, line);
        break;

    case AST_FROM_IMPORT_STMT:
        compile_from_import(node->data.from_import.path,
                            node->data.from_import.names,
                            node->data.from_import.name_count, line);
        break;

    /* ── program root ── */
    case AST_PROGRAM:
        for (AstNodeList *s = node->data.program_stmts; s; s = s->next)
            compile_stmt(s->node);
        break;

    default:
        compiler_error("Unhandled statement node", line);
        break;
    }
}

/* ══════════════════════════════════════════════════════════════
   Public entry point
   ══════════════════════════════════════════════════════════════ */

KhanFunction *compile(AstNode *program, const char *base_path) {
    KhanFunction *script = khanfn_new("<script>", 0);

    CompilerState state;
    compiler_state_init(&state, script, NULL);

    // Set base path dir
    if (base_path) {
        char *last_slash = strrchr(base_path, '/');
        char *last_backslash = strrchr(base_path, '\\');
        char *sep = (last_slash > last_backslash) ? last_slash : last_backslash;
        if (sep) {
            int len = (int)(sep - base_path);
            state.imports->base_path = malloc(len + 1);
            memcpy(state.imports->base_path, base_path, len);
            state.imports->base_path[len] = '\0';

            // Also set as source_dir for the top-level script
            if (script->source_dir) free(script->source_dir);
            script->source_dir = strdup(state.imports->base_path);
        }
    }

    current = &state;

    /* compile all top-level statements */
    if (program->type == AST_PROGRAM) {
        for (AstNodeList *s = program->data.program_stmts; s; s = s->next)
            compile_stmt(s->node);
    } else {
        compile_stmt(program);
    }

    /* implicit nil return from script */
    emit(OP_NIL, 0);
    emit(OP_RETURN, 0);

    current = NULL;

    if (state.had_error) {
        khanfn_free(script);
        return NULL;
    }
    return script;
}