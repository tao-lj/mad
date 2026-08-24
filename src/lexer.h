// Tokenizer: converts MAD source into a flat token stream.
#ifndef MAD_LEXER_H
#define MAD_LEXER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TOK_WORD,
    TOK_INT,
    TOK_UINT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_COLON,
    TOK_SEMI,
    TOK_GLOBAL_REF, // :{a b} captured globals, encoded on function headers
    TOK_PARAM_END   // marks the end of a reversed [] parameter group
} TokKind;

typedef struct {
    TokKind kind;
    char *text;
    size_t line;
} Token;

typedef struct { Token *v; size_t n, cap; } TokenVec;

bool is_var_token(const char *s);

// Lexes source text. [] groups are expanded by reversing their tokens;
// parentheses are discarded; nested [] is rejected.
void lex_source(const char *src, TokenVec *out);

void free_tokens(TokenVec *tv);

#endif
