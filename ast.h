#ifndef SLOP_AST_H
#define SLOP_AST_H

#include "arena.h"
#include "lexer.h"

#include <stdint.h>
#include <stdio.h>

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Item Item;

/* The parser produces a tree, never a type: no name is resolved and no
 * constant is folded here. A child may be NULL when the parser recovered
 * from an error, so every consumer must be NULL-tolerant until the parse
 * has been confirmed error-free. */

/* Where a node came from. `file` is the path the parser was handed and must
 * outlive the arena; line and col are the lexer's, 1-based. */
typedef struct {
    const char *file;
    int32_t line;
    int32_t col;
} SrcPos;

/* A name or a string literal. Identifiers point into the source buffer, so
 * it has to outlive the AST; decoded string literals are copied into the
 * arena, because the lexer's pool dies with the Lexer. Not NUL-terminated in
 * the first case, NUL-terminated in the second. */
typedef struct {
    const char *data;
    int32_t len;
} StrView;

int strview_eq(StrView v, const char *cstr);

/* ------------------------------------------------------------------ types */

typedef enum {
    TYPE_CORE,    /* i32, u8, void, ... */
    TYPE_NAME,    /* a struct or enum name, unresolved */
    TYPE_POINTER, /* T*  */
    TYPE_ARRAY    /* T[N] */
} TypeKind;

struct Type {
    TypeKind kind;
    SrcPos pos;
    TokenKind core;  /* TYPE_CORE: one of the TOK_KW_* core types */
    StrView name;    /* TYPE_NAME */
    Type *elem;      /* TYPE_POINTER, TYPE_ARRAY */
    uint64_t length; /* TYPE_ARRAY */
};

/* ------------------------------------------------------------ expressions */

typedef enum {
    EXPR_INT,
    EXPR_FLOAT,
    EXPR_CHAR,
    EXPR_STRING,
    EXPR_BOOL,
    EXPR_NULL,
    EXPR_NAME,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CALL,
    EXPR_INDEX,
    EXPR_FIELD,
    EXPR_CAST,  /* e.as(T) */
    EXPR_SIZEOF /* sizeof(T) */
} ExprKind;

struct Expr {
    ExprKind kind;
    SrcPos pos;
    union {
        uint64_t ival; /* EXPR_INT, EXPR_CHAR */
        double fval;   /* EXPR_FLOAT */
        StrView sval;  /* EXPR_STRING (decoded), EXPR_NAME */
        int bval;      /* EXPR_BOOL */
        struct {
            TokenKind op; /* - ! ~ * & */
            Expr *operand;
        } unary;
        struct {
            TokenKind op;
            Expr *lhs;
            Expr *rhs;
        } binary;
        struct {
            Expr *callee;
            Expr **args;
            uint32_t nargs;
        } call;
        struct {
            Expr *base;
            Expr *index;
        } index;
        struct {
            Expr *base;
            StrView name;
        } field;
        struct {
            Expr *operand;
            Type *type;
        } cast;
        struct {
            Type *type;
        } size_of;
    } u;
};

/* ------------------------------------------------------------- statements */

typedef struct {
    Stmt **stmts;
    uint32_t nstmts;
    SrcPos pos;
} Block;

/* `_` is represented by is_wildcard, not by a name: the parser only treats
 * `_` specially in label position, so it stays a usable identifier. */
typedef struct {
    SrcPos pos;
    int is_wildcard;
    Expr *value; /* EXPR_INT, EXPR_CHAR or EXPR_NAME; NULL when wildcard */
} MatchLabel;

typedef struct {
    SrcPos pos;
    MatchLabel *labels;
    uint32_t nlabels;
    Stmt *body;
} MatchArm;

typedef enum {
    STMT_BLOCK,
    STMT_VAR,
    STMT_IF,
    STMT_WHILE,
    STMT_MATCH,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_EXPR,
    STMT_ASSIGN,
    STMT_EMPTY
} StmtKind;

struct Stmt {
    StmtKind kind;
    SrcPos pos;
    union {
        Block block;
        struct {
            int is_mut;
            Type *type;
            StrView name;
            Expr *init; /* NULL when absent: zero-initialized */
        } var;
        struct {
            Expr *cond;
            Block then_block;
            Stmt *else_stmt; /* STMT_BLOCK or STMT_IF, or NULL */
        } if_stmt;
        struct {
            Expr *cond;
            Block body;
        } while_stmt;
        struct {
            Expr *scrutinee;
            MatchArm *arms;
            uint32_t narms;
        } match;
        struct {
            Expr *value; /* NULL for a bare `return;` */
        } ret;
        struct {
            Expr *expr;
        } expr;
        struct {
            /* The lvalue check is sema's job -- the parser accepts any
             * expression on the left so `a + b = 5` gets a real diagnostic. */
            Expr *target;
            TokenKind op;
            Expr *value;
        } assign;
    } u;
};

/* ------------------------------------------------------------------ items */

typedef struct {
    SrcPos pos;
    int is_mut;
    Type *type;
    StrView name;
} Param;

typedef struct {
    SrcPos pos;
    StrView name;
    Param *params;
    uint32_t nparams;
    int is_variadic; /* trailing `...`; only reachable inside extern blocks */
    Type *ret;       /* never NULL: a synthesized `void` when omitted */
    Block body;
    int has_body; /* 0 for an extern declaration */
} FnDecl;

typedef struct {
    SrcPos pos;
    Type *type;
    StrView name;
} Field;

typedef struct {
    SrcPos pos;
    StrView name;
    int has_value; /* explicit `= int_lit`; numbering is sema's job */
    uint64_t value;
} EnumMember;

typedef enum { ITEM_FN, ITEM_STRUCT, ITEM_ENUM, ITEM_GLOBAL, ITEM_EXTERN } ItemKind;

struct Item {
    ItemKind kind;
    SrcPos pos;
    union {
        FnDecl fn;
        struct {
            StrView name;
            Field *fields;
            uint32_t nfields;
        } struct_decl;
        struct {
            StrView name;
            TokenKind base; /* core type; TOK_KW_I32 when `: T` is omitted */
            EnumMember *members;
            uint32_t nmembers;
        } enum_decl;
        struct {
            int is_mut;
            Type *type;
            StrView name;
            Expr *init; /* a literal only, per the v0 grammar */
        } global;
        struct {
            StrView abi; /* the decoded string literal, e.g. "c" */
            FnDecl *fns;
            uint32_t nfns;
        } extern_block;
    } u;
};

typedef struct {
    const char *file;
    Item **items;
    uint32_t nitems;
} Program;

/* -------------------------------------------------------- constructors */

Type *type_new(Arena *a, TypeKind kind, SrcPos pos);
Expr *expr_new(Arena *a, ExprKind kind, SrcPos pos);
Stmt *stmt_new(Arena *a, StmtKind kind, SrcPos pos);
Item *item_new(Arena *a, ItemKind kind, SrcPos pos);

/* -------------------------------------------------------------- printing */

/* Writes the type back in source syntax (`i32*[10]`). */
void ast_print_type(FILE *out, const Type *t);

/* Indented tree dump, one node per line. Round-tripping is not a goal; this
 * exists so the parser can be eyeballed before sema exists. */
void ast_dump(FILE *out, const Program *prog);

#endif /* SLOP_AST_H */
