#ifndef KHAN_TOKEN_H
#define KHAN_TOKEN_H

typedef enum {
    // Literals
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,

    // Keywords
    TOKEN_LET,
    TOKEN_FN,
    TOKEN_CLASS, TOKEN_SUPER,
    TOKEN_PRINT,
    TOKEN_IMPORT,
    TOKEN_FROM,
    TOKEN_IF,
    TOKEN_ELIF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_FOR,
    TOKEN_IN,
    TOKEN_RETURN,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_TRY,
    TOKEN_CATCH,
    TOKEN_THROW,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NIL,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,

    // Operators
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_EQUAL,         // =
    TOKEN_EQUAL_EQUAL,   // ==
    TOKEN_BANG_EQUAL,    // !=
    TOKEN_LESS, TOKEN_LESS_EQUAL,
    TOKEN_GREATER, TOKEN_GREATER_EQUAL,

    // Punctuation
    TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_LBRACE, TOKEN_RBRACE,
    TOKEN_COLON,
    TOKEN_COMMA,
    TOKEN_DOT,

    // Structural
    TOKEN_NEWLINE,
    TOKEN_INDENT,
    TOKEN_DEDENT,
    TOKEN_EOF,

    TOKEN_ERROR
} TokenKind;

typedef struct {
    TokenKind type;
    const char *start;   // pointer into source
    int length;
    int line;
    int column;          // 1-based column of the token's first character
} Token;

#endif