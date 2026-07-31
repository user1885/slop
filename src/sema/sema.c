#include "sema/sema.h"

#include "sema/scope.h"
#include "sema/types.h"
#include "support/diag.h"

#include <stdio.h>
#include <string.h>

/* Spelling a StrView into a printf. */
#define SV(v) (int)(v).len, (v).data

typedef struct {
    Arena *arena;
    Diag diag;
    TypeTable types;
    Globals globals;
    Scopes scopes;
    Sym *cur_fn;    /* the function whose body is being checked */
    int loop_depth; /* for break/continue */
} Sema;

static Ty *check_expr(Sema *s, Expr **slot, Ty *expected);
static void check_stmt(Sema *s, Stmt *st);
static void check_block(Sema *s, Block *b);
static void layout_struct(Sema *s, Ty *t);
static Ty *resolve_type(Sema *s, Type *t);

static Ty *poison(Sema *s) {
    return ty_core(&s->types, TY_POISON);
}

static Ty *core(Sema *s, TyKind kind) {
    return ty_core(&s->types, kind);
}

static Sym *sym_new(Sema *s, SymKind kind, StrView name, SrcPos pos) {
    Sym *sym = arena_alloc(s->arena, sizeof(Sym));
    sym->kind = kind;
    sym->name = name;
    sym->pos = pos;
    return sym;
}

static uint32_t round_up(uint32_t n, uint32_t align) {
    return (n + align - 1u) / align * align;
}

/* ================================================================ pass 2 ==
 * Global name collection: the top level of every file, nothing else. */

static void collect_struct(Sema *s, Item *it) {
    StrView name = it->u.struct_decl.name;
    Ty *existing = globals_find_type(&s->globals, name);
    Ty *t;

    if (existing != NULL) {
        diag_error(&s->diag, it->pos, "duplicate declaration of type '%.*s'", SV(name));
        diag_note(&s->diag, existing->pos, "first declared here");
        return;
    }
    t = ty_struct_new(&s->types, name, it->pos);
    t->decl = it;
    globals_add_type(&s->globals, name, t);
}

static void collect_enum(Sema *s, Item *it) {
    StrView name = it->u.enum_decl.name;
    Ty *existing = globals_find_type(&s->globals, name);
    Ty *base = ty_from_token(&s->types, it->u.enum_decl.base);
    EnumSym *members = NULL;
    uint64_t next = 0;
    uint32_t n = it->u.enum_decl.nmembers;
    uint32_t i;
    Ty *t;

    if (base == NULL || !ty_is_integer(base)) {
        diag_error(&s->diag, it->pos, "enum '%.*s' needs an integer underlying type", SV(name));
        base = core(s, TY_I32);
    }
    if (existing != NULL) {
        diag_error(&s->diag, it->pos, "duplicate declaration of type '%.*s'", SV(name));
        diag_note(&s->diag, existing->pos, "first declared here");
        return;
    }

    t = ty_enum_new(&s->types, name, base, it->pos);
    globals_add_type(&s->globals, name, t);

    if (n != 0) {
        members = arena_alloc(s->arena, sizeof(EnumSym) * n);
    }
    for (i = 0; i < n; i++) {
        EnumMember *m = &it->u.enum_decl.members[i];
        Sym *sym = globals_find_value(&s->globals, m->name);
        uint64_t value = m->has_value ? m->value : next;
        char buf[64];

        if (!ty_int_fits(base, value)) {
            diag_error(&s->diag, m->pos, "enum value %llu does not fit in %s",
                       (unsigned long long)value, ty_str(base, buf, sizeof(buf)));
        }
        next = value + 1;

        members[i].name = m->name;
        members[i].value = value;
        members[i].pos = m->pos;

        if (sym != NULL) {
            diag_error(&s->diag, m->pos, "duplicate declaration of '%.*s'", SV(m->name));
            diag_note(&s->diag, sym->pos, "first declared here");
            continue;
        }
        sym = sym_new(s, SYM_ENUM_MEMBER, m->name, m->pos);
        sym->type = t;
        sym->value = value;
        globals_add_value(&s->globals, m->name, sym);
    }
    t->members = members;
    t->nmembers = n;
}

static void collect_fn(Sema *s, FnDecl *fn, int is_extern) {
    Sym *existing = globals_find_value(&s->globals, fn->name);
    Sym *sym;

    if (existing != NULL) {
        diag_error(&s->diag, fn->pos, "duplicate declaration of '%.*s'", SV(fn->name));
        diag_note(&s->diag, existing->pos, "first declared here");
        return;
    }
    sym = sym_new(s, SYM_FN, fn->name, fn->pos);
    sym->decl = fn;
    sym->is_variadic = fn->is_variadic;
    sym->is_extern = is_extern;
    globals_add_value(&s->globals, fn->name, sym);
}

static void collect_global(Sema *s, Item *it) {
    StrView name = it->u.global.name;
    Sym *existing = globals_find_value(&s->globals, name);
    Sym *sym;

    if (existing != NULL) {
        diag_error(&s->diag, it->pos, "duplicate declaration of '%.*s'", SV(name));
        diag_note(&s->diag, existing->pos, "first declared here");
        return;
    }
    sym = sym_new(s, SYM_GLOBAL, name, it->pos);
    sym->is_mut = it->u.global.is_mut;
    sym->item = it;
    globals_add_value(&s->globals, name, sym);
}

static void collect_program(Sema *s, Program *prog) {
    uint32_t i, j;

    for (i = 0; i < prog->nitems; i++) {
        Item *it = prog->items[i];
        if (it == NULL) {
            continue;
        }
        switch (it->kind) {
        case ITEM_STRUCT:
            collect_struct(s, it);
            break;
        case ITEM_ENUM:
            collect_enum(s, it);
            break;
        case ITEM_FN:
            collect_fn(s, &it->u.fn, 0);
            break;
        case ITEM_GLOBAL:
            collect_global(s, it);
            break;
        case ITEM_EXTERN:
            if (!strview_eq(it->u.extern_block.abi, "c")) {
                diag_error(&s->diag, it->pos, "unknown ABI \"%.*s\"; only \"c\" is implemented",
                           SV(it->u.extern_block.abi));
            }
            for (j = 0; j < it->u.extern_block.nfns; j++) {
                collect_fn(s, &it->u.extern_block.fns[j], 1);
            }
            break;
        }
    }
}

/* ================================================================ pass 3 ==
 * Type resolution and layout. */

