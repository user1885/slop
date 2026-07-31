#include "lexer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- pool */

#define POOL_BLOCK 4096

struct PoolBlock {
    PoolBlock *next;
    size_t used;
    size_t cap;
    char data[];
};

static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (p == NULL) {
        fprintf(stderr, "slop: out of memory\n");
        exit(1);
    }
    return p;
}

static void *pool_alloc(Lexer *lx, size_t size) {
    size = (size + 7u) & ~(size_t)7u;
    PoolBlock *b = lx->pool;
    if (b == NULL || b->cap - b->used < size) {
        size_t cap = size > POOL_BLOCK ? size : POOL_BLOCK;
        b = xmalloc(sizeof(PoolBlock) + cap);
        b->next = lx->pool;
        b->used = 0;
        b->cap = cap;
        lx->pool = b;
    }
    char *p = b->data + b->used;
    b->used += size;
    return p;
}

/* ---------------------------------------------------------------- classes */

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned hex_val(char c) {
    if (is_digit(c)) {
        return (unsigned)(c - '0');
    }
    return (unsigned)((c | 0x20) - 'a') + 10u;
}

static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_cont(char c) {
    return is_ident_start(c) || is_digit(c);
}

/* ---------------------------------------------------------------- tables */

static const char *const kind_names[TOK__COUNT] = {
#define X(kind, spelling) spelling,
    SLOP_SPECIAL_TOKENS(X)
    SLOP_KEYWORDS(X)
    SLOP_PUNCT(X)
#undef X
};

static const struct {
    const char *spelling;
    size_t len;
    TokenKind kind;
} keywords[] = {
#define X(kind, spelling) {spelling, sizeof(spelling) - 1, kind},
    SLOP_KEYWORDS(X)
#undef X
};

static const struct {
    const char *spelling;
    size_t len;
    TokenKind kind;
} puncts[] = {
#define X(kind, spelling) {spelling, sizeof(spelling) - 1, kind},
    SLOP_PUNCT(X)
#undef X
};

const char *token_kind_name(TokenKind kind) {
    if (kind < 0 || kind >= TOK__COUNT) {
        return "<invalid>";
    }
    return kind_names[kind];
}

/* ---------------------------------------------------------------- driver */

void lexer_init(Lexer *lx, const char *file, const char *src, size_t len) {
    lx->file = file != NULL ? file : "<input>";
    lx->src = src;
    lx->cur = src;
    lx->end = src + len;
    lx->line_start = src;
    lx->line = 1;
    lx->errors = 0;
    lx->pool = NULL;
}

void lexer_free(Lexer *lx) {
    PoolBlock *b = lx->pool;
    while (b != NULL) {
        PoolBlock *next = b->next;
        free(b);
        b = next;
    }
    lx->pool = NULL;
}

static void skip_trivia(Lexer *lx) {
    while (lx->cur != lx->end) {
        char c = *lx->cur;
        if (c == '\n') {
            lx->cur++;
            lx->line++;
            lx->line_start = lx->cur;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            lx->cur++;
        } else if (c == '#') {
            while (lx->cur != lx->end && *lx->cur != '\n') {
                lx->cur++;
            }
        } else {
            return;
        }
    }
}

static Token finish(Lexer *lx, Token t, TokenKind kind) {
    t.kind = kind;
    t.len = (int32_t)(lx->cur - t.text);
    return t;
}

static Token error_at(Lexer *lx, Token t, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    size_t n = strlen(buf) + 1;
    char *msg = pool_alloc(lx, n);
    memcpy(msg, buf, n);

    lx->errors++;
    t = finish(lx, t, TOK_ERROR);
    t.val.err = msg;
    return t;
}

/* --------------------------------------------------------------- literals */

/* Reads one char_lit/string_lit element at lx->cur. Returns 0 and fills
 * errbuf on failure, leaving lx->cur wherever the failure was noticed. */
