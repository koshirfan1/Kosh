#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "lexer.h"

void lexer_init(Lexer *lexer, const char *source) {
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->line_start = source;
    lexer->indent_stack[0] = 0;
    lexer->indent_top = 0;
    lexer->at_line_start = 1;
    lexer->pending_dedents = 0;
    lexer->bracket_depth = 0;
}

static int is_at_end(Lexer *lexer) {
    return *lexer->current == '\0';
}

static char advance(Lexer *lexer) {
    lexer->current++;
    return lexer->current[-1];
}

static char peek(Lexer *lexer) {
    return *lexer->current;
}

static char peek_next(Lexer *lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static int match(Lexer *lexer, char expected) {
    if (is_at_end(lexer)) return 0;
    if (*lexer->current != expected) return 0;
    lexer->current++;
    return 1;
}

static Token make_token(Lexer *lexer, TokenKind type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    token.column = (int)(lexer->start - lexer->line_start) + 1;
    return token;
}

static Token error_token(Lexer *lexer, const char *message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer->line;
    token.column = (int)(lexer->start - lexer->line_start) + 1;
    return token;
}

static void skip_inline_whitespace(Lexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lexer);
        } else if (c == '#') {
            while (peek(lexer) != '\n' && !is_at_end(lexer)) advance(lexer);
        } else if (lexer->bracket_depth > 0 && c == '\n') {
            // Inside (), [], or {} — a newline is just whitespace, not a
            // statement separator. This is what lets a map/array literal
            // (or a call's argument list) span multiple physical lines.
            advance(lexer);
            lexer->line++;
            lexer->line_start = lexer->current;
        } else {
            break;
        }
    }
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static TokenKind keyword_type(const char *start, int length) {
    struct { const char *word; TokenKind type; } keywords[] = {
        {"let", TOKEN_LET}, {"fn", TOKEN_FN}, {"class", TOKEN_CLASS}, {"super", TOKEN_SUPER}, {"print", TOKEN_PRINT},
        {"import", TOKEN_IMPORT}, {"from", TOKEN_FROM}, {"if", TOKEN_IF}, {"elif", TOKEN_ELIF},
        {"else", TOKEN_ELSE}, {"while", TOKEN_WHILE}, {"for", TOKEN_FOR},
        {"in", TOKEN_IN}, {"return", TOKEN_RETURN},
        {"break", TOKEN_BREAK}, {"continue", TOKEN_CONTINUE},
        {"try", TOKEN_TRY}, {"catch", TOKEN_CATCH}, {"throw", TOKEN_THROW},
        {"true", TOKEN_TRUE}, {"false", TOKEN_FALSE}, {"nil", TOKEN_NIL},
        {"and", TOKEN_AND}, {"or", TOKEN_OR}, {"not", TOKEN_NOT},
    };
    int n = sizeof(keywords) / sizeof(keywords[0]);
    for (int i = 0; i < n; i++) {
        size_t klen = strlen(keywords[i].word);
        if ((int)klen == length && memcmp(keywords[i].word, start, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOKEN_IDENTIFIER;
}

static Token identifier(Lexer *lexer) {
    while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) advance(lexer);
    int length = (int)(lexer->current - lexer->start);
    TokenKind type = keyword_type(lexer->start, length);
    return make_token(lexer, type);
}

static Token number(Lexer *lexer) {
    while (is_digit(peek(lexer))) advance(lexer);
    if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
        advance(lexer);
        while (is_digit(peek(lexer))) advance(lexer);
    }
    return make_token(lexer, TOKEN_NUMBER);
}

static Token string_literal(Lexer *lexer) {
    while (peek(lexer) != '"' && !is_at_end(lexer)) {
        if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
            // Skip the escaped character too, so \" doesn't end the string
            // and \\ doesn't leave a dangling unescaped backslash.
            advance(lexer);
            int was_newline = (peek(lexer) == '\n');
            if (was_newline) lexer->line++;
            advance(lexer);
            if (was_newline) lexer->line_start = lexer->current;
            continue;
        }
        int was_newline = (peek(lexer) == '\n');
        if (was_newline) lexer->line++;
        advance(lexer);
        if (was_newline) lexer->line_start = lexer->current;
    }
    if (is_at_end(lexer)) return error_token(lexer, "Unterminated string.");
    advance(lexer);
    return make_token(lexer, TOKEN_STRING);
}

