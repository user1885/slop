#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stop printing after this many errors; the count stays exact. */
#define MAX_REPORTED_ERRORS 20

/* Recursion limit for nested expressions and statements. Hitting it is a
 * diagnostic, not a crash -- the parser recurses on the C stack. */
#define MAX_NESTING         200

typedef struct {
    Arena *arena;
    const Token *toks;
    size_t ntoks;
    size_t pos;
    const char *file;
    int errors;
    int reported;
    int depth;
} Parser;

/* The assignment operators are scattered through the lexer's punct table
 * (it is ordered by spelling length for maximal munch), so this is a switch
 * rather than a range check. */
static int is_assign_op(TokenKind kind) {
    switch (kind) {
    case TOK_EQ:
    case TOK_PLUS_EQ:
    case TOK_MINUS_EQ:
    case TOK_STAR_EQ:
    case TOK_SLASH_EQ:
    case TOK_PERCENT_EQ:
    case TOK_AMP_EQ:
    case TOK_PIPE_EQ:
    case TOK_CARET_EQ:
    case TOK_SHL_EQ:
    case TOK_SHR_EQ:
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------- scratch vectors */

/* Node lists are collected here and copied into the arena once the final
 * length is known, so the arena never holds a half-grown buffer. */
typedef struct {
    char *data;
    uint32_t len;
    uint32_t cap;
    size_t elem;
} Vec;

static void vec_init(Vec *v, size_t elem) {
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem = elem;
}

static void *vec_push(Vec *v) {
    void *slot;
    if (v->len == v->cap) {
        uint32_t cap = v->cap != 0 ? v->cap * 2 : 8;
        char *data = realloc(v->data, (size_t)cap * v->elem);
        if (data == NULL) {
            fprintf(stderr, "slop: out of memory\n");
            exit(1);
        }
        v->data = data;
        v->cap = cap;
    }
    slot = v->data + (size_t)v->len * v->elem;
    memset(slot, 0, v->elem);
    v->len++;
    return slot;
}

static void vec_push_ptr(Vec *v, void *item) {
    *(void **)vec_push(v) = item;
}

static void *vec_take(Arena *a, Vec *v, uint32_t *out_len) {
    void *out = NULL;
    *out_len = v->len;
    if (v->len != 0) {
        out = arena_dup(a, v->data, (size_t)v->len * v->elem);
    }
    free(v->data);
    vec_init(v, v->elem);
    return out;
}

static void vec_drop(Vec *v) {
    free(v->data);
    vec_init(v, v->elem);
}

/* ---------------------------------------------------------- token access */

static void error_at(Parser *p, SrcPos pos, const char *fmt, ...);

static TokenKind cur(Parser *p) {
    return p->toks[p->pos].kind;
}

static SrcPos here(Parser *p) {
    const Token *t = &p->toks[p->pos];
    SrcPos pos;
    pos.file = p->file;
    pos.line = t->line;
    pos.col = t->col;
    return pos;
}

static StrView tok_view(const Token *t) {
    StrView v;
    v.data = t->text;
    v.len = t->len;
    return v;
}

/* The lexer's decoded strings live in a pool that lexer_free() reclaims, so
 * a string literal is copied into the arena and the AST outlives the Lexer.
 * Identifiers keep pointing into the source buffer. */
static StrView intern_string(Parser *p, const Token *t) {
    StrView v;
    v.data = arena_strndup(p->arena, t->val.str.ptr, (size_t)t->val.str.len);
    v.len = t->val.str.len;
    return v;
}

static int at(Parser *p, TokenKind kind) {
    return cur(p) == kind;
}

/* TOK_ERROR tokens carry the lexer's own message. Reporting them here, as
 * the parser walks past them, is what keeps lexical and syntactic errors
 * interleaved in source order; the lexer itself never prints. */
static void skip_error_tokens(Parser *p) {
    while (p->toks[p->pos].kind == TOK_ERROR && p->pos + 1 < p->ntoks) {
        error_at(p, here(p), "%s", p->toks[p->pos].val.err);
        p->pos++;
    }
}

/* Never walks past the terminating TOK_EOF. */
static const Token *advance(Parser *p) {
    const Token *t = &p->toks[p->pos];
    if (p->pos + 1 < p->ntoks) {
        p->pos++;
        skip_error_tokens(p);
    }
    return t;
}

static int accept(Parser *p, TokenKind kind) {
    if (at(p, kind)) {
        advance(p);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ diagnostics */

static void error_at(Parser *p, SrcPos pos, const char *fmt, ...) {
    va_list ap;

    p->errors++;
    if (p->reported >= MAX_REPORTED_ERRORS) {
        if (p->reported == MAX_REPORTED_ERRORS) {
            p->reported++;
            fprintf(stderr, "%s: too many errors; further diagnostics suppressed\n", p->file);
        }
        return;
    }
    p->reported++;

    fprintf(stderr, "%s:%d:%d: error: ", pos.file != NULL ? pos.file : p->file, pos.line, pos.col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* token_kind_name() gives keywords and punctuation their spelling and the
 * rest a prose name, so only the former wants quotes around it. */
static void describe_kind(TokenKind kind, char *buf, size_t n) {
    if (kind > TOK_STRING && kind < TOK__COUNT) {
        snprintf(buf, n, "'%s'", token_kind_name(kind));
    } else {
        snprintf(buf, n, "%s", token_kind_name(kind));
    }
}

/* Describes the current token for a "found ..." clause. Identifiers and
 * numbers get their spelling attached, which makes the message point at
 * something the user can actually see in the source. */
static void describe_current(Parser *p, char *buf, size_t n) {
    const Token *t = &p->toks[p->pos];

    if (t->kind == TOK_IDENT) {
        snprintf(buf, n, "identifier '%.*s'", (int)t->len, t->text);
    } else if (t->kind == TOK_INT || t->kind == TOK_FLOAT) {
        snprintf(buf, n, "'%.*s'", (int)t->len, t->text);
    } else {
        describe_kind(t->kind, buf, n);
    }
}

static void error_unexpected(Parser *p, const char *expected) {
    char found[80];
    describe_current(p, found, sizeof(found));
    error_at(p, here(p), "expected %s, found %s", expected, found);
}

static int expect(Parser *p, TokenKind kind) {
    char want[48];

    if (at(p, kind)) {
        advance(p);
        return 1;
    }
    describe_kind(kind, want, sizeof(want));
    error_unexpected(p, want);
    return 0;
}

static int expect_ident(Parser *p, StrView *out) {
    if (at(p, TOK_IDENT)) {
        *out = tok_view(advance(p));
        return 1;
    }
    error_unexpected(p, "identifier");
    return 0;
}

/* --------------------------------------------------------------- recovery */

/* Panic-mode resync. Each of these leaves the parser either just past a
 * boundary or right on a token that can start the next construct, so the
 * enclosing loop can pick up where it left off. */

static void sync_stmt(Parser *p) {
    while (!at(p, TOK_EOF)) {
        if (accept(p, TOK_SEMI)) {
            return;
        }
        switch (cur(p)) {
        case TOK_RBRACE:
        case TOK_LBRACE:
        case TOK_KW_LET:
        case TOK_KW_IF:
        case TOK_KW_WHILE:
        case TOK_KW_MATCH:
        case TOK_KW_RETURN:
        case TOK_KW_BREAK:
        case TOK_KW_CONTINUE:
        case TOK_KW_FN:
        case TOK_KW_STRUCT:
        case TOK_KW_ENUM:
        case TOK_KW_EXTERN:
            return;
        default:
            advance(p);
        }
    }
}

/* For struct fields, enum members and extern declarations: anything up to
 * the next separator. */
static void sync_member(Parser *p) {
    while (!at(p, TOK_EOF)) {
        if (accept(p, TOK_SEMI) || accept(p, TOK_COMMA)) {
            return;
        }
        if (at(p, TOK_RBRACE)) {
            return;
        }
        advance(p);
    }
}

static void sync_item(Parser *p) {
    while (!at(p, TOK_EOF)) {
        switch (cur(p)) {
        case TOK_KW_FN:
        case TOK_KW_STRUCT:
        case TOK_KW_ENUM:
        case TOK_KW_LET:
        case TOK_KW_EXTERN:
            return;
        default:
            advance(p);
        }
    }
}

/* ------------------------------------------------------------------ types */

/* Type       = PrimaryType TypeSuffix*
 * TypeSuffix = "*" | "[" int_lit "]"
 *
 * Suffixes apply left to right, so each one wraps what came before:
 * `i32*[10]` is an array of ten pointers, `i32[10]*` a pointer to an
 * array. */
static Type *parse_type(Parser *p) {
    SrcPos pos = here(p);
    Type *t;

    if (token_is_core_type(cur(p))) {
        t = type_new(p->arena, TYPE_CORE, pos);
        t->core = advance(p)->kind;
    } else if (at(p, TOK_IDENT)) {
        t = type_new(p->arena, TYPE_NAME, pos);
        t->name = tok_view(advance(p));
    } else {
        error_unexpected(p, "a type");
        return NULL;
    }

    for (;;) {
        if (accept(p, TOK_STAR)) {
            Type *ptr = type_new(p->arena, TYPE_POINTER, pos);
            ptr->elem = t;
            t = ptr;
        } else if (accept(p, TOK_LBRACKET)) {
            Type *arr = type_new(p->arena, TYPE_ARRAY, pos);
            arr->elem = t;
            if (at(p, TOK_INT)) {
                arr->length = advance(p)->val.ival;
            } else {
                error_unexpected(p, "an integer literal as the array length");
            }
            expect(p, TOK_RBRACKET);
            t = arr;
        } else {
            return t;
        }
    }
}

/* ------------------------------------------------------------ expressions */

static Expr *parse_expr(Parser *p);

static Expr *make_binary(Parser *p, TokenKind op, SrcPos pos, Expr *lhs, Expr *rhs) {
    Expr *e = expr_new(p->arena, EXPR_BINARY, pos);
    e->u.binary.op = op;
    e->u.binary.lhs = lhs;
    e->u.binary.rhs = rhs;
    return e;
}

/* Primary = literal | identifier | "(" Expr ")" */
static Expr *parse_primary(Parser *p) {
    SrcPos pos = here(p);
    Expr *e;

    switch (cur(p)) {
    case TOK_INT:
        e = expr_new(p->arena, EXPR_INT, pos);
        e->u.ival = advance(p)->val.ival;
        return e;
    case TOK_FLOAT:
        e = expr_new(p->arena, EXPR_FLOAT, pos);
        e->u.fval = advance(p)->val.fval;
        return e;
    case TOK_CHAR:
        e = expr_new(p->arena, EXPR_CHAR, pos);
        e->u.ival = advance(p)->val.ival;
        return e;
    case TOK_STRING:
        e = expr_new(p->arena, EXPR_STRING, pos);
        e->u.sval = intern_string(p, advance(p));
        return e;
    case TOK_KW_TRUE:
    case TOK_KW_FALSE:
        e = expr_new(p->arena, EXPR_BOOL, pos);
        e->u.bval = advance(p)->kind == TOK_KW_TRUE;
        return e;
    case TOK_KW_NULL:
        advance(p);
        return expr_new(p->arena, EXPR_NULL, pos);
    case TOK_IDENT:
        e = expr_new(p->arena, EXPR_NAME, pos);
        e->u.sval = tok_view(advance(p));
        return e;
    case TOK_LPAREN:
        advance(p);
        e = parse_expr(p);
        expect(p, TOK_RPAREN);
        return e;
    default:
        error_unexpected(p, "an expression");
        return NULL;
    }
}

/* PostfixOp = "(" ArgList? ")" | "[" Expr "]" | "." identifier
 *           | "." "as" "(" Type ")"
 *
 * The cast is a postfix operator with its target type in its own
 * parentheses, so `n.as(BinNode*).lhs` chains without any wrapping. */
static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);

    if (e == NULL) {
        return NULL; /* recover at the statement level, not here */
    }
    for (;;) {
        SrcPos pos = here(p);

        if (accept(p, TOK_LPAREN)) {
            Expr *call = expr_new(p->arena, EXPR_CALL, pos);
            Vec args;
            vec_init(&args, sizeof(Expr *));
            if (!at(p, TOK_RPAREN)) {
                for (;;) {
                    vec_push_ptr(&args, parse_expr(p));
                    if (!accept(p, TOK_COMMA)) {
                        break;
                    }
                }
            }
            expect(p, TOK_RPAREN);
            call->u.call.callee = e;
            call->u.call.args = vec_take(p->arena, &args, &call->u.call.nargs);
            e = call;
        } else if (accept(p, TOK_LBRACKET)) {
            Expr *idx = expr_new(p->arena, EXPR_INDEX, pos);
            idx->u.index.base = e;
            idx->u.index.index = parse_expr(p);
            expect(p, TOK_RBRACKET);
            e = idx;
        } else if (accept(p, TOK_DOT)) {
            if (accept(p, TOK_KW_AS)) {
                Expr *cast = expr_new(p->arena, EXPR_CAST, pos);
                cast->u.cast.operand = e;
                if (expect(p, TOK_LPAREN)) {
                    cast->u.cast.type = parse_type(p);
                    expect(p, TOK_RPAREN);
                }
                e = cast;
            } else {
                Expr *field = expr_new(p->arena, EXPR_FIELD, pos);
                field->u.field.base = e;
                expect_ident(p, &field->u.field.name);
                e = field;
            }
        } else {
            return e;
        }
    }
}

/* Unary = ( "-" | "!" | "~" | "*" | "&" ) Unary
 *       | "sizeof" "(" Type ")"
 *       | Postfix */
static Expr *parse_unary(Parser *p) {
    SrcPos pos = here(p);
    Expr *e;

    switch (cur(p)) {
    case TOK_MINUS:
    case TOK_BANG:
    case TOK_TILDE:
    case TOK_STAR:
    case TOK_AMP:
        e = expr_new(p->arena, EXPR_UNARY, pos);
        e->u.unary.op = advance(p)->kind;
        e->u.unary.operand = parse_unary(p);
        return e;
    case TOK_KW_SIZEOF:
        advance(p);
        e = expr_new(p->arena, EXPR_SIZEOF, pos);
        if (expect(p, TOK_LPAREN)) {
            e->u.size_of.type = parse_type(p);
            expect(p, TOK_RPAREN);
        }
        return e;
    default:
        return parse_postfix(p);
    }
}

/* The precedence ladder, tightest first. Bitwise operators bind tighter
 * than comparisons here, unlike in C: `a & b == c` means `(a & b) == c`. */

static Expr *parse_mul(Parser *p) {
    Expr *lhs = parse_unary(p);
    while (at(p, TOK_STAR) || at(p, TOK_SLASH) || at(p, TOK_PERCENT)) {
        SrcPos pos = here(p);
        TokenKind op = advance(p)->kind;
        lhs = make_binary(p, op, pos, lhs, parse_unary(p));
    }
    return lhs;
}

static Expr *parse_add(Parser *p) {
    Expr *lhs = parse_mul(p);
    while (at(p, TOK_PLUS) || at(p, TOK_MINUS)) {
        SrcPos pos = here(p);
        TokenKind op = advance(p)->kind;
        lhs = make_binary(p, op, pos, lhs, parse_mul(p));
    }
    return lhs;
}

static Expr *parse_shift(Parser *p) {
    Expr *lhs = parse_add(p);
    while (at(p, TOK_SHL) || at(p, TOK_SHR)) {
        SrcPos pos = here(p);
        TokenKind op = advance(p)->kind;
        lhs = make_binary(p, op, pos, lhs, parse_add(p));
    }
    return lhs;
}

static Expr *parse_bitand(Parser *p) {
    Expr *lhs = parse_shift(p);
    while (at(p, TOK_AMP)) {
        SrcPos pos = here(p);
        advance(p);
        lhs = make_binary(p, TOK_AMP, pos, lhs, parse_shift(p));
    }
    return lhs;
}

static Expr *parse_bitxor(Parser *p) {
    Expr *lhs = parse_bitand(p);
    while (at(p, TOK_CARET)) {
        SrcPos pos = here(p);
        advance(p);
        lhs = make_binary(p, TOK_CARET, pos, lhs, parse_bitand(p));
    }
    return lhs;
}

static Expr *parse_bitor(Parser *p) {
    Expr *lhs = parse_bitxor(p);
    while (at(p, TOK_PIPE)) {
        SrcPos pos = here(p);
        advance(p);
        lhs = make_binary(p, TOK_PIPE, pos, lhs, parse_bitxor(p));
    }
    return lhs;
}

static Expr *parse_relational(Parser *p) {
    Expr *lhs = parse_bitor(p);
    while (at(p, TOK_LT) || at(p, TOK_LE) || at(p, TOK_GT) || at(p, TOK_GE)) {
        SrcPos pos = here(p);
        TokenKind op = advance(p)->kind;
        lhs = make_binary(p, op, pos, lhs, parse_bitor(p));
    }
    return lhs;
}

static Expr *parse_equality(Parser *p) {
    Expr *lhs = parse_relational(p);
    while (at(p, TOK_EQ_EQ) || at(p, TOK_BANG_EQ)) {
        SrcPos pos = here(p);
        TokenKind op = advance(p)->kind;
        lhs = make_binary(p, op, pos, lhs, parse_relational(p));
    }
    return lhs;
}

static Expr *parse_logical_and(Parser *p) {
    Expr *lhs = parse_equality(p);
    while (at(p, TOK_AMP_AMP)) {
        SrcPos pos = here(p);
        advance(p);
        lhs = make_binary(p, TOK_AMP_AMP, pos, lhs, parse_equality(p));
    }
    return lhs;
}

static Expr *parse_logical_or(Parser *p) {
    Expr *lhs = parse_logical_and(p);
    while (at(p, TOK_PIPE_PIPE)) {
        SrcPos pos = here(p);
        advance(p);
        lhs = make_binary(p, TOK_PIPE_PIPE, pos, lhs, parse_logical_and(p));
    }
    return lhs;
}

static Expr *parse_expr(Parser *p) {
    Expr *e;

    if (p->depth >= MAX_NESTING) {
        error_at(p, here(p), "expression nests too deeply");
        return NULL;
    }
    p->depth++;
    e = parse_logical_or(p);
    p->depth--;
    return e;
}

/* Global initializers and enum values are literals, not expressions: v0 has
 * no constant-expression evaluator, so there is nothing to fold. */
static Expr *parse_literal(Parser *p) {
    switch (cur(p)) {
    case TOK_INT:
    case TOK_FLOAT:
    case TOK_CHAR:
    case TOK_STRING:
    case TOK_KW_TRUE:
    case TOK_KW_FALSE:
    case TOK_KW_NULL:
        return parse_primary(p);
    default:
        error_unexpected(p, "a literal (v0 has no constant expressions)");
        return NULL;
    }
}

/* ------------------------------------------------------------- statements */

static Stmt *parse_stmt(Parser *p);

static Block parse_block(Parser *p) {
    Block b;
    Vec stmts;

    memset(&b, 0, sizeof(b));
    b.pos = here(p);
    if (!expect(p, TOK_LBRACE)) {
        return b;
    }

    vec_init(&stmts, sizeof(Stmt *));
    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
        size_t before = p->pos;
        Stmt *s = parse_stmt(p);
        if (s != NULL) {
            vec_push_ptr(&stmts, s);
        } else {
            sync_stmt(p);
        }
        if (p->pos == before) {
            advance(p); /* recovery must always make progress */
        }
    }
    expect(p, TOK_RBRACE);

    b.stmts = vec_take(p->arena, &stmts, &b.nstmts);
    return b;
}

static Stmt *parse_block_stmt(Parser *p) {
    Stmt *s = stmt_new(p->arena, STMT_BLOCK, here(p));
    s->u.block = parse_block(p);
    return s;
}

/* VarDecl = "let" "mut"? Type identifier ( "=" Expr )? ";" */
static Stmt *parse_var_decl(Parser *p) {
    Stmt *s = stmt_new(p->arena, STMT_VAR, here(p));

    advance(p); /* let */
    s->u.var.is_mut = accept(p, TOK_KW_MUT);
    s->u.var.type = parse_type(p);
    if (s->u.var.type == NULL) {
        return NULL;
    }
    if (!expect_ident(p, &s->u.var.name)) {
        return NULL;
    }
    if (accept(p, TOK_EQ)) {
        s->u.var.init = parse_expr(p);
    }
    expect(p, TOK_SEMI);
    return s;
}

/* IfStmt = "if" Expr Block ( "else" ( IfStmt | Block ) )?
 *
 * The condition is not parenthesized, which costs nothing: v0 has no struct
 * literals, so a `{` after the condition can only start the block. */
static Stmt *parse_if(Parser *p) {
    Stmt *s = stmt_new(p->arena, STMT_IF, here(p));

    advance(p); /* if */
    s->u.if_stmt.cond = parse_expr(p);
    s->u.if_stmt.then_block = parse_block(p);

    if (accept(p, TOK_KW_ELSE)) {
        if (at(p, TOK_KW_IF)) {
            s->u.if_stmt.else_stmt = parse_if(p);
        } else if (at(p, TOK_LBRACE)) {
            s->u.if_stmt.else_stmt = parse_block_stmt(p);
        } else {
            error_unexpected(p, "'{' or 'if' after 'else'");
        }
    }
    return s;
}

static Stmt *parse_while(Parser *p) {
    Stmt *s = stmt_new(p->arena, STMT_WHILE, here(p));

    advance(p); /* while */
    s->u.while_stmt.cond = parse_expr(p);
    s->u.while_stmt.body = parse_block(p);
    return s;
}

/* MatchArm   = MatchLabel ( "," MatchLabel )* "=>" Statement
 * MatchLabel = int_lit | char_lit | identifier | "_"
 *
 * `_` is not a keyword: it lexes as an ordinary identifier and only means
 * "wildcard" in label position, so it stays usable as a variable name. */
static int parse_match_arm(Parser *p, MatchArm *out) {
    Vec labels;

    out->pos = here(p);
    vec_init(&labels, sizeof(MatchLabel));

    for (;;) {
        MatchLabel label;
        memset(&label, 0, sizeof(label));
        label.pos = here(p);

        if (at(p, TOK_IDENT)) {
            StrView name = tok_view(advance(p));
            if (strview_eq(name, "_")) {
                label.is_wildcard = 1;
            } else {
                label.value = expr_new(p->arena, EXPR_NAME, label.pos);
                label.value->u.sval = name;
            }
        } else if (at(p, TOK_INT) || at(p, TOK_CHAR)) {
            ExprKind kind = at(p, TOK_INT) ? EXPR_INT : EXPR_CHAR;
            label.value = expr_new(p->arena, kind, label.pos);
            label.value->u.ival = advance(p)->val.ival;
        } else {
            error_unexpected(p, "a match label (integer, character, name or '_')");
            vec_drop(&labels);
            return 0;
        }

        *(MatchLabel *)vec_push(&labels) = label;
        if (!accept(p, TOK_COMMA)) {
            break;
        }
    }

    if (!expect(p, TOK_FAT_ARROW)) {
        vec_drop(&labels);
        return 0;
    }
    out->labels = vec_take(p->arena, &labels, &out->nlabels);
    out->body = parse_stmt(p);
    return out->body != NULL;
}

static Stmt *parse_match(Parser *p) {
    Stmt *s = stmt_new(p->arena, STMT_MATCH, here(p));
    Vec arms;

    advance(p); /* match */
    s->u.match.scrutinee = parse_expr(p);
    if (!expect(p, TOK_LBRACE)) {
        return s;
    }

    vec_init(&arms, sizeof(MatchArm));
    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
        size_t before = p->pos;
        MatchArm arm;
        memset(&arm, 0, sizeof(arm));
        if (parse_match_arm(p, &arm)) {
            *(MatchArm *)vec_push(&arms) = arm;
        } else {
            sync_stmt(p);
        }
        if (p->pos == before) {
            advance(p);
        }
    }
    expect(p, TOK_RBRACE);

    s->u.match.arms = vec_take(p->arena, &arms, &s->u.match.narms);
    return s;
}

static Stmt *parse_return(Parser *p) {
    Stmt *s = stmt_new(p->arena, STMT_RETURN, here(p));

    advance(p); /* return */
    if (!at(p, TOK_SEMI)) {
        s->u.ret.value = parse_expr(p);
    }
    expect(p, TOK_SEMI);
    return s;
}

/* ExprStmt = Expr ( AssignOp Expr )? ";"
 *
 * One pass, no backtracking: parse the expression, then look at the next
 * token. Whether the left side is assignable is sema's problem, so
 * `a + b = 5` gets a real diagnostic instead of "unexpected token". */
static Stmt *parse_expr_stmt(Parser *p) {
    SrcPos pos = here(p);
    Expr *e = parse_expr(p);
    Stmt *s;

    if (e == NULL) {
        return NULL;
    }
    if (is_assign_op(cur(p))) {
        s = stmt_new(p->arena, STMT_ASSIGN, here(p));
        s->u.assign.target = e;
        s->u.assign.op = advance(p)->kind;
        s->u.assign.value = parse_expr(p);
    } else {
        s = stmt_new(p->arena, STMT_EXPR, pos);
        s->u.expr.expr = e;
    }
    expect(p, TOK_SEMI);
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    Stmt *s;

    if (p->depth >= MAX_NESTING) {
        error_at(p, here(p), "statement nests too deeply");
        return NULL;
    }
    p->depth++;

    switch (cur(p)) {
    case TOK_LBRACE:
        s = parse_block_stmt(p);
        break;
    case TOK_KW_LET:
        s = parse_var_decl(p);
        break;
    case TOK_KW_IF:
        s = parse_if(p);
        break;
    case TOK_KW_WHILE:
        s = parse_while(p);
        break;
    case TOK_KW_MATCH:
        s = parse_match(p);
        break;
    case TOK_KW_RETURN:
        s = parse_return(p);
        break;
    case TOK_KW_BREAK:
        s = stmt_new(p->arena, STMT_BREAK, here(p));
        advance(p);
        expect(p, TOK_SEMI);
        break;
    case TOK_KW_CONTINUE:
        s = stmt_new(p->arena, STMT_CONTINUE, here(p));
        advance(p);
        expect(p, TOK_SEMI);
        break;
    case TOK_SEMI:
        s = stmt_new(p->arena, STMT_EMPTY, here(p));
        advance(p);
        break;
    default:
        s = parse_expr_stmt(p);
        break;
    }

    p->depth--;
    return s;
}

/* ------------------------------------------------------------------ items */

/* FnDecl       = "fn" identifier "(" ParamList? ")" ( "->" Type )? Block
 * ExternFnDecl = "fn" identifier "(" ExternParams? ")" ( "->" Type )? ";"
 * Param        = "mut"? Type identifier
 *
 * A missing `-> Type` becomes a synthesized `void`, so later passes never
 * have to special-case a NULL return type. */
static int parse_fn_decl(Parser *p, FnDecl *out, int is_extern) {
    Vec params;

    out->pos = here(p);
    if (!expect(p, TOK_KW_FN)) {
        return 0;
    }
    if (!expect_ident(p, &out->name)) {
        return 0;
    }
    if (!expect(p, TOK_LPAREN)) {
        return 0;
    }

    vec_init(&params, sizeof(Param));
    if (!at(p, TOK_RPAREN)) {
        for (;;) {
            Param prm;

            if (at(p, TOK_ELLIPSIS)) {
                SrcPos pos = here(p);
                advance(p);
                if (is_extern) {
                    out->is_variadic = 1;
                } else {
                    error_at(p, pos, "'...' is only allowed in an extern declaration");
                }
                break;
            }

            memset(&prm, 0, sizeof(prm));
            prm.pos = here(p);
            prm.is_mut = accept(p, TOK_KW_MUT);
            prm.type = parse_type(p);
            if (prm.type == NULL || !expect_ident(p, &prm.name)) {
                vec_drop(&params);
                return 0;
            }
            *(Param *)vec_push(&params) = prm;

            if (!accept(p, TOK_COMMA)) {
                break;
            }
        }
    }
    if (!expect(p, TOK_RPAREN)) {
        vec_drop(&params);
        return 0;
    }
    out->params = vec_take(p->arena, &params, &out->nparams);

    if (accept(p, TOK_ARROW)) {
        out->ret = parse_type(p);
        if (out->ret == NULL) {
            return 0;
        }
    } else {
        out->ret = type_new(p->arena, TYPE_CORE, out->pos);
        out->ret->core = TOK_KW_VOID;
    }

    if (is_extern) {
        out->has_body = 0;
        return expect(p, TOK_SEMI);
    }
    out->body = parse_block(p);
    out->has_body = 1;
    return 1;
}

/* StructDecl = "struct" identifier "{" Field* "}"
 * Field      = Type identifier ";" */
static Item *parse_struct(Parser *p) {
    Item *it = item_new(p->arena, ITEM_STRUCT, here(p));
    Vec fields;

    advance(p); /* struct */
    if (!expect_ident(p, &it->u.struct_decl.name)) {
        return NULL;
    }
    if (!expect(p, TOK_LBRACE)) {
        return NULL;
    }

    vec_init(&fields, sizeof(Field));
    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
        size_t before = p->pos;
        Field f;

        memset(&f, 0, sizeof(f));
        f.pos = here(p);
        f.type = parse_type(p);
        if (f.type != NULL && expect_ident(p, &f.name) && expect(p, TOK_SEMI)) {
            *(Field *)vec_push(&fields) = f;
        } else {
            sync_member(p);
        }
        if (p->pos == before) {
            advance(p);
        }
    }
    expect(p, TOK_RBRACE);

    it->u.struct_decl.fields = vec_take(p->arena, &fields, &it->u.struct_decl.nfields);
    return it;
}

/* EnumDecl = "enum" identifier ( ":" core_type )? "{" EnumBody? "}"
 * EnumItem = identifier ( "=" int_lit )?
 *
 * Numbering (implicit members continuing from the last explicit one) is
 * sema's job; the parser only records whether a value was written. */
static Item *parse_enum(Parser *p) {
    Item *it = item_new(p->arena, ITEM_ENUM, here(p));
    Vec members;

    advance(p); /* enum */
    it->u.enum_decl.base = TOK_KW_I32;
    if (!expect_ident(p, &it->u.enum_decl.name)) {
        return NULL;
    }
    if (accept(p, TOK_COLON)) {
        if (token_is_core_type(cur(p))) {
            it->u.enum_decl.base = advance(p)->kind;
        } else {
            error_unexpected(p, "a core type as the enum's underlying type");
        }
    }
    if (!expect(p, TOK_LBRACE)) {
        return NULL;
    }

    vec_init(&members, sizeof(EnumMember));
    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
        size_t before = p->pos;
        EnumMember m;

        memset(&m, 0, sizeof(m));
        m.pos = here(p);
        if (!expect_ident(p, &m.name)) {
            sync_member(p);
        } else {
            if (accept(p, TOK_EQ)) {
                if (at(p, TOK_INT)) {
                    m.has_value = 1;
                    m.value = advance(p)->val.ival;
                } else {
                    error_unexpected(p, "a non-negative integer literal");
                }
            }
            *(EnumMember *)vec_push(&members) = m;
            if (!accept(p, TOK_COMMA)) {
                break; /* the trailing comma is optional */
            }
        }
        if (p->pos == before) {
            advance(p);
        }
    }
    expect(p, TOK_RBRACE);

    it->u.enum_decl.members = vec_take(p->arena, &members, &it->u.enum_decl.nmembers);
    return it;
}

