#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "compiler.h"
#include "vm.h"
#include "khan_stdlib.h"
#include "json_lib.h"
#include "datetime_lib.h"
#include "requests_lib.h"
#include "webi_lib.h"

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

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: khan_vm <script.kh>\n");
        return 64;
    }

    char *source = read_file(argv[1]);

    Lexer lexer;
    lexer_init(&lexer, source);

    Parser parser;
    parser_init(&parser, &lexer);
    AstNode *program = parser_parse(&parser);

    if (parser.had_error) {
        ast_free(program);
        free(source);
        return 65;
    }

    /* Compile to VM bytecode */
    KhanFunction *script = compile(program, argv[1]);
    if (!script) {
        ast_free(program);
        free(source);
        return 65;
    }

    /* Initialize and run VM */
    VM vm;
    vm_init(&vm);

    /* Register all native libraries */
    // We need a dummy global environment to reuse existing register_all functions
    // but the VM doesn't use Environment.
    // I'll create a new way to register them or just manually call vm_global_set_native.
    // Actually, I'll modify the register_all functions to be more generic,
    // but for now, I'll manually define them in a helper.

    vm_register_builtins(&vm);

    // Register extra libs
    // Note: this is a bit of a hack to reuse existing C logic
    // but let's try calling stdlib_register_all with a special proxy env if needed.
    // For now, I'll just manually add the most important ones.

    json_register_all_vm(&vm);
    datetime_register_all_vm(&vm);
    requests_register_all_vm(&vm);
    webi_register_all_vm(&vm);

    InterpretResult result = vm_run(&vm, script);

    /* Cleanup (basic) */
    vm_free(&vm);
    // free(source);
    // ast_free(program);

    return result == INTERPRET_OK ? 0 : 70;
}