static Ty *resolve_type(Sema *s, Type *t) {
    Ty *elem;
    Ty *r;

    if (t == NULL) {
        return poison(s);
    }
    switch (t->kind) {
    case TYPE_CORE:
        r = ty_from_token(&s->types, t->core);
        return r != NULL ? r : poison(s);

    case TYPE_NAME:
        r = globals_find_type(&s->globals, t->name);
        if (r == NULL) {
            diag_error(&s->diag, t->pos, "unknown type '%.*s'", SV(t->name));
            return poison(s);
        }
        return r;

    case TYPE_POINTER:
        /* Deliberately does not lay out the pointee: recursion through a
         * pointer is exactly what must keep working. */
        return ty_ptr(&s->types, resolve_type(s, t->elem));

    case TYPE_ARRAY:
        elem = resolve_type(s, t->elem);
        if (elem->kind == TY_STRUCT) {
            layout_struct(s, elem);
        }
        if (t->length == 0) {
            diag_error(&s->diag, t->pos, "array length must be greater than 0");
            return poison(s);
        }
        if (!ty_is_sized(elem)) {
            char buf[128];
            diag_error(&s->diag, t->pos, "array element type %s has no size",
                       ty_str(elem, buf, sizeof(buf)));
            return poison(s);
        }
        if (elem->size != 0 && t->length > 0xFFFFFFFFull / elem->size) {
            diag_error(&s->diag, t->pos, "array is too large");
            return poison(s);
        }
        return ty_array(&s->types, elem, t->length);
    }
    return poison(s);
}

static void layout_struct(Sema *s, Ty *t) {
    Item *it = t->decl;
    FieldSym *fields = NULL;
    uint32_t offset = 0;
    uint32_t align = 1;
    uint32_t n;
    uint32_t i, j;

    if (t->state == LAYOUT_DONE) {
        return;
    }
    if (t->state == LAYOUT_IN_PROGRESS) {
        diag_error(&s->diag, t->pos, "struct '%.*s' contains itself; use a pointer", SV(t->name));
        t->state = LAYOUT_DONE;
        return;
    }
    if (it == NULL) {
        t->state = LAYOUT_DONE;
        return;
    }

    t->state = LAYOUT_IN_PROGRESS;
    n = it->u.struct_decl.nfields;
    if (n != 0) {
        fields = arena_alloc(s->arena, sizeof(FieldSym) * n);
    }
    for (i = 0; i < n; i++) {
        Field *f = &it->u.struct_decl.fields[i];
        Ty *ft = resolve_type(s, f->type);

        if (ft->kind == TY_STRUCT) {
            layout_struct(s, ft);
        }
        if (!ty_is_sized(ft)) {
            char buf[128];
            diag_error(&s->diag, f->pos, "field '%.*s' has type %s, which has no size", SV(f->name),
                       ty_str(ft, buf, sizeof(buf)));
            ft = poison(s);
        }
        for (j = 0; j < i; j++) {
            if (strview_eq_view(fields[j].name, f->name)) {
                diag_error(&s->diag, f->pos, "duplicate field '%.*s'", SV(f->name));
                diag_note(&s->diag, fields[j].pos, "first declared here");
                break;
            }
        }

        offset = round_up(offset, ft->align);
        fields[i].name = f->name;
        fields[i].type = ft;
        fields[i].offset = offset;
        fields[i].pos = f->pos;
        offset += ft->size;
        if (ft->align > align) {
            align = ft->align;
        }
    }

    t->fields = fields;
    t->nfields = n;
    t->align = align;
    t->size = round_up(offset, align);
    t->state = LAYOUT_DONE;
}

/* Structs by value are forbidden as parameter and return types (GRAMMAR.md
 * section 6): it is what keeps SysV argument classification out of the
 * backend. Arrays go the same way -- v0 has no decay rule to give them. */
static int check_passable(Sema *s, Ty *t, SrcPos pos, const char *what) {
    char buf[128];

    if (t->kind == TY_STRUCT) {
        diag_error(&s->diag, pos, "struct '%.*s' cannot be %s by value; pass a pointer",
                   SV(t->name), what);
        return 0;
    }
    if (t->kind == TY_ARRAY) {
        diag_error(&s->diag, pos, "array type %s cannot be %s; pass a pointer",
                   ty_str(t, buf, sizeof(buf)), what);
        return 0;
    }
    return 1;
}

static void resolve_signature(Sema *s, Sym *sym) {
    FnDecl *fn = sym->decl;
    uint32_t i;

    sym->type = resolve_type(s, fn->ret);
    if (sym->type->kind == TY_STRUCT) {
        layout_struct(s, sym->type);
    }
    check_passable(s, sym->type, fn->pos, "returned");

    sym->nparams = fn->nparams;
    if (fn->nparams != 0) {
        sym->params = arena_alloc(s->arena, sizeof(Ty *) * fn->nparams);
    }
    for (i = 0; i < fn->nparams; i++) {
        Ty *pt = resolve_type(s, fn->params[i].type);
        if (pt->kind == TY_STRUCT) {
            layout_struct(s, pt);
        }
        if (pt->kind == TY_VOID) {
            diag_error(&s->diag, fn->params[i].pos, "parameter '%.*s' cannot have type void",
                       SV(fn->params[i].name));
            pt = poison(s);
        } else if (!check_passable(s, pt, fn->params[i].pos, "passed")) {
            pt = poison(s);
        }
        sym->params[i] = pt;
    }
}

static void resolve_global(Sema *s, Sym *sym) {
    Item *it = sym->item;
    char buf[128];

    sym->type = resolve_type(s, it->u.global.type);
    if (sym->type->kind == TY_STRUCT) {
        layout_struct(s, sym->type);
    }
    if (!ty_is_sized(sym->type)) {
        diag_error(&s->diag, it->pos, "'%.*s' has type %s, which is not a value type",
                   SV(sym->name), ty_str(sym->type, buf, sizeof(buf)));
        sym->type = poison(s);
    }
    if (it->u.global.init != NULL) {
        check_expr(s, &it->u.global.init, sym->type);
    } else if (!sym->is_mut) {
        diag_error(&s->diag, it->pos, "'%.*s' has no initializer, so it must be declared 'let mut'",
                   SV(sym->name));
    }
}