/* GlobalDecl = "let" "mut"? Type identifier ( "=" literal )? ";" */
static Item *parse_global(Parser *p) {
    Item *it = item_new(p->arena, ITEM_GLOBAL, here(p));

    advance(p); /* let */
    it->u.global.is_mut = accept(p, TOK_KW_MUT);
    it->u.global.type = parse_type(p);
    if (it->u.global.type == NULL) {
        return NULL;
    }
    if (!expect_ident(p, &it->u.global.name)) {
        return NULL;
    }
    if (accept(p, TOK_EQ)) {
        it->u.global.init = parse_literal(p);
        if (it->u.global.init == NULL) {
            return NULL;
        }
    }
    expect(p, TOK_SEMI);
    return it;
}

/* ExternBlock = "extern" string_lit "{" ExternFnDecl* "}" */
static Item *parse_extern(Parser *p) {
    Item *it = item_new(p->arena, ITEM_EXTERN, here(p));
    Vec fns;

    advance(p); /* extern */
    if (!at(p, TOK_STRING)) {
        error_unexpected(p, "an ABI string after 'extern'");
        return NULL;
    }
    it->u.extern_block.abi = intern_string(p, advance(p));
    if (!expect(p, TOK_LBRACE)) {
        return NULL;
    }

    vec_init(&fns, sizeof(FnDecl));
    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF)) {
        size_t before = p->pos;
        FnDecl fn;

        memset(&fn, 0, sizeof(fn));
        if (parse_fn_decl(p, &fn, 1)) {
            *(FnDecl *)vec_push(&fns) = fn;
        } else {
            sync_member(p);
        }
        if (p->pos == before) {
            advance(p);
        }
    }
    expect(p, TOK_RBRACE);

    it->u.extern_block.fns = vec_take(p->arena, &fns, &it->u.extern_block.nfns);
    return it;
}

