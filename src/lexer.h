#ifndef KHAN_LEXER_H
#define KHAN_LEXER_H

#include "token.h"

typedef struct {
    const char *start;
    const char *current;
    int line;
    const char *line_start; // points at the first char of the current line;
                             // used to compute column on demand rather than
                             // incrementally tracking it through every one
                             // of the several separate newline-handling
                             // sites below (safer than trying to keep a
                             // manually-incremented counter in sync in 6
                             // different places)

    // Indentation tracking
    int indent_stack[64];
    int indent_top;
    int at_line_start;
    int pending_dedents;

    // Tracks nesting depth inside (), [], {}. While > 0, newlines are
    // treated as insignificant whitespace instead of statement separators
    // — this is what lets a map/array literal (or a call's argument list)
    // span multiple physical lines.
    int bracket_depth;
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next_token(Lexer *lexer);
const char *token_type_name(TokenKind type);

#endif