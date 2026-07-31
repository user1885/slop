/* Lexer for slop v0 — see GRAMMAR.md section 1. */
#ifndef SLOP_LEXER_H
#define SLOP_LEXER_H

#include <stddef.h>
#include <stdint.h>

/* Token kinds are built from three lists so that the enum, the spelling
 * tables and the printable names cannot drift apart. */

#define SLOP_SPECIAL_TOKENS(X)                                                                     \
    X(TOK_EOF, "end of file")                                                                      \
    X(TOK_ERROR, "error")                                                                          \
    X(TOK_IDENT, "identifier")                                                                     \
    X(TOK_INT, "int literal")                                                                      \
    X(TOK_FLOAT, "float literal")                                                                  \
    X(TOK_CHAR, "char literal")                                                                    \
    X(TOK_STRING, "string literal")

/* Keywords, then core types last — token_is_core_type() relies on that split. */
#define SLOP_KEYWORDS(X)                                                                           \
    X(TOK_KW_LET, "let")                                                                           \
    X(TOK_KW_FN, "fn")                                                                             \
    X(TOK_KW_IF, "if")                                                                             \
    X(TOK_KW_ELSE, "else")                                                                         \
    X(TOK_KW_WHILE, "while")                                                                       \
    X(TOK_KW_BREAK, "break")                                                                       \
    X(TOK_KW_CONTINUE, "continue")                                                                 \
    X(TOK_KW_RETURN, "return")                                                                     \
    X(TOK_KW_STRUCT, "struct")                                                                     \
    X(TOK_KW_ENUM, "enum")                                                                         \
    X(TOK_KW_EXTERN, "extern")                                                                     \
    X(TOK_KW_MUT, "mut")                                                                           \
    X(TOK_KW_AS, "as")                                                                             \
    X(TOK_KW_SIZEOF, "sizeof")                                                                     \
    X(TOK_KW_MATCH, "match")                                                                       \
    X(TOK_KW_TRUE, "true")                                                                         \
    X(TOK_KW_FALSE, "false")                                                                       \
    X(TOK_KW_NULL, "null")                                                                         \
    X(TOK_KW_I8, "i8")                                                                             \
    X(TOK_KW_U8, "u8")                                                                             \
    X(TOK_KW_I32, "i32")                                                                           \
    X(TOK_KW_U32, "u32")                                                                           \
    X(TOK_KW_I64, "i64")                                                                           \
    X(TOK_KW_U64, "u64")                                                                           \
    X(TOK_KW_F32, "f32")                                                                           \
    X(TOK_KW_F64, "f64")                                                                           \
    X(TOK_KW_B8, "b8")                                                                             \
    X(TOK_KW_B32, "b32")                                                                           \
    X(TOK_KW_B64, "b64")                                                                           \
    X(TOK_KW_VOID, "void")

/* Longest spelling first: the scanner probes this table in order, which is
 * exactly the maximal-munch order the grammar asks for (`<<=` before `<<`
 * before `<=` before `<`, `->` before `-`, `==`/`=>` before `=`). */
