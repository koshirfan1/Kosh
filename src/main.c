// ===========================================================================
// main.c — Khan High-Performance Entry Point
//
// Uses the Bytecode VM for execution (36% faster than Python).
// ===========================================================================

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static void enable_ansi(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
}
#else
static void enable_ansi(void) {}
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <limits.h>
#include <stdlib.h> /* realpath */
#endif

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "compiler.h"
#include "vm.h"
#include "vision_lib.h"
#include "vision_cv.h"
#include "vision_cascade.h"
#include "tensor_lib.h"
#include "khan_version.h"
#ifdef LLM_SUPPORT
#include "llm_lib.h"
#endif

/* ── Read entire file into a heap-allocated string ── */
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }
    fseek(file, 0L, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char *buffer = malloc(size + 1);
    if (!buffer) { fprintf(stderr, "Out of memory.\n"); exit(74); }
    size_t n = fread(buffer, 1, size, file);
    buffer[n] = '\0';
    fclose(file);
    return buffer;
}

static void print_usage(FILE *out) {
    fprintf(out,
        "Khan " KHAN_VERSION "\n"
        "Usage: khan <script.kh> [args...]\n"
        "       khan                    (starts an interactive REPL)\n"
        "       khan --version | -v\n"
        "       khan --help    | -h\n"
        "\n"
        "Once run, extra arguments after the script path are available\n"
        "inside the script as the global array `argv` (argv[0] is the\n"
        "first extra argument, matching argv[2] on the C command line —\n"
        "the script path itself, argv[1] in C terms, is not included).\n");
}

/* Registers every native library the script-running path also
   registers, so the REPL and `khan script.kh` behave identically as
   far as what's available to call — `import` isn't needed for any of
   these either way, matching how the script path already works.
   Kept as its own function specifically so run_repl() and main()'s
   script-running path can't silently drift apart on this list. */
static void register_all_libraries(VM *vm) {
    vm_register_builtins(vm);
    json_register_all_vm(vm);
    datetime_register_all_vm(vm);
    requests_register_all_vm(vm);
    webi_register_all_vm(vm);
    sqlite_register_all_vm(vm);
    vision_register_all_vm(vm);
    vision_cv_register_all_vm(vm);
    vision_cascade_register_all_vm(vm);
    tensor_register_all_vm(vm);
#ifdef LLM_SUPPORT
    llm_register_all_vm(vm);
#endif
}

/* Reads one logical unit of REPL input: a single line, or — if that
 * line's content (trimmed) ends with ':' — that line plus every
 * following line up to and including the next blank line, all
 * concatenated together as one multi-line block. Khan is indentation-
 * based like Python, so a `fn foo():`/`if x:`/`while x:`/`try:` header
 * needs its indented body typed on subsequent lines before it forms a
 * complete, compilable unit — a single-line-at-a-time REPL would never
 * be able to parse a function or loop definition at all. Requiring a
 * blank line to close a block is a deliberate simplification (Python's
 * own REPL instead tracks indentation depth to know when a block
 * ends) — less clever, but far less code, and blank-line-to-finish is
 * a pattern most people already know from other tools.
 *
 * Returns a malloc'd, NUL-terminated string (caller frees) containing
 * everything read, or NULL on EOF with nothing entered yet (Ctrl+D at
 * a fresh prompt). EOF encountered PARTWAY through a block returns
 * whatever was read so far, rather than NULL, so pasting a complete
 * block into a REPL that's about to receive EOF right after still
 * works. */
