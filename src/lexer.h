// Tokenizer: converts MAD source into a flat token stream.
#ifndef MAD_LEXER_H
#define MAD_LEXER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TOK_WORD,
    TOK_INT,       // plain decimal integer → i64
    TOK_UINT,      // u:N → u64
    TOK_FLOAT,     // decimal float → f64
    TOK_STRING,    // "..." string literal
    TOK_CHAR,      // 'c' character literal
    TOK_TYPED,     // i8:N, u16:42, f32:1.5, char:65 etc.
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
