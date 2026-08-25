#include "lexer.h"

#include "common.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void token_push(TokenVec *tv, TokKind k, const char *s, size_t line) {
    VEC_GROW(tv->v, tv->n, tv->cap, Token);
    tv->v[tv->n].kind = k;
    tv->v[tv->n].text = xstrdup(s);
    tv->v[tv->n].line = line;
    tv->n++;
}

static bool is_ident_start(int c) { return isalpha(c) || c == '_'; }
static bool is_ident_char(int c) { return isalnum(c) || c == '_' || c == '@'; }

bool is_var_token(const char *s) {
    if (!s || !*s || !is_ident_start((unsigned char)s[0])) return false;
    for (const char *p = s + 1; *p; ++p) {
        if (!is_ident_char((unsigned char)*p)) return false;
    }
    return true;
}

// Recognized type prefixes for typed prefix literals (i8:N, f32:N, etc.).
static bool has_type_prefix(const char *buf) {
    // Must start with i/u/f followed by digit or 'har', and contain ':'.
    const char *colon = strchr(buf, ':');
    if (!colon || colon == buf) return false;
    size_t plen = (size_t)(colon - buf);
    // type name lengths: 2 (i8,u8) or 3 (i16,u16,i32,u32,i64,u64,f32,f64) or 4 (char)
    if (plen == 2 || plen == 3 || plen == 4) {
        // Quick check: first char must be i, u, f, or c.
        char c0 = buf[0];
        if (c0 == 'i' || c0 == 'u' || c0 == 'f' || c0 == 'c')
            return true;
    }
    return false;
}

// Classifies bare words as INT/UINT/FLOAT/TYPED literals.
static TokKind classify_word(const char *buf, TokKind k) {
    if (k != TOK_WORD) return k;
    bool has_dot = false;
    bool all_num = true;
    size_t digits = 0;
    const char *q = buf;
    if (*q == '-' || *q == '+') ++q;
    for (; *q; ++q) {
        if (*q == '.') {
            if (has_dot) { all_num = false; break; }
            has_dot = true;
        } else if (!isdigit((unsigned char)*q)) {
            all_num = false;
            break;
        }
        ++digits;
    }
    if (all_num && digits > 0) return has_dot ? TOK_FLOAT : TOK_INT;
    if (strncmp(buf, "u:", 2) == 0 && buf[2]) return TOK_UINT;
    if (has_type_prefix(buf)) return TOK_TYPED;
    return TOK_WORD;
}

// Reads a reference token: sig followed by identifier characters.
static void lex_reference(char *buf, size_t *n, size_t buf_sz, const char **pp,
                          char sig, size_t line) {
    const char *p = *pp;
    if (!is_ident_start((unsigned char)*p)) fatal_at(line, "bad reference token '%c'", sig);
    buf[(*n)++] = sig;
    while (is_ident_char((unsigned char)*p)) {
        if (*n + 1 >= buf_sz) die_oom();
        buf[(*n)++] = *p++;
    }
    buf[*n] = '\0';
    *pp = p;
}

// Documented compact global capture syntax: :{a b}name
static void lex_global_ref(TokenVec *out, const char **pp, size_t line) {
    const char *p = *pp + 2; // skip ":{"
    token_push(out, TOK_COLON, ":", line);

    char gbuf[512];
    size_t gn = 0;
    while (*p && *p != '}') {
        if (gn + 1 >= sizeof(gbuf)) fatal_at(line, "global list too long");
        gbuf[gn++] = *p++;
    }
    if (*p != '}') fatal_at(line, "unterminated global list");
    ++p;
    gbuf[gn] = '\0';
    token_push(out, TOK_GLOBAL_REF, gbuf, line);

    char buf[256];
    size_t n = 0;
    while (*p && !isspace((unsigned char)*p) && *p != ';') {
        if (n + 1 >= sizeof(buf)) fatal_at(line, "function name too long");
        buf[n++] = *p++;
    }
    if (n == 0) fatal_at(line, "function name expected after global list");
    buf[n] = '\0';
    token_push(out, TOK_WORD, buf, line);
    *pp = p;
}

// Decode a single escape character after a backslash.
static char decode_escape(char c, size_t line) {
    switch (c) {
    case 'n':  return '\n';
    case 'r':  return '\r';
    case 't':  return '\t';
    case '0':  return '\0';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"':  return '"';
    default:
        fatal_at(line, "unknown escape sequence '\\%c'", c);
    }
    return 0;
}