static void resolve_program(Sema *s, Program *prog) {
    uint32_t i, j;

    for (i = 0; i < prog->nitems; i++) {
        Item *it = prog->items[i];
        Sym *sym;
        Ty *t;

        if (it == NULL) {
            continue;
        }
        switch (it->kind) {
        case ITEM_STRUCT:
            t = globals_find_type(&s->globals, it->u.struct_decl.name);
            /* Skip a rejected duplicate: the name belongs to the first one. */
            if (t != NULL && t->decl == it) {
                layout_struct(s, t);
            }
            break;
        case ITEM_ENUM:
            break; /* nothing left to resolve: pass 2 numbered them */
        case ITEM_FN:
            sym = globals_find_value(&s->globals, it->u.fn.name);
            if (sym != NULL && sym->kind == SYM_FN && sym->decl == &it->u.fn) {
                resolve_signature(s, sym);
            }
            break;
        case ITEM_GLOBAL:
            sym = globals_find_value(&s->globals, it->u.global.name);
            if (sym != NULL && sym->kind == SYM_GLOBAL && sym->item == it) {
                resolve_global(s, sym);
            }
            break;
        case ITEM_EXTERN:
            for (j = 0; j < it->u.extern_block.nfns; j++) {
                FnDecl *fn = &it->u.extern_block.fns[j];
                sym = globals_find_value(&s->globals, fn->name);
                if (sym != NULL && sym->kind == SYM_FN && sym->decl == fn) {
                    resolve_signature(s, sym);
                }
            }
            break;
        }
    }
}

/* ================================================================ pass 4 ==
 * Body checking. */

/* GRAMMAR.md section 6: widening within the same signedness is implicit and
 * everything else -- narrowing, signedness changes, int/float, and any
 * pointer conversion -- needs an explicit .as(T). */
static int assignable(Ty *from, Ty *to) {
    if (from == to) {
        return 1;
    }
    if (from->kind == TY_POISON || to->kind == TY_POISON) {
        return 1;
    }
    switch (from->kind) {
    case TY_UNTYPED_INT:
        return ty_is_integer(to) || ty_is_float(to) || to->kind == TY_UNTYPED_FLOAT;
    case TY_UNTYPED_FLOAT:
        return ty_is_float(to);
    case TY_UNTYPED_BOOL:
        return ty_is_bool(to);
    case TY_NULL:
        return to->kind == TY_PTR;
    case TY_ENUM:
        /* An enum is assignable to what its underlying type is assignable
         * to. Required by the common-prefix trick: `n.kind = N_INT` stores a
         * NodeKind into an i32 field. */
        return assignable(from->base, to);
    default:
        break;
    }
    if (ty_is_integer(from) && ty_is_integer(to)) {
        return ty_is_signed(from) == ty_is_signed(to) && to->size > from->size;
    }
    if (from->kind == TY_F32 && to->kind == TY_F64) {
        return 1;
    }
    if (ty_is_bool(from) && ty_is_bool(to)) {
        return to->size > from->size;
    }
    return 0;
}

static Ty *common_type(Ty *a, Ty *b) {
    if (a == b) {
        return a;
    }
    if (assignable(a, b)) {
        return b;
    }
    if (assignable(b, a)) {
        return a;
    }
    return NULL;
}

/* Does this operator hand its operands' type to its result? Only those may
 * push a context type down into an untyped subtree. */
static int op_propagates(TokenKind op) {
    switch (op) {
    case TOK_PLUS:
    case TOK_MINUS:
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_PERCENT:
    case TOK_AMP:
    case TOK_PIPE:
    case TOK_CARET:
        return 1;
    default:
        return 0;
    }
}

/* An untyped literal does not get converted -- it *is* the target type. That
 * has to reach through the operators that kept the expression untyped, or
 * `(size + 7) & ~7` computes ~7 as i32 and then widens the wrong value. */
static void retype_untyped(Sema *s, Expr *e, Ty *target) {
    char buf[128];

    if (e == NULL) {
        return;
    }
    switch (e->kind) {
    case EXPR_INT:
    case EXPR_CHAR:
        if (ty_is_integer(target) && !ty_int_fits(target, e->u.ival)) {
            diag_error(&s->diag, e->pos, "integer literal %llu does not fit in %s",
                       (unsigned long long)e->u.ival, ty_str(target, buf, sizeof(buf)));
        }
        break;
    case EXPR_UNARY:
        if (e->u.unary.op == TOK_MINUS || e->u.unary.op == TOK_TILDE) {
            retype_untyped(s, e->u.unary.operand, target);
        }
        break;
    case EXPR_BINARY:
        if (op_propagates(e->u.binary.op)) {
            retype_untyped(s, e->u.binary.lhs, target);
            retype_untyped(s, e->u.binary.rhs, target);
        } else if (e->u.binary.op == TOK_SHL || e->u.binary.op == TOK_SHR) {
            retype_untyped(s, e->u.binary.lhs, target);
        }
        break;
    default:
        break;
    }
    e->sem_type = target;
}

static Ty *coerce(Sema *s, Expr **slot, Ty *target) {
    Expr *e = *slot;
    Ty *from;
    Expr *conv;
    char b1[128], b2[128];

    if (e == NULL || target == NULL) {
        return target != NULL ? target : poison(s);
    }
    from = e->sem_type;
    if (from == NULL || from == target || from->kind == TY_POISON || target->kind == TY_POISON) {
        return target;
    }
    if (!assignable(from, target)) {
        diag_error(&s->diag, e->pos, "cannot convert %s to %s implicitly; use '.as(%s)'",
                   ty_str(from, b1, sizeof(b1)), ty_str(target, b2, sizeof(b2)),
                   ty_str(target, b2, sizeof(b2)));
        e->sem_type = poison(s);
        return poison(s);
    }
    if (ty_is_untyped(from)) {
        retype_untyped(s, e, target);
        return target;
    }

    conv = expr_new(s->arena, EXPR_CONV, e->pos);
    conv->u.conv.operand = e;
    conv->sem_type = target;
    *slot = conv;
    return target;
}

/* The type an untyped expression takes when nothing gave it a context. */
static Ty *default_type(Sema *s, Expr *e) {
    switch (e->sem_type->kind) {
    case TY_UNTYPED_INT:
        /* Char literals are written as bytes, so that is what they default
         * to; a bare integer literal defaults to i32 per GRAMMAR.md. */
        return e->kind == EXPR_CHAR ? core(s, TY_U8) : core(s, TY_I32);
    case TY_UNTYPED_FLOAT:
        return core(s, TY_F64);
    case TY_UNTYPED_BOOL:
        return core(s, TY_B8);
    case TY_NULL:
        return ty_ptr(&s->types, core(s, TY_VOID));
    default:
        return e->sem_type;
    }
}