// Measures indentation for exactly one logical (non-blank, non-comment-only)
// line, then updates the indent stack. Skips blank/comment-only lines first.
// Returns 1 if a structural token (INDENT/DEDENT) was produced via *out.
static int measure_indentation(Lexer *lexer, Token *out) {
    for (;;) {
        int spaces = 0;
        while (peek(lexer) == ' ') { spaces++; advance(lexer); }

        if (peek(lexer) == '#') {
            while (peek(lexer) != '\n' && peek(lexer) != '\r' && !is_at_end(lexer)) advance(lexer);
        }

        // Treat \r\n (Windows) and a lone \r the same as \n: a blank or
        // comment-only line, not a real line at whatever indent level the
        // leading spaces happened to add up to. Without this, a blank line
        // with trailing spaces before \r\n (common from Windows editors)
        // gets misread as a real code line and corrupts the indent stack
        // for the rest of the file.
        if (peek(lexer) == '\r') {
            advance(lexer);
            if (peek(lexer) == '\n') advance(lexer);
            lexer->line++;
            lexer->line_start = lexer->current;
            continue;
        }
        if (peek(lexer) == '\n') {
            advance(lexer);
            lexer->line++;
            lexer->line_start = lexer->current;
            continue; // blank or comment-only line; keep scanning
        }

        // Reached either EOF or real content. Reset start so structural
        // tokens don't carry stray newline/space text.
        lexer->start = lexer->current;

        int top = lexer->indent_stack[lexer->indent_top];

        if (spaces > top) {
            lexer->indent_top++;
            lexer->indent_stack[lexer->indent_top] = spaces;
            *out = make_token(lexer, TOKEN_INDENT);
            return 1;
        }

        if (spaces < top) {
            int pops = 0;
            while (lexer->indent_top > 0 && spaces < lexer->indent_stack[lexer->indent_top]) {
                lexer->indent_top--;
                pops++;
            }
            lexer->pending_dedents = pops - 1;
            *out = make_token(lexer, TOKEN_DEDENT);
            return 1;
        }

        return 0; // spaces == top: no structural token needed
    }
}