void lex_source(const char *src, TokenVec *out) {
    const char *p = src;
    size_t line = 1;
    bool in_bracket = false;
    TokenVec bracket = {0};

    while (*p) {
        if (*p == '\n') { ++line; ++p; continue; }
        if (isspace((unsigned char)*p)) { ++p; continue; }
        if (*p == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') ++p;
            continue;
        }
        if (*p == '(' || *p == ')') { ++p; continue; }

        if (*p == '[') {
            if (in_bracket) fatal_at(line, "nested [] is not allowed");
            in_bracket = true;
            ++p;
            continue;
        }
        if (*p == ']') {
            if (!in_bracket) fatal_at(line, "unexpected ]");
            in_bracket = false;
            // [] expansion reverses the collected tokens in-place.
            for (size_t i = bracket.n; i-- > 0; ) {
                token_push(out, bracket.v[i].kind, bracket.v[i].text, bracket.v[i].line);
            }
            token_push(out, TOK_PARAM_END, "]", line);
            for (size_t i = 0; i < bracket.n; ++i) free(bracket.v[i].text);
            free(bracket.v);
            bracket = (TokenVec){0};
            ++p;
            continue;
        }

        TokKind k = TOK_WORD;
        char buf[256];
        size_t n = 0;

        if (*p == ':' || *p == ';') {
            if (*p == ':' && p[1] == '{') {
                lex_global_ref(out, &p, line);
                continue;
            }
            k = (*p == ':') ? TOK_COLON : TOK_SEMI;
            buf[0] = *p;
            buf[1] = '\0';
            ++p;
        } else if (*p == '"') {
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    if (n + 2 >= sizeof(buf)) fatal_at(line, "string too long");
                    buf[n++] = *p++;
                    buf[n++] = *p++;
                    continue;
                }
                if (n + 1 >= sizeof(buf)) fatal_at(line, "string too long");
                if (*p == '\n') ++line;
                buf[n++] = *p++;
            }
            if (*p != '"') fatal_at(line, "unterminated string");
            ++p;
            buf[n] = '\0';
            k = TOK_STRING;
        } else if (*p == '\'') {
            // Character literal: 'c', '\n', '\x41', etc.
            ++p;
            if (*p == '\\') {
                ++p;
                if (!*p) fatal_at(line, "unterminated escape in char literal");
                buf[0] = decode_escape(*p, line);
                ++p;
            } else {
                if (!*p || *p == '\n') fatal_at(line, "empty char literal");
                buf[0] = *p;
                ++p;
            }
            if (*p != '\'') fatal_at(line, "unterminated char literal");
            ++p;
            buf[1] = '\0';
            k = TOK_CHAR;
        } else if ((*p == '&' || *p == '*') && is_ident_start((unsigned char)p[1])) {
            // '&name' and '*name' are reference tokens. A standalone '*' is
            // deliberately NOT a reference: it is the multiplication operator.
            char sig = *p++;
            lex_reference(buf, &n, sizeof(buf), &p, sig, line);
        } else if (*p == '!' && p[1] == '@') {
            p += 2;
            buf[n++] = '!';
            buf[n++] = '@';
            while (is_ident_char((unsigned char)*p)) {
                if (n + 1 >= sizeof(buf)) die_oom();
                buf[n++] = *p++;
            }
            buf[n] = '\0';
        } else {
            while (*p && !isspace((unsigned char)*p) && !strchr("()[]", *p)) {
                if (n + 1 >= sizeof(buf)) fatal_at(line, "token too long");
                buf[n++] = *p++;
            }
            buf[n] = '\0';
            k = classify_word(buf, k);
        }

        if (in_bracket) {
            VEC_GROW(bracket.v, bracket.n, bracket.cap, Token);
            bracket.v[bracket.n].kind = k;
            bracket.v[bracket.n].text = xstrdup(buf);
            bracket.v[bracket.n].line = line;
            bracket.n++;
        } else {
            token_push(out, k, buf, line);
        }
    }
    if (in_bracket) fatal_at(line, "unterminated []");
}

void free_tokens(TokenVec *tv) {
    for (size_t i = 0; i < tv->n; ++i) free(tv->v[i].text);
    free(tv->v);
    *tv = (TokenVec){0};
}