/* Checks an expression that has to end up with a real type. */
static Ty *check_value(Sema *s, Expr **slot) {
    Ty *t = check_expr(s, slot, NULL);
    if (t != NULL && ty_is_untyped(t)) {
        return coerce(s, slot, default_type(s, *slot));
    }
    return t;
}

/* ------------------------------------------------------------- lvalues */

/* GRAMMAR.md section 4 has the parser accept `a + b = 5` so this can be a
 * real diagnostic rather than "unexpected token". An lvalue is a name, a
 * dereference, an index or a field -- a shape, not a grammar rule. */
static int is_lvalue(Expr *e) {
    if (e == NULL) {
        return 0;
    }
    switch (e->kind) {
    case EXPR_NAME:
    case EXPR_INDEX:
    case EXPR_FIELD:
        return 1;
    case EXPR_UNARY:
        return e->u.unary.op == TOK_STAR;
    default:
        return 0;
    }
}

/* The variable an assignment ultimately writes to, or NULL when the path
 * goes through a pointer -- v0 has no const, so a pointer says nothing
 * about whether its pointee may be written. */
static Sym *lvalue_root(Sema *s, Expr *e) {
    Sym *sym;

    switch (e->kind) {
    case EXPR_NAME:
        sym = scope_find(&s->scopes, e->u.sval);
        if (sym == NULL) {
            sym = globals_find_value(&s->globals, e->u.sval);
        }
        return sym;
    case EXPR_INDEX:
        if (e->u.index.base != NULL && e->u.index.base->sem_type != NULL &&
            e->u.index.base->sem_type->kind == TY_PTR) {
            return NULL;
        }
        return lvalue_root(s, e->u.index.base);
    case EXPR_FIELD:
        if (e->u.field.base != NULL && e->u.field.base->sem_type != NULL &&
            e->u.field.base->sem_type->kind == TY_PTR) {
            return NULL;
        }
        return lvalue_root(s, e->u.field.base);
    default:
        return NULL; /* through a dereference */
    }
}

static Ty *check_assign_target(Sema *s, Expr **slot) {
    Ty *t = check_value(s, slot);
    Expr *e = *slot;
    Sym *root;

    if (t->kind == TY_POISON) {
        return t;
    }
    if (!is_lvalue(e)) {
        diag_error(&s->diag, e->pos, "cannot assign to this expression");
        return poison(s);
    }
    if (t->kind == TY_ARRAY) {
        diag_error(&s->diag, e->pos, "cannot assign to a whole array");
        return poison(s);
    }
    root = lvalue_root(s, e);
    if (root != NULL) {
        if (root->kind == SYM_FN || root->kind == SYM_ENUM_MEMBER) {
            diag_error(&s->diag, e->pos, "cannot assign to '%.*s'", SV(root->name));
            return poison(s);
        }
        if (!root->is_mut) {
            diag_error(&s->diag, e->pos, "'%.*s' is not mutable; declare it with 'let mut'",
                       SV(root->name));
            diag_note(&s->diag, root->pos, "declared here");
        }
    }
    return t;
}

/* --------------------------------------------------------- expressions */

static TokenKind assign_binop(TokenKind op) {
    switch (op) {
    case TOK_PLUS_EQ:
        return TOK_PLUS;
    case TOK_MINUS_EQ:
        return TOK_MINUS;
    case TOK_STAR_EQ:
        return TOK_STAR;
    case TOK_SLASH_EQ:
        return TOK_SLASH;
    case TOK_PERCENT_EQ:
        return TOK_PERCENT;
    case TOK_AMP_EQ:
        return TOK_AMP;
    case TOK_PIPE_EQ:
        return TOK_PIPE;
    case TOK_CARET_EQ:
        return TOK_CARET;
    case TOK_SHL_EQ:
        return TOK_SHL;
    case TOK_SHR_EQ:
        return TOK_SHR;
    default:
        return TOK_EQ;
    }
}

static int is_integerish(Ty *t) {
    return ty_is_integer(t) || t->kind == TY_UNTYPED_INT || t->kind == TY_ENUM;
}

/* Both operands are already checked; this applies the operator's rule and
 * coerces them to whatever it needs. */