/* Every kind of item starts with its own keyword, so the top level is
 * single-token dispatch. */
static Item *parse_item(Parser *p) {
    switch (cur(p)) {
    case TOK_KW_FN: {
        Item *it = item_new(p->arena, ITEM_FN, here(p));
        if (!parse_fn_decl(p, &it->u.fn, 0)) {
            return NULL;
        }
        return it;
    }
    case TOK_KW_STRUCT:
        return parse_struct(p);
    case TOK_KW_ENUM:
        return parse_enum(p);
    case TOK_KW_LET:
        return parse_global(p);
    case TOK_KW_EXTERN:
        return parse_extern(p);
    default:
        error_unexpected(p, "an item ('fn', 'struct', 'enum', 'let' or 'extern')");
        return NULL;
    }
}

/* ---------------------------------------------------------------- driver */

Program *parse_program(Arena *arena, const Token *tokens, size_t count, const char *file,
                       int *out_errors) {
    Parser p;
    Program *prog = arena_alloc(arena, sizeof(Program));
    Vec items;

    prog->file = file;
    if (tokens == NULL || count == 0) {
        if (out_errors != NULL) {
            *out_errors = 0;
        }
        return prog;
    }

    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.toks = tokens;
    p.ntoks = count;
    p.file = file;
    skip_error_tokens(&p);

    vec_init(&items, sizeof(Item *));
    while (!at(&p, TOK_EOF)) {
        size_t before = p.pos;
        Item *it = parse_item(&p);
        if (it != NULL) {
            vec_push_ptr(&items, it);
        } else {
            sync_item(&p);
        }
        if (p.pos == before) {
            advance(&p);
        }
    }
    prog->items = vec_take(arena, &items, &prog->nitems);

    if (out_errors != NULL) {
        *out_errors = p.errors;
    }
    return prog;
}