Token lexer_next_token(Lexer *lexer) {
    if (lexer->bracket_depth == 0 && lexer->pending_dedents > 0) {
        lexer->pending_dedents--;
        lexer->start = lexer->current;
        return make_token(lexer, TOKEN_DEDENT);
    }

    if (lexer->bracket_depth == 0 && lexer->at_line_start) {
        Token structural;
        if (measure_indentation(lexer, &structural)) {
            lexer->at_line_start = 0;
            return structural;
        }
        lexer->at_line_start = 0;
    }

    skip_inline_whitespace(lexer);
    lexer->start = lexer->current;

    if (is_at_end(lexer)) {
        // Unwind any remaining indentation levels by queuing DEDENTs
        if (lexer->indent_top > 0 && lexer->pending_dedents == 0) {
            lexer->pending_dedents = lexer->indent_top;
            lexer->indent_top = 0;
        }
        if (lexer->pending_dedents > 0) {
            lexer->pending_dedents--;
            lexer->start = lexer->current;
            return make_token(lexer, TOKEN_DEDENT);
        }
        return make_token(lexer, TOKEN_EOF);
    }

    char c = advance(lexer);

    if (c == '\n') {
        Token tok = make_token(lexer, TOKEN_NEWLINE);
        lexer->line++;
        lexer->line_start = lexer->current;
        lexer->at_line_start = 1;
        return tok;
    }

    if (is_alpha(c)) return identifier(lexer);
    if (is_digit(c)) return number(lexer);

    switch (c) {
        case '"': return string_literal(lexer);
        case '(': lexer->bracket_depth++; return make_token(lexer, TOKEN_LPAREN);
        case ')':
            if (lexer->bracket_depth > 0) lexer->bracket_depth--;
            return make_token(lexer, TOKEN_RPAREN);
        case '[': lexer->bracket_depth++; return make_token(lexer, TOKEN_LBRACKET);
        case ']':
            if (lexer->bracket_depth > 0) lexer->bracket_depth--;
            return make_token(lexer, TOKEN_RBRACKET);
        case '{': lexer->bracket_depth++; return make_token(lexer, TOKEN_LBRACE);
        case '}':
            if (lexer->bracket_depth > 0) lexer->bracket_depth--;
            return make_token(lexer, TOKEN_RBRACE);
        case ':': return make_token(lexer, TOKEN_COLON);
        case ',': return make_token(lexer, TOKEN_COMMA);
        case '.': return make_token(lexer, TOKEN_DOT);
        case '+': return make_token(lexer, TOKEN_PLUS);
        case '-': return make_token(lexer, TOKEN_MINUS);
        case '*': return make_token(lexer, TOKEN_STAR);
        case '/': return make_token(lexer, TOKEN_SLASH);
        case '%': return make_token(lexer, TOKEN_PERCENT);
        case '=':
            return make_token(lexer, match(lexer, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '!':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_BANG_EQUAL);
            return error_token(lexer, "Unexpected '!'.");
        case '<':
            return make_token(lexer, match(lexer, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>':
            return make_token(lexer, match(lexer, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
    }

    return error_token(lexer, "Unexpected character.");
}

const char *token_type_name(TokenKind type) {
    switch (type) {
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_STRING: return "STRING";
        case TOKEN_LET: return "LET";
        case TOKEN_FN: return "FN";
        case TOKEN_CLASS: return "CLASS";
        case TOKEN_SUPER: return "SUPER";
        case TOKEN_PRINT: return "PRINT";
        case TOKEN_IMPORT: return "IMPORT";
        case TOKEN_FROM: return "FROM";
        case TOKEN_IF: return "IF";
        case TOKEN_ELSE: return "ELSE";
        case TOKEN_WHILE: return "WHILE";
        case TOKEN_FOR: return "FOR";
        case TOKEN_IN: return "IN";
        case TOKEN_RETURN:   return "RETURN";
        case TOKEN_BREAK:    return "BREAK";
        case TOKEN_CONTINUE: return "CONTINUE";
        case TOKEN_TRY:      return "TRY";
        case TOKEN_CATCH:    return "CATCH";
        case TOKEN_THROW:    return "THROW";
        case TOKEN_ELIF:     return "ELIF";
        case TOKEN_TRUE: return "TRUE";
        case TOKEN_FALSE: return "FALSE";
        case TOKEN_NIL: return "NIL";
        case TOKEN_AND: return "AND";
        case TOKEN_OR: return "OR";
        case TOKEN_NOT: return "NOT";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_STAR: return "STAR";
        case TOKEN_SLASH: return "SLASH";
        case TOKEN_PERCENT: return "PERCENT";
        case TOKEN_EQUAL: return "EQUAL";
        case TOKEN_EQUAL_EQUAL: return "EQUAL_EQUAL";
        case TOKEN_BANG_EQUAL: return "BANG_EQUAL";
        case TOKEN_LESS: return "LESS";
        case TOKEN_LESS_EQUAL: return "LESS_EQUAL";
        case TOKEN_GREATER: return "GREATER";
        case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
        case TOKEN_LPAREN: return "LPAREN";
        case TOKEN_RPAREN: return "RPAREN";
        case TOKEN_LBRACKET: return "LBRACKET";
        case TOKEN_RBRACKET: return "RBRACKET";
        case TOKEN_LBRACE: return "LBRACE";
        case TOKEN_RBRACE: return "RBRACE";
        case TOKEN_COLON: return "COLON";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_DOT: return "DOT";
        case TOKEN_NEWLINE: return "NEWLINE";
        case TOKEN_INDENT: return "INDENT";
        case TOKEN_DEDENT: return "DEDENT";
        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
    }
    return "UNKNOWN";
}