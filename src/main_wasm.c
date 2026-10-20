/*
 * main_wasm.c — browser/WASM entry point for the Khan playground.
 *
 * Adapts the exact same lexer -> parser -> compiler -> VM pipeline
 * main.c uses for the real CLI, but:
 *   - takes Khan source as an in-memory string (no file to read — there's
 *     no real filesystem in a bare browser playground)
 *   - registers only the native libraries that make sense with no real
 *     filesystem, no real network socket access, and no libtesseract
 *     compiled to WASM: stdlib, json, datetime, the (mock) sqlite bridge,
 *     and vision's pixel-math functions. webi/requests (real sockets)
 *     and ocr (needs libtesseract) are deliberately left out here — this
 *     is a reduced build for trying the LANGUAGE, not a full khan.exe.
 *
 * print()/stdout flow through Emscripten's normal stdout handling, which
 * the surrounding HTML hooks via Module.print.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "compiler.h"
#include "vm.h"
#include "json_lib.h"
#include "datetime_lib.h"
#include "sqlite_lib.h"
#include "vision_lib.h"
#include "vision_cv.h"
#include "vision_cascade.h"

EMSCRIPTEN_KEEPALIVE
int run_khan_source(const char *source) {
    Lexer lexer;
    lexer_init(&lexer, source);

    Parser parser;
    parser_init(&parser, &lexer, "playground.kh");
    AstNode *program = parser_parse(&parser);

    if (parser.had_error) {
        ast_free(program);
        return 65;
    }

    KhanFunction *script = compile(program, "playground.kh");
    if (!script) {
        ast_free(program);
        return 65;
    }

    VM vm;
    vm_init(&vm);
    vm.base_path = strdup(".");
    strcpy(vm.current_import_dir, ".");

    vm_register_builtins(&vm);
    json_register_all_vm(&vm);
    datetime_register_all_vm(&vm);
    sqlite_register_all_vm(&vm);
    vision_register_all_vm(&vm);
    vision_cv_register_all_vm(&vm);
    vision_cascade_register_all_vm(&vm);

    InterpretResult result = vm_run(&vm, script);

    vm_free(&vm);
    fflush(stdout);

    return result == INTERPRET_OK ? 0 : 70;
}