static int scan_char_unit(Lexer *lx, unsigned char *out, char *errbuf, size_t errcap) {
    if (lx->cur == lx->end || *lx->cur == '\n') {
        snprintf(errbuf, errcap, "unterminated literal");
        return 0;
    }

    char c = *lx->cur++;
    if (c != '\\') {
        *out = (unsigned char)c;
        return 1;
    }

    if (lx->cur == lx->end || *lx->cur == '\n') {
        snprintf(errbuf, errcap, "unterminated escape sequence");
        return 0;
    }

    char e = *lx->cur++;
    switch (e) {
    case 'n': *out = '\n'; return 1;
    case 'r': *out = '\r'; return 1;
    case 't': *out = '\t'; return 1;
    case '0': *out = '\0'; return 1;
    case '\\': *out = '\\'; return 1;
    case '\'': *out = '\''; return 1;
    case '"': *out = '"'; return 1;
    case 'x':
        if (lx->end - lx->cur < 2 || !is_hex_digit(lx->cur[0]) || !is_hex_digit(lx->cur[1])) {
            snprintf(errbuf, errcap, "\\x needs exactly two hex digits");
            return 0;
        }
        *out = (unsigned char)(hex_val(lx->cur[0]) * 16u + hex_val(lx->cur[1]));
        lx->cur += 2;
        return 1;
    default:
        snprintf(errbuf, errcap, "unknown escape sequence '\\%c'", e);
        return 0;
    }
}

static Token lex_char(Lexer *lx, Token t) {
    char errbuf[128];
    unsigned char ch;

    lx->cur++; /* opening quote */
    if (lx->cur != lx->end && *lx->cur == '\'') {
        lx->cur++;
        return error_at(lx, t, "empty character literal");
    }
    if (!scan_char_unit(lx, &ch, errbuf, sizeof errbuf)) {
        return error_at(lx, t, "%s", errbuf);
    }
    if (lx->cur == lx->end || *lx->cur != '\'') {
        return error_at(lx, t, "unterminated character literal");
    }
    lx->cur++;

    t = finish(lx, t, TOK_CHAR);
    t.val.ival = ch;
    return t;
}

static Token lex_string(Lexer *lx, Token t) {
    char errbuf[128];

    lx->cur++; /* opening quote */

    /* A string cannot span a newline, so the rest of the line is an upper
     * bound on the decoded length — one pool allocation, no resizing. */
    const char *eol = lx->cur;
    while (eol != lx->end && *eol != '\n') {
        eol++;
    }
    char *buf = pool_alloc(lx, (size_t)(eol - lx->cur) + 1);

    size_t n = 0;
    for (;;) {
        if (lx->cur == lx->end || *lx->cur == '\n') {
            return error_at(lx, t, "unterminated string literal");
        }
        if (*lx->cur == '"') {
            lx->cur++;
            break;
        }
        unsigned char ch;
        if (!scan_char_unit(lx, &ch, errbuf, sizeof errbuf)) {
            return error_at(lx, t, "%s", errbuf);
        }
        buf[n++] = (char)ch;
    }
    buf[n] = '\0';

    t = finish(lx, t, TOK_STRING);
    t.val.str.ptr = buf;
    t.val.str.len = (int32_t)n;
    return t;
}