static Ty *check_binary_op(Sema *s, TokenKind op, SrcPos pos, Expr **lslot, Expr **rslot) {
    Ty *lt = (*lslot)->sem_type;
    Ty *rt = (*rslot)->sem_type;
    Ty *c;
    char b1[128], b2[128];

    if (lt == NULL || rt == NULL || lt->kind == TY_POISON || rt->kind == TY_POISON) {
        return poison(s);
    }

    /* Pointer arithmetic scales by sizeof(T); p - q is i64 in elements. */
    if (op == TOK_PLUS || op == TOK_MINUS) {
        if (lt->kind == TY_PTR && is_integerish(rt)) {
            coerce(s, rslot, ty_is_integer(rt) ? rt : core(s, TY_I64));
            return lt;
        }
        if (op == TOK_PLUS && rt->kind == TY_PTR && is_integerish(lt)) {
            coerce(s, lslot, ty_is_integer(lt) ? lt : core(s, TY_I64));
            return rt;
        }
        if (op == TOK_MINUS && lt->kind == TY_PTR && rt->kind == TY_PTR) {
            if (lt != rt) {
                diag_error(&s->diag, pos, "cannot subtract %s from %s", ty_str(rt, b1, sizeof(b1)),
                           ty_str(lt, b2, sizeof(b2)));
                return poison(s);
            }
            return core(s, TY_I64);
        }
    }

    switch (op) {
    case TOK_AMP_AMP:
    case TOK_PIPE_PIPE:
        if (!ty_is_bool(lt) && lt->kind != TY_UNTYPED_BOOL) {
            diag_error(&s->diag, pos, "'%s' needs a boolean left operand, not %s",
                       token_kind_name(op), ty_str(lt, b1, sizeof(b1)));
            return poison(s);
        }
        if (!ty_is_bool(rt) && rt->kind != TY_UNTYPED_BOOL) {
            diag_error(&s->diag, pos, "'%s' needs a boolean right operand, not %s",
                       token_kind_name(op), ty_str(rt, b1, sizeof(b1)));
            return poison(s);
        }
        return core(s, TY_UNTYPED_BOOL);

    case TOK_SHL:
    case TOK_SHR:
        if (!is_integerish(lt) || !is_integerish(rt)) {
            diag_error(&s->diag, pos, "'%s' needs integer operands", token_kind_name(op));
            return poison(s);
        }
        /* The shift amount is typed on its own: it says how far, not what. */
        if (rt->kind == TY_UNTYPED_INT) {
            coerce(s, rslot, core(s, TY_I32));
        }
        return lt;

    case TOK_EQ_EQ:
    case TOK_BANG_EQ:
        if (lt->kind == TY_PTR && rt->kind == TY_NULL) {
            coerce(s, rslot, lt);
            return core(s, TY_UNTYPED_BOOL);
        }
        if (lt->kind == TY_NULL && rt->kind == TY_PTR) {
            coerce(s, lslot, rt);
            return core(s, TY_UNTYPED_BOOL);
        }
        /* fall through */
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:
        c = common_type(lt, rt);
        if (c == NULL || (!ty_is_numeric(c) && c->kind != TY_PTR && c->kind != TY_ENUM)) {
            diag_error(&s->diag, pos, "cannot compare %s with %s", ty_str(lt, b1, sizeof(b1)),
                       ty_str(rt, b2, sizeof(b2)));
            return poison(s);
        }
        coerce(s, lslot, c);
        coerce(s, rslot, c);
        return core(s, TY_UNTYPED_BOOL);

    case TOK_PERCENT:
    case TOK_AMP:
    case TOK_PIPE:
    case TOK_CARET:
        if (!is_integerish(lt) || !is_integerish(rt)) {
            diag_error(&s->diag, pos, "'%s' is not defined for %s and %s", token_kind_name(op),
                       ty_str(lt, b1, sizeof(b1)), ty_str(rt, b2, sizeof(b2)));
            return poison(s);
        }
        break;

    default: /* + - * / on numbers */
        if (!ty_is_numeric(lt) || !ty_is_numeric(rt)) {
            diag_error(&s->diag, pos, "'%s' is not defined for %s and %s", token_kind_name(op),
                       ty_str(lt, b1, sizeof(b1)), ty_str(rt, b2, sizeof(b2)));
            return poison(s);
        }
        break;
    }

    c = common_type(lt, rt);
    if (c == NULL) {
        diag_error(&s->diag, pos, "'%s' has mismatched operands: %s and %s", token_kind_name(op),
                   ty_str(lt, b1, sizeof(b1)), ty_str(rt, b2, sizeof(b2)));
        return poison(s);
    }
    coerce(s, lslot, c);
    coerce(s, rslot, c);
    return c;
}

static Ty *check_unary(Sema *s, Expr *e) {
    Ty *t;
    char buf[128];

    switch (e->u.unary.op) {
    case TOK_MINUS:
        t = check_expr(s, &e->u.unary.operand, NULL);
        if (t->kind == TY_POISON) {
            return t;
        }
        if (!ty_is_numeric(t)) {
            diag_error(&s->diag, e->pos, "cannot negate %s", ty_str(t, buf, sizeof(buf)));
            return poison(s);
        }
        if (ty_is_integer(t) && !ty_is_signed(t)) {
            diag_error(&s->diag, e->pos, "cannot negate %s, which is unsigned",
                       ty_str(t, buf, sizeof(buf)));
            return poison(s);
        }
        return t;

    case TOK_TILDE:
        t = check_expr(s, &e->u.unary.operand, NULL);
        if (t->kind == TY_POISON) {
            return t;
        }
        if (!is_integerish(t)) {
            diag_error(&s->diag, e->pos, "'~' needs an integer, not %s",
                       ty_str(t, buf, sizeof(buf)));
            return poison(s);
        }
        return t;

    case TOK_BANG:
        t = check_expr(s, &e->u.unary.operand, NULL);
        if (t->kind == TY_POISON) {
            return t;
        }
        if (!ty_is_bool(t) && t->kind != TY_UNTYPED_BOOL) {
            diag_error(&s->diag, e->pos, "'!' needs a boolean, not %s",
                       ty_str(t, buf, sizeof(buf)));
            return poison(s);
        }
        return core(s, TY_UNTYPED_BOOL);

    case TOK_STAR:
        t = check_value(s, &e->u.unary.operand);
        if (t->kind == TY_POISON) {
            return t;
        }
        if (t->kind != TY_PTR) {
            diag_error(&s->diag, e->pos, "cannot dereference %s", ty_str(t, buf, sizeof(buf)));
            return poison(s);
        }
        if (t->elem->kind == TY_VOID) {
            diag_error(&s->diag, e->pos, "cannot dereference void*; cast it first");
            return poison(s);
        }
        return t->elem;

    case TOK_AMP:
        t = check_value(s, &e->u.unary.operand);
        if (t->kind == TY_POISON) {
            return t;
        }
        if (!is_lvalue(e->u.unary.operand)) {
            diag_error(&s->diag, e->pos, "cannot take the address of this expression");
            return poison(s);
        }
        return ty_ptr(&s->types, t);

    default:
        return poison(s);
    }
}

/* C's default argument promotions, which is what makes printf work: the
 * callee has no parameter type to convert against. */
static void promote_vararg(Sema *s, Expr **slot) {
    Ty *t = check_value(s, slot);

    if (t->kind == TY_F32) {
        coerce(s, slot, core(s, TY_F64));
    } else if (t->kind == TY_I8) {
        coerce(s, slot, core(s, TY_I32));
    } else if (t->kind == TY_U8) {
        coerce(s, slot, core(s, TY_U32));
    }
}

static Ty *check_call(Sema *s, Expr *e) {
    Expr *callee = e->u.call.callee;
    Sym *sym;
    uint32_t i;

    if (callee == NULL || callee->kind != EXPR_NAME) {
        diag_error(&s->diag, e->pos,
                   "only a function name can be called; v0 has no function "
                   "pointers");
        return poison(s);
    }
    sym = scope_find(&s->scopes, callee->u.sval);
    if (sym == NULL) {
        sym = globals_find_value(&s->globals, callee->u.sval);
    }
    if (sym == NULL) {
        diag_error(&s->diag, callee->pos, "undeclared function '%.*s'", SV(callee->u.sval));
        return poison(s);
    }
    if (sym->kind != SYM_FN) {
        diag_error(&s->diag, callee->pos, "'%.*s' is not a function", SV(callee->u.sval));
        return poison(s);
    }

    if (e->u.call.nargs < sym->nparams) {
        diag_error(&s->diag, e->pos, "too few arguments to '%.*s': expected %u, got %u",
                   SV(sym->name), sym->nparams, e->u.call.nargs);
    } else if (e->u.call.nargs > sym->nparams && !sym->is_variadic) {
        diag_error(&s->diag, e->pos, "too many arguments to '%.*s': expected %u, got %u",
                   SV(sym->name), sym->nparams, e->u.call.nargs);
    }

    for (i = 0; i < e->u.call.nargs; i++) {
        if (i < sym->nparams) {
            Expr **slot = &e->u.call.args[i];
            Ty *at = check_expr(s, slot, NULL);
            if (at->kind != TY_POISON && !assignable(at, sym->params[i])) {
                char b1[128], b2[128];
                diag_error(&s->diag, (*slot)->pos, "argument %u of '%.*s': cannot convert %s to %s",
                           i + 1, SV(sym->name), ty_str(at, b1, sizeof(b1)),
                           ty_str(sym->params[i], b2, sizeof(b2)));
            } else {
                coerce(s, slot, sym->params[i]);
            }
        } else {
            promote_vararg(s, &e->u.call.args[i]);
        }
    }
    return sym->type != NULL ? sym->type : poison(s);
}