static char *read_repl_input(void) {
    char line[4096];
    size_t cap = 4096;
    char *buf = malloc(cap);
    buf[0] = '\0';
    size_t len = 0;
    int in_block = 0;
    int have_any = 0;

    for (;;) {
        printf(in_block ? "... " : "khan> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            if (!have_any) { free(buf); return NULL; }
            break;
        }

        size_t line_len = strlen(line);
        char trimmed[4096];
        strncpy(trimmed, line, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        size_t tlen = strlen(trimmed);
        while (tlen > 0 && (trimmed[tlen - 1] == '\n' || trimmed[tlen - 1] == '\r')) {
            trimmed[--tlen] = '\0';
        }

        if (!in_block && tlen == 0) {
            /* Blank line at a fresh prompt: nothing to do, just reprompt
               without accumulating an empty statement. */
            continue;
        }

        if (len + line_len + 1 > cap) {
            cap = (len + line_len + 1) * 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, line, line_len);
        len += line_len;
        buf[len] = '\0';
        have_any = 1;

        if (in_block) {
            if (tlen == 0) break; /* blank line closes the block */
            continue;
        }
        if (tlen > 0 && trimmed[tlen - 1] == ':') {
            in_block = 1;
            continue;
        }
        break; /* ordinary single-line input */
    }
    return buf;
}

/* Interactive mode — entered when `khan` is run with no script
 * argument, the same trigger Python's own `python` (no args) uses for
 * its REPL. One VM instance persists across every line typed, so a
 * `let x = 5` on one line is still visible as `x` on the next — the
 * whole point of a REPL over running one-off scripts. A bare
 * expression (e.g. typing `2 + 2` with no `print`) auto-displays its
 * value, matching Python's REPL convention, by rewriting the parsed
 * AST_EXPR_STMT into an AST_PRINT_STMT before compiling — nothing
 * downstream (compiler, VM) needs to know this is happening.
 *
 * A runtime error partway through a line does not end the session:
 * the VM's stack is unwound (any values left on it from the aborted
 * line are freed) and its call-frame count reset to zero before the
 * next line runs, but `vm.globals` is never touched — recoverable,
 * matching how a real interactive interpreter should behave when
 * given bad input, rather than being one-mistake-and-you're-out. */
static int run_repl(void) {
    printf("Khan " KHAN_VERSION " — interactive mode. Type 'exit' or press Ctrl+D to quit.\n");

    VM vm;
    vm_init(&vm);
    vm.base_path = strdup(".");
    strncpy(vm.current_import_dir, ".", sizeof(vm.current_import_dir) - 1);
    register_all_libraries(&vm);
    vm_global_set(&vm, "argv", value_array(NULL, 0));

    for (;;) {
        char *source = read_repl_input();
        if (!source) { printf("\n"); break; } /* Ctrl+D */

        char trimmed[64];
        strncpy(trimmed, source, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        size_t tlen = strlen(trimmed);
        while (tlen > 0 && (trimmed[tlen - 1] == '\n' || trimmed[tlen - 1] == '\r' ||
                            trimmed[tlen - 1] == ' ')) {
            trimmed[--tlen] = '\0';
        }
        if (strcmp(trimmed, "exit") == 0 || strcmp(trimmed, "exit()") == 0 ||
            strcmp(trimmed, "quit") == 0 || strcmp(trimmed, "quit()") == 0) {
            free(source);
            break;
        }
        if (tlen == 0) { free(source); continue; }

        Lexer lexer;
        lexer_init(&lexer, source);
        Parser parser;
        parser_init(&parser, &lexer, "<repl>");
        AstNode *program = parser_parse(&parser);

        if (parser.had_error) {
            ast_free(program);
            free(source);
            continue;
        }

        /* Single bare expression -> auto-print, Python-REPL style. */
        if (program->type == AST_PROGRAM && program->data.statements &&
            !program->data.statements->next &&
            program->data.statements->node->type == AST_EXPR_STMT) {
            AstNode *stmt = program->data.statements->node;
            program->data.statements->node = ast_new_print_stmt(stmt->data.expr, stmt->line);
            stmt->data.expr = NULL; /* now owned by the new print stmt; don't let this free it */
            ast_free(stmt);
        }

        KhanFunction *script = compile(program, "<repl>");
        ast_free(program);
        if (!script) { free(source); continue; }

        InterpretResult result = vm_run(&vm, script);
        if (result != INTERPRET_OK) {
            for (Value *slot = vm.stack; slot < vm.stack_top; slot++) value_free(*slot);
            vm.stack_top = vm.stack;
            vm.frame_count = 0;
        }

        free(source);
    }

    vm_free(&vm);
    return 0;
}

int main(int argc, char *argv[]) {
    enable_ansi();

    if (argc < 2) {
        return run_repl();
    }

    /* --version/-v and --help/-h are recognized wherever they'd
       otherwise be read as the script path (argv[1]) — matching how
       most CLI tools special-case these two regardless of position
       among the first argument. Anything after a real script path is
       left alone and passed through to the script as-is (a Khan
       script named "--version.kh" would still need to be run some
       other way; this is a deliberate, minor trade-off in favor of
       the common case working without ceremony). */
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("Khan " KHAN_VERSION "\n");
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(stdout);
        return 0;
    }

    char *source = read_file(argv[1]);

    /* ── Lex ── */
    Lexer lexer;
    lexer_init(&lexer, source);

    /* ── Parse ── */
    Parser parser;
    parser_init(&parser, &lexer, argv[1]);
    AstNode *program = parser_parse(&parser);

    if (parser.had_error) {
        ast_free(program);
        free(source);
        return 65;
    }

    /* ── Compile to VM bytecode ── */
    KhanFunction *script = compile(program, argv[1]);
    if (!script) {
        ast_free(program);
        free(source);
        return 65;
    }

    /* ── Initialize VM ── */
    VM vm;
    vm_init(&vm);

    /* ── Resolve base path for imports ── */
#ifdef _WIN32
    char path_buf[2048];
    GetFullPathNameA(argv[1], (DWORD)sizeof(path_buf), path_buf, NULL);
#else
    /* realpath() requires a destination buffer of at least PATH_MAX
       bytes (glibc's fortified _chk variant enforces this exactly). */
    char path_buf[PATH_MAX];
    if (!realpath(argv[1], path_buf)) {
        /* Fall back to the raw argument if realpath fails (e.g. file
           does not exist yet or path is already relative-safe). */
        strncpy(path_buf, argv[1], sizeof(path_buf) - 1);
        path_buf[sizeof(path_buf) - 1] = '\0';
    }
#endif

    char *last_slash     = strrchr(path_buf, '/');
    char *last_backslash = strrchr(path_buf, '\\');
    char *sep = (last_slash > last_backslash) ? last_slash : last_backslash;
    if (sep) {
        *sep = '\0'; // path_buf now contains the absolute dir
        vm.base_path = strdup(path_buf);
        strncpy(vm.current_import_dir, path_buf, sizeof(vm.current_import_dir) - 1);
        vm.current_import_dir[sizeof(vm.current_import_dir) - 1] = '\0';
    } else {
        vm.base_path = strdup(".");
        strcpy(vm.current_import_dir, ".");
    }

    // printf("[DEBUG] base_path: %s, current_import_dir: %s\n", vm.base_path, vm.current_import_dir);

    /* ── Register built-ins and libraries ── */
    register_all_libraries(&vm);

    /* ── Pass command line arguments as global 'argv' ── */
    Value argv_val = value_array(NULL, 0);
    for (int i = 2; i < argc; i++) {
        Obj *o = argv_val.as.obj;
        o->as.array.items = realloc(o->as.array.items, sizeof(Value) * (o->as.array.count + 1));
        o->as.array.items[o->as.array.count++] = value_string(argv[i]);
        o->as.array.capacity = o->as.array.count;
    }
    vm_global_set(&vm, "argv", argv_val);

    /* ── Execute ── */
    InterpretResult result = vm_run(&vm, script);

    vm_free(&vm);

    return result == INTERPRET_OK ? 0 : 70;
}