#define SLOP_PUNCT(X)                                                                              \
    X(TOK_SHL_EQ, "<<=")                                                                           \
    X(TOK_SHR_EQ, ">>=")                                                                           \
    X(TOK_ELLIPSIS, "...")                                                                         \
    X(TOK_AMP_AMP, "&&")                                                                           \
    X(TOK_PIPE_PIPE, "||")                                                                         \
    X(TOK_SHL, "<<")                                                                               \
    X(TOK_SHR, ">>")                                                                               \
    X(TOK_LE, "<=")                                                                                \
    X(TOK_GE, ">=")                                                                                \
    X(TOK_EQ_EQ, "==")                                                                             \
    X(TOK_BANG_EQ, "!=")                                                                           \
    X(TOK_PLUS_EQ, "+=")                                                                           \
    X(TOK_MINUS_EQ, "-=")                                                                          \
    X(TOK_STAR_EQ, "*=")                                                                           \
    X(TOK_SLASH_EQ, "/=")                                                                          \
    X(TOK_PERCENT_EQ, "%=")                                                                        \
    X(TOK_AMP_EQ, "&=")                                                                            \
    X(TOK_PIPE_EQ, "|=")                                                                           \
    X(TOK_CARET_EQ, "^=")                                                                          \
    X(TOK_ARROW, "->")                                                                             \
    X(TOK_FAT_ARROW, "=>")                                                                         \
    X(TOK_PLUS, "+")                                                                               \
    X(TOK_MINUS, "-")                                                                              \
    X(TOK_STAR, "*")                                                                               \
    X(TOK_SLASH, "/")                                                                              \
    X(TOK_PERCENT, "%")                                                                            \
    X(TOK_BANG, "!")                                                                               \
    X(TOK_AMP, "&")                                                                                \
    X(TOK_PIPE, "|")                                                                               \
    X(TOK_CARET, "^")                                                                              \
    X(TOK_TILDE, "~")                                                                              \
    X(TOK_LT, "<")                                                                                 \
    X(TOK_GT, ">")                                                                                 \
    X(TOK_EQ, "=")                                                                                 \
    X(TOK_LPAREN, "(")                                                                             \
    X(TOK_RPAREN, ")")                                                                             \
    X(TOK_LBRACE, "{")                                                                             \
    X(TOK_RBRACE, "}")                                                                             \
    X(TOK_LBRACKET, "[")                                                                           \
    X(TOK_RBRACKET, "]")                                                                           \
    X(TOK_DOT, ".")                                                                                \
    X(TOK_COMMA, ",")                                                                              \
    X(TOK_SEMI, ";")                                                                               \
    X(TOK_COLON, ":")

typedef enum {
#define X(kind, spelling) kind,
    SLOP_SPECIAL_TOKENS(X)
    SLOP_KEYWORDS(X)
    SLOP_PUNCT(X)
#undef X
    TOK__COUNT
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *text; /* into the source buffer; NOT NUL-terminated */
    int32_t len;
    int32_t line; /* 1-based */
    int32_t col;  /* 1-based, counted in bytes */
    union {
        uint64_t ival; /* TOK_INT, TOK_CHAR */
        double fval;   /* TOK_FLOAT */
        struct {       /* TOK_STRING: escapes decoded, NUL-terminated */
            const char *ptr;
            int32_t len;
        } str;
        const char *err; /* TOK_ERROR */
    } val;
} Token;

typedef struct PoolBlock PoolBlock;

typedef struct {
    const char *file;
    const char *src;
    const char *cur;
    const char *end;
    const char *line_start;
    int32_t line;
    int32_t errors;
    PoolBlock *pool; /* owns decoded strings and error messages */
} Lexer;

/* `src` is borrowed and must outlive the lexer. */
void lexer_init(Lexer *lx, const char *file, const char *src, size_t len);

/* Frees the pool, which invalidates every TOK_STRING value and TOK_ERROR
 * message the lexer handed out. Token `text` pointers stay valid — they
 * point into `src`. */
void lexer_free(Lexer *lx);

/* Returns TOK_EOF forever once the source is exhausted. On a lexical error
 * the token is TOK_ERROR, the scanner has already skipped past the offending
 * text, and lx->errors is bumped. */
Token lexer_next(Lexer *lx);

/* Lexes everything into one malloc'd array, always terminated by TOK_EOF.
 * The caller frees *out_tokens; the lexer still owns the string pool.
 * Returns the number of errors seen. */
int32_t lexer_lex_all(Lexer *lx, Token **out_tokens, size_t *out_count);

const char *token_kind_name(TokenKind kind);

static inline int token_is_keyword(TokenKind kind) {
    return kind >= TOK_KW_LET && kind <= TOK_KW_VOID;
}

static inline int token_is_core_type(TokenKind kind) {
    return kind >= TOK_KW_I8 && kind <= TOK_KW_VOID;
}

#endif /* SLOP_LEXER_H */