/* `.as(T)` permits numeric to numeric in any direction, pointer to pointer,
 * and a boolean to a number. Integer to pointer is not a conversion v0
 * needs, and allowing it would hide real mistakes. */
static int cast_allowed(Ty *from, Ty *to) {
    if (from->kind == TY_POISON || to->kind == TY_POISON || from == to) {
        return 1;
    }
    if (from->kind == TY_ENUM) {
        from = from->base;
    }
    if (to->kind == TY_ENUM) {
        to = to->base;
    }
    if (ty_is_numeric(from) && ty_is_numeric(to)) {
        return 1;
    }
    if ((from->kind == TY_PTR || from->kind == TY_NULL) && to->kind == TY_PTR) {
        return 1;
    }
    if ((ty_is_bool(from) || from->kind == TY_UNTYPED_BOOL) && ty_is_numeric(to)) {
        return 1;
    }
    if ((ty_is_bool(from) || from->kind == TY_UNTYPED_BOOL) && ty_is_bool(to)) {
        return 1;
    }
    return 0;
}

static Ty *check_cast(Sema *s, Expr *e) {
    Ty *target = resolve_type(s, e->u.cast.type);
    Ty *from;
    char b1[128], b2[128];

    if (target->kind == TY_STRUCT) {
        layout_struct(s, target);
    }
    from = check_expr(s, &e->u.cast.operand, NULL);

    /* An untyped literal simply becomes the target type; there is nothing to
     * convert, and this is what makes 0xFFFFFFFF.as(u64) work. */
    if (ty_is_untyped(from) && assignable(from, target)) {
        retype_untyped(s, e->u.cast.operand, target);
        return target;
    }
    if (ty_is_untyped(from)) {
        from = check_value(s, &e->u.cast.operand);
    }
    if (!ty_is_sized(target) && target->kind != TY_POISON) {
        diag_error(&s->diag, e->pos, "cannot cast to %s", ty_str(target, b1, sizeof(b1)));
        return poison(s);
    }
    if (!cast_allowed(from, target)) {
        diag_error(&s->diag, e->pos, "cannot convert %s to %s", ty_str(from, b1, sizeof(b1)),
                   ty_str(target, b2, sizeof(b2)));
        return poison(s);
    }
    return target;
}

static Ty *check_expr(Sema *s, Expr **slot, Ty *expected) {
    Expr *e = *slot;
    Ty *t;
    char b1[128];

    if (e == NULL) {
        return poison(s);
    }

    switch (e->kind) {
    case EXPR_INT:
    case EXPR_CHAR:
        t = core(s, TY_UNTYPED_INT);
        break;
    case EXPR_FLOAT:
        t = core(s, TY_UNTYPED_FLOAT);
        break;
    case EXPR_BOOL:
        t = core(s, TY_UNTYPED_BOOL);
        break;
    case EXPR_NULL:
        t = core(s, TY_NULL);
        break;
    case EXPR_STRING:
        /* String literals are u8*, into static data, NUL-terminated. */
        t = ty_ptr(&s->types, core(s, TY_U8));
        break;

    case EXPR_NAME: {
        Sym *sym = scope_find(&s->scopes, e->u.sval);
        if (sym == NULL) {
            sym = globals_find_value(&s->globals, e->u.sval);
        }
        if (sym == NULL) {
            diag_error(&s->diag, e->pos, "undeclared name '%.*s'", SV(e->u.sval));
            t = poison(s);
        } else if (sym->kind == SYM_FN) {
            diag_error(&s->diag, e->pos, "'%.*s' is a function; v0 has no function pointers",
                       SV(e->u.sval));
            t = poison(s);
        } else {
            t = sym->type != NULL ? sym->type : poison(s);
        }
        break;
    }

    case EXPR_UNARY:
        t = check_unary(s, e);
        break;

    case EXPR_BINARY:
        check_expr(s, &e->u.binary.lhs, NULL);
        check_expr(s, &e->u.binary.rhs, NULL);
        t = check_binary_op(s, e->u.binary.op, e->pos, &e->u.binary.lhs, &e->u.binary.rhs);
        break;

    case EXPR_CALL:
        t = check_call(s, e);
        break;

    case EXPR_INDEX: {
        Ty *bt = check_value(s, &e->u.index.base);
        Ty *it = check_expr(s, &e->u.index.index, NULL);
        if (it->kind == TY_UNTYPED_INT) {
            coerce(s, &e->u.index.index, core(s, TY_I64));
        } else if (!ty_is_integer(it) && it->kind != TY_POISON) {
            diag_error(&s->diag, e->pos, "index must be an integer, not %s",
                       ty_str(it, b1, sizeof(b1)));
        }
        if (bt->kind == TY_ARRAY) {
            t = bt->elem;
        } else if (bt->kind == TY_PTR && bt->elem->kind != TY_VOID) {
            t = bt->elem;
        } else if (bt->kind == TY_POISON) {
            t = bt;
        } else {
            diag_error(&s->diag, e->pos, "cannot index %s", ty_str(bt, b1, sizeof(b1)));
            t = poison(s);
        }
        break;
    }

    case EXPR_FIELD: {
        Ty *bt = check_value(s, &e->u.field.base);
        const FieldSym *f;
        /* One level of pointer is stripped automatically; there is no `->`. */
        if (bt->kind == TY_PTR) {
            bt = bt->elem;
        }
        if (bt->kind == TY_STRUCT) {
            layout_struct(s, bt);
        }
        if (bt->kind == TY_POISON) {
            t = bt;
        } else if (bt->kind != TY_STRUCT) {
            diag_error(&s->diag, e->pos, "%s has no fields", ty_str(bt, b1, sizeof(b1)));
            t = poison(s);
        } else {
            f = ty_field(bt, e->u.field.name);
            if (f == NULL) {
                diag_error(&s->diag, e->pos, "struct '%.*s' has no field '%.*s'", SV(bt->name),
                           SV(e->u.field.name));
                t = poison(s);
            } else {
                t = f->type;
            }
        }
        break;
    }

    case EXPR_CAST:
        t = check_cast(s, e);
        break;

    case EXPR_SIZEOF: {
        Ty *target = resolve_type(s, e->u.size_of.type);
        if (target->kind == TY_STRUCT) {
            layout_struct(s, target);
        }
        if (!ty_is_sized(target) && target->kind != TY_POISON) {
            diag_error(&s->diag, e->pos, "'sizeof' needs a sized type, not %s",
                       ty_str(target, b1, sizeof(b1)));
        }
        t = core(s, TY_U64);
        break;
    }

    case EXPR_CONV:
        /* Only sema makes these, and only after the operand is checked. */
        t = e->sem_type != NULL ? e->sem_type : poison(s);
        break;

    default:
        t = poison(s);
        break;
    }

    e->sem_type = t;
    if (expected != NULL) {
        return coerce(s, slot, expected);
    }
    return t;
}

