#ifndef KOSH_COMPILER_H
#define KOSH_COMPILER_H

#include "ast.h"
#include "chunk.h"

/*
 * compiler.h — compiles a Kosh AST into bytecode chunks.
 *
 * The lexer, parser, and AST are completely unchanged.
 * compile() takes the program AstNode returned by parser_parse()
 * and fills a KoshFunction (the top-level script function).
 *
 * Returns: pointer to the compiled top-level KoshFunction on success,
 *          NULL on compile error.
 */
KoshFunction *compile(AstNode *program, const char *base_path);

#endif /* KOSH_COMPILER_H */