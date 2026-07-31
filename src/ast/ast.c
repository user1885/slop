#include "ast/ast.h"

Type *type_new(Arena *a, TypeKind kind, SrcPos pos) {
    Type *t = arena_alloc(a, sizeof(Type));
    t->kind = kind;
    t->pos = pos;
    return t;
}

Expr *expr_new(Arena *a, ExprKind kind, SrcPos pos) {
    Expr *e = arena_alloc(a, sizeof(Expr));
    e->kind = kind;
    e->pos = pos;
    return e;
}

Stmt *stmt_new(Arena *a, StmtKind kind, SrcPos pos) {
    Stmt *s = arena_alloc(a, sizeof(Stmt));
    s->kind = kind;
    s->pos = pos;
    return s;
}

Item *item_new(Arena *a, ItemKind kind, SrcPos pos) {
    Item *it = arena_alloc(a, sizeof(Item));
    it->kind = kind;
    it->pos = pos;
    return it;
}