/* ---------------------------------------------------------- statements */

static void check_condition(Sema *s, Expr **slot, const char *what) {
    Ty *t = check_expr(s, slot, NULL);
    char buf[128];

    if (t->kind == TY_UNTYPED_BOOL) {
        coerce(s, slot, core(s, TY_B8));
        return;
    }
    if (!ty_is_bool(t) && t->kind != TY_POISON) {
        diag_error(&s->diag, (*slot)->pos,
                   "the %s must have type b8, b32 or b64, not %s; write 'x != 0'", what,
                   ty_str(t, buf, sizeof(buf)));
    }
}

static void check_var_decl(Sema *s, Stmt *st) {
    Ty *t = resolve_type(s, st->u.var.type);
    Sym *sym;
    char buf[128];

    if (t->kind == TY_STRUCT) {
        layout_struct(s, t);
    }
    if (!ty_is_sized(t) && t->kind != TY_POISON) {
        diag_error(&s->diag, st->pos, "'%.*s' has type %s, which is not a value type",
                   SV(st->u.var.name), ty_str(t, buf, sizeof(buf)));
        t = poison(s);
    }

    if (st->u.var.init != NULL) {
        check_expr(s, &st->u.var.init, t);
    } else if (!st->u.var.is_mut) {
        diag_error(&s->diag, st->pos, "'%.*s' has no initializer, so it must be declared 'let mut'",
                   SV(st->u.var.name));
    }

    /* Declared after its own initializer, so `let i32 x = x;` names the
     * outer x rather than reading itself. */
    sym = scope_find_current(&s->scopes, st->u.var.name);
    if (sym != NULL) {
        diag_error(&s->diag, st->pos, "'%.*s' is already declared in this scope",
                   SV(st->u.var.name));
        diag_note(&s->diag, sym->pos, "first declared here");
        return;
    }
    sym = sym_new(s, SYM_LOCAL, st->u.var.name, st->pos);
    sym->type = t;
    sym->is_mut = st->u.var.is_mut;
    scope_declare(&s->scopes, sym);
}

static void check_match(Sema *s, Stmt *st) {
    Ty *st_ty = check_value(s, &st->u.match.scrutinee);
    char b1[128], b2[128];
    uint32_t i, j;

    if (st_ty->kind != TY_POISON && !ty_is_integer(st_ty) && st_ty->kind != TY_ENUM) {
        diag_error(&s->diag, st->pos, "match needs an integer or enum value, not %s",
                   ty_str(st_ty, b1, sizeof(b1)));
        st_ty = poison(s);
    }

    for (i = 0; i < st->u.match.narms; i++) {
        MatchArm *arm = &st->u.match.arms[i];
        for (j = 0; j < arm->nlabels; j++) {
            MatchLabel *label = &arm->labels[j];
            Expr *lv = label->value;

            if (label->is_wildcard || lv == NULL) {
                continue;
            }
            if (lv->kind == EXPR_NAME) {
                Sym *sym = globals_find_value(&s->globals, lv->u.sval);
                if (sym == NULL || sym->kind != SYM_ENUM_MEMBER) {
                    diag_error(&s->diag, label->pos,
                               "'%.*s' is not an enum member; a match label must be a constant",
                               SV(lv->u.sval));
                    continue;
                }
                lv->sem_type = sym->type;
                if (st_ty->kind != TY_POISON && !assignable(sym->type, st_ty)) {
                    diag_error(&s->diag, label->pos, "label of type %s does not match %s",
                               ty_str(sym->type, b1, sizeof(b1)), ty_str(st_ty, b2, sizeof(b2)));
                }
            } else {
                lv->sem_type = core(s, TY_UNTYPED_INT);
                if (st_ty->kind != TY_POISON) {
                    retype_untyped(s, lv, st_ty);
                }
            }
        }
        check_stmt(s, arm->body);
    }
}

