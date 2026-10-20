#ifndef KHAN_COMPILER_H
#define KHAN_COMPILER_H

#include "ast.h"
#include "chunk.h"

/*
 * compiler.h — compiles a Khan AST into bytecode chunks.
 *
 * The lexer, parser, and AST are completely unchanged.
 * compile() takes the program AstNode returned by parser_parse()
 * and fills a KhanFunction (the top-level script function).
 *
 * Returns: pointer to the compiled top-level KhanFunction on success,
 *          NULL on compile error.
 */
KhanFunction *compile(AstNode *program, const char *base_path);

#endif /* KHAN_COMPILER_H */