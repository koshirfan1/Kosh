#ifndef KHAN_PARSER_H
#define KHAN_PARSER_H

#include "token.h"
#include "ast.h"
#include "lexer.h"

typedef struct {
    Lexer *lexer;
    Token current;
    Token previous;
    int had_error;
    const char *filename; // for error messages; may be NULL if unknown
} Parser;

// Parse the entire source into a program AST.
AstNode *parser_parse(Parser *parser);

// Initialise the parser with a lexer. `filename` is used in error
// messages (pass NULL if unavailable — errors fall back to omitting it).
void parser_init(Parser *parser, Lexer *lexer, const char *filename);

#endif