static void check_stmt(Sema *s, Stmt *st) {
    char b1[128], b2[128];

    if (st == NULL) {
        return;
    }
    switch (st->kind) {
    case STMT_BLOCK:
        check_block(s, &st->u.block);
        break;

    case STMT_VAR:
        check_var_decl(s, st);
        break;

    case STMT_IF:
        check_condition(s, &st->u.if_stmt.cond, "condition of 'if'");
        check_block(s, &st->u.if_stmt.then_block);
        if (st->u.if_stmt.else_stmt != NULL) {
            check_stmt(s, st->u.if_stmt.else_stmt);
        }
        break;

    case STMT_WHILE:
        check_condition(s, &st->u.while_stmt.cond, "condition of 'while'");
        s->loop_depth++;
        check_block(s, &st->u.while_stmt.body);
        s->loop_depth--;
        break;

    case STMT_MATCH:
        check_match(s, st);
        break;

    case STMT_RETURN: {
        Ty *want = s->cur_fn != NULL && s->cur_fn->type != NULL ? s->cur_fn->type : poison(s);
        if (st->u.ret.value == NULL) {
            if (want->kind != TY_VOID && want->kind != TY_POISON) {
                diag_error(&s->diag, st->pos, "'return' needs a value of type %s",
                           ty_str(want, b1, sizeof(b1)));
            }
        } else if (want->kind == TY_VOID) {
            diag_error(&s->diag, st->pos, "'return' with a value in a function returning void");
            check_value(s, &st->u.ret.value);
        } else {
            check_expr(s, &st->u.ret.value, want);
        }
        break;
    }

    case STMT_BREAK:
    case STMT_CONTINUE:
        if (s->loop_depth == 0) {
            diag_error(&s->diag, st->pos, "'%s' outside a loop",
                       st->kind == STMT_BREAK ? "break" : "continue");
        }
        break;

    case STMT_EXPR:
        check_value(s, &st->u.expr.expr);
        break;

    case STMT_ASSIGN: {
        Ty *target = check_assign_target(s, &st->u.assign.target);
        if (st->u.assign.op == TOK_EQ) {
            check_expr(s, &st->u.assign.value, target);
        } else {
            TokenKind op = assign_binop(st->u.assign.op);
            Ty *result;
            check_expr(s, &st->u.assign.value, NULL);
            result = check_binary_op(s, op, st->pos, &st->u.assign.target, &st->u.assign.value);
            if (result->kind != TY_POISON && target->kind != TY_POISON &&
                !assignable(result, target)) {
                diag_error(&s->diag, st->pos, "'%s' produces %s, which does not fit %s",
                           token_kind_name(st->u.assign.op), ty_str(result, b1, sizeof(b1)),
                           ty_str(target, b2, sizeof(b2)));
            }
        }
        break;
    }

    case STMT_EMPTY:
        break;
    }
}

static void check_block(Sema *s, Block *b) {
    uint32_t i;

    scope_push(&s->scopes);
    for (i = 0; i < b->nstmts; i++) {
        check_stmt(s, b->stmts[i]);
    }
    scope_pop(&s->scopes);
}

/* ------------------------------------------------- return reachability */

static int block_diverges(const Block *b);

static int stmt_diverges(const Stmt *st) {
    uint32_t i;
    int has_wildcard = 0;

    if (st == NULL) {
        return 0;
    }
    switch (st->kind) {
    case STMT_RETURN:
        return 1;
    case STMT_BREAK:
    case STMT_CONTINUE:
        /* Not a return, but nothing after it in this block runs either. */
        return 1;
    case STMT_BLOCK:
        return block_diverges(&st->u.block);
    case STMT_IF:
        return st->u.if_stmt.else_stmt != NULL && block_diverges(&st->u.if_stmt.then_block) &&
               stmt_diverges(st->u.if_stmt.else_stmt);
    case STMT_MATCH:
        /* GRAMMAR.md section 6: a match diverges when it has a `_` arm and
         * every arm diverges. Without that, a function whose every arm
         * returns would still demand a trailing unreachable return. */
        for (i = 0; i < st->u.match.narms; i++) {
            uint32_t j;
            for (j = 0; j < st->u.match.arms[i].nlabels; j++) {
                if (st->u.match.arms[i].labels[j].is_wildcard) {
                    has_wildcard = 1;
                }
            }
            if (!stmt_diverges(st->u.match.arms[i].body)) {
                return 0;
            }
        }
        return has_wildcard;
    default:
        return 0;
    }
}

static int block_diverges(const Block *b) {
    uint32_t i;
    for (i = 0; i < b->nstmts; i++) {
        if (stmt_diverges(b->stmts[i])) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ function */

static void check_fn(Sema *s, Sym *sym) {
    FnDecl *fn = sym->decl;
    uint32_t i;
    char buf[128];

    if (!fn->has_body) {
        return;
    }
    s->cur_fn = sym;
    s->loop_depth = 0;

    /* Parameters get the function's outermost scope, so a local may shadow
     * one without that being a redeclaration. */
    scope_push(&s->scopes);
    for (i = 0; i < fn->nparams && i < sym->nparams; i++) {
        Sym *p = scope_find_current(&s->scopes, fn->params[i].name);
        if (p != NULL) {
            diag_error(&s->diag, fn->params[i].pos, "duplicate parameter '%.*s'",
                       SV(fn->params[i].name));
            diag_note(&s->diag, p->pos, "first declared here");
            continue;
        }
        p = sym_new(s, SYM_PARAM, fn->params[i].name, fn->params[i].pos);
        p->type = sym->params[i];
        p->is_mut = fn->params[i].is_mut;
        scope_declare(&s->scopes, p);
    }

    check_block(s, &fn->body);
    scope_pop(&s->scopes);

    if (sym->type != NULL && sym->type->kind != TY_VOID && sym->type->kind != TY_POISON &&
        !block_diverges(&fn->body)) {
        diag_error(&s->diag, fn->pos, "missing return in '%.*s', which returns %s", SV(fn->name),
                   ty_str(sym->type, buf, sizeof(buf)));
    }
    s->cur_fn = NULL;
}

static void check_program(Sema *s, Program *prog) {
    uint32_t i;

    for (i = 0; i < prog->nitems; i++) {
        Item *it = prog->items[i];
        Sym *sym;

        if (it == NULL || it->kind != ITEM_FN) {
            continue;
        }
        sym = globals_find_value(&s->globals, it->u.fn.name);
        if (sym != NULL && sym->kind == SYM_FN && sym->decl == &it->u.fn) {
            check_fn(s, sym);
        }
    }
}

/* ---------------------------------------------------------------- entry */

void sema_print_type(FILE *out, const struct Ty *ty) {
    char buf[128];
    fputs(ty_str((const Ty *)ty, buf, sizeof(buf)), out);
}

int sema_check(Arena *arena, Program **programs, size_t nprograms) {
    Sema s;
    size_t i;
    int errors;

    memset(&s, 0, sizeof(s));
    s.arena = arena;
    diag_init(&s.diag, nprograms > 0 ? programs[0]->file : "<input>");
    types_init(&s.types, arena);
    globals_init(&s.globals);
    scopes_init(&s.scopes);

    for (i = 0; i < nprograms; i++) {
        collect_program(&s, programs[i]);
    }
    for (i = 0; i < nprograms; i++) {
        resolve_program(&s, programs[i]);
    }
    for (i = 0; i < nprograms; i++) {
        check_program(&s, programs[i]);
    }

    errors = diag_error_count(&s.diag);
    scopes_free(&s.scopes);
    globals_free(&s.globals);
    types_free(&s.types);
    return errors;
}