static Token lex_number(Lexer *lx, Token t) {
    uint64_t v = 0;
    int overflow = 0;

    if (lx->end - lx->cur >= 2 && lx->cur[0] == '0' && lx->cur[1] == 'x') {
        lx->cur += 2;
        if (lx->cur == lx->end || !is_hex_digit(*lx->cur)) {
            return error_at(lx, t, "expected hex digits after '0x'");
        }
        while (lx->cur != lx->end && is_hex_digit(*lx->cur)) {
            unsigned d = hex_val(*lx->cur++);
            if (v > (UINT64_MAX - d) / 16u) {
                overflow = 1;
            }
            v = v * 16u + d;
        }
    } else {
        while (lx->cur != lx->end && is_digit(*lx->cur)) {
            unsigned d = (unsigned)(*lx->cur++ - '0');
            if (v > (UINT64_MAX - d) / 10u) {
                overflow = 1;
            }
            v = v * 10u + d;
        }

        /* A dot only starts a fraction when a digit follows, so `1.as(f64)`
         * still lexes as int, dot, `as`. */
        if (lx->end - lx->cur >= 2 && lx->cur[0] == '.' && is_digit(lx->cur[1])) {
            lx->cur += 2;
            while (lx->cur != lx->end && is_digit(*lx->cur)) {
                lx->cur++;
            }
            if (lx->cur != lx->end && is_ident_cont(*lx->cur)) {
                lx->cur++;
                return error_at(lx, t, "unexpected character after float literal");
            }

            size_t len = (size_t)(lx->cur - t.text);
            char *copy = pool_alloc(lx, len + 1);
            memcpy(copy, t.text, len);
            copy[len] = '\0';

            t = finish(lx, t, TOK_FLOAT);
            t.val.fval = strtod(copy, NULL);
            return t;
        }
    }

    if (lx->cur != lx->end && is_ident_cont(*lx->cur)) {
        lx->cur++;
        return error_at(lx, t, "unexpected character after integer literal");
    }
    if (overflow) {
        return error_at(lx, t, "integer literal does not fit in 64 bits");
    }

    t = finish(lx, t, TOK_INT);
    t.val.ival = v;
    return t;
}

static Token lex_ident(Lexer *lx, Token t) {
    while (lx->cur != lx->end && is_ident_cont(*lx->cur)) {
        lx->cur++;
    }

    size_t len = (size_t)(lx->cur - t.text);
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (keywords[i].len == len && memcmp(t.text, keywords[i].spelling, len) == 0) {
            return finish(lx, t, keywords[i].kind);
        }
    }
    return finish(lx, t, TOK_IDENT);
}

/* ------------------------------------------------------------------- next */

Token lexer_next(Lexer *lx) {
    skip_trivia(lx);

    Token t;
    t.kind = TOK_EOF;
    t.text = lx->cur;
    t.len = 0;
    t.line = lx->line;
    t.col = (int32_t)(lx->cur - lx->line_start) + 1;
    t.val.ival = 0;

    if (lx->cur == lx->end) {
        return t;
    }

    char c = *lx->cur;
    if (is_ident_start(c)) {
        return lex_ident(lx, t);
    }
    if (is_digit(c)) {
        return lex_number(lx, t);
    }
    if (c == '\'') {
        return lex_char(lx, t);
    }
    if (c == '"') {
        return lex_string(lx, t);
    }

    size_t left = (size_t)(lx->end - lx->cur);
    for (size_t i = 0; i < sizeof puncts / sizeof puncts[0]; i++) {
        if (puncts[i].len <= left && memcmp(lx->cur, puncts[i].spelling, puncts[i].len) == 0) {
            lx->cur += puncts[i].len;
            return finish(lx, t, puncts[i].kind);
        }
    }

    lx->cur++;
    if (c >= 0x20 && c < 0x7f) {
        return error_at(lx, t, "unexpected character '%c'", c);
    }
    return error_at(lx, t, "unexpected byte 0x%02X", (unsigned char)c);
}

int32_t lexer_lex_all(Lexer *lx, Token **out_tokens, size_t *out_count) {
    size_t cap = 256;
    size_t count = 0;
    Token *tokens = xmalloc(cap * sizeof(Token));

    for (;;) {
        if (count == cap) {
            cap *= 2;
            Token *grown = realloc(tokens, cap * sizeof(Token));
            if (grown == NULL) {
                free(tokens);
                fprintf(stderr, "slop: out of memory\n");
                exit(1);
            }
            tokens = grown;
        }
        tokens[count] = lexer_next(lx);
        if (tokens[count++].kind == TOK_EOF) {
            break;
        }
    }

    *out_tokens = tokens;
    *out_count = count;
    return lx->errors;
}
