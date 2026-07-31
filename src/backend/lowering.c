#include "backend/lowering.h"

#include "sema/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ environment */

typedef struct {
    StrView name;
    IrValue *addr; /* the alloca, or the global */
    IrType *type;  /* what lives at that address */
    const Ty *ty;
} Binding;

typedef struct {
    StrView name;
    IrType *type;
    Item *decl;
} NamedType;

typedef struct {
    StrView name;
    IrFunction *fn;
} FuncSym;

typedef struct {
    IrBlock *continue_to;
    IrBlock *break_to;
} Loop;

typedef struct {
    Arena *arena;
    IrModule *m;
    IrBuilder b;

    Binding *globals;
    size_t nglobals, cglobals;
    Binding *locals;
    size_t nlocals, clocals;
    NamedType *types;
    size_t ntypes, ctypes;
    FuncSym *funcs;
    size_t nfuncs, cfuncs;
    Loop *loops;
    size_t nloops, cloops;

    IrFunction *cur;
    const Ty *cur_ret_ty;
    char **used_names; /* names taken in the current function */
    size_t nused, cused;
    IrFunction *memcpy_fn;
    IrFunction *memset_fn;
    uint32_t next_label;
    int errors;
} Lower;

static void fail(Lower *L, SrcPos pos, const char *what) {
    fprintf(stderr, "%s:%d:%d: lowering: %s\n", pos.file != NULL ? pos.file : "<input>", pos.line,
            pos.col, what);
    L->errors++;
}

#define GROW(L, field, count, cap, type)                                                           \
    do {                                                                                           \
        if ((L)->count == (L)->cap) {                                                              \
            size_t nc = (L)->cap == 0 ? 16 : (L)->cap * 2;                                         \
            type *g = realloc((L)->field, nc * sizeof(type));                                      \
            if (g == NULL) {                                                                       \
                fprintf(stderr, "slop: out of memory\n");                                          \
                exit(1);                                                                           \
            }                                                                                      \
            (L)->field = g;                                                                        \
            (L)->cap = nc;                                                                         \
        }                                                                                          \
    } while (0)

static int view_eq(StrView a, StrView b) {
    return a.len == b.len && a.data != NULL && b.data != NULL &&
           memcmp(a.data, b.data, (size_t)a.len) == 0;
}

/* Arena copy of a StrView as a C string, for the names the IR wants. */
static const char *cstr(Lower *L, StrView v) {
    return arena_strndup(L->arena, v.data, (size_t)v.len);
}

/* slop lets an inner block shadow an outer name, but a function's IR value
 * names must be unique, so a repeat gets a suffix -- `%x`, then `%x.1`. */
static const char *unique_name(Lower *L, const char *base) {
    char buf[160];
    size_t i;
    unsigned n = 0;

    snprintf(buf, sizeof buf, "%s", base);
    for (;;) {
        int taken = 0;
        for (i = 0; i < L->nused; i++) {
            if (strcmp(L->used_names[i], buf) == 0) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            break;
        }
        snprintf(buf, sizeof buf, "%s.%u", base, ++n);
    }

    GROW(L, used_names, nused, cused, char *);
    L->used_names[L->nused] = arena_strndup(L->arena, buf, strlen(buf));
    return L->used_names[L->nused++];
}

static char *label_name(Lower *L, const char *stem) {
    char buf[32];
    snprintf(buf, sizeof buf, "%s%u", stem, L->next_label++);
    return arena_strndup(L->arena, buf, strlen(buf));
}

/* ----------------------------------------------------------------- types */

static IrType *ir_of_ast_type(Lower *L, const Type *t);

static NamedType *find_named(Lower *L, StrView name) {
    size_t i;
    for (i = 0; i < L->ntypes; i++) {
        if (view_eq(L->types[i].name, name)) {
            return &L->types[i];
        }
    }
    return NULL;
}

static IrType *ir_of_core(Lower *L, TokenKind k) {
    switch (k) {
    case TOK_KW_I8:
    case TOK_KW_U8:
    case TOK_KW_B8: return ir_type_int(L->m, 8);
    case TOK_KW_I32:
    case TOK_KW_U32:
    case TOK_KW_B32: return ir_type_int(L->m, 32);
    case TOK_KW_I64:
    case TOK_KW_U64:
    case TOK_KW_B64: return ir_type_int(L->m, 64);
    case TOK_KW_F32: return ir_type_float(L->m, 32);
    case TOK_KW_F64: return ir_type_float(L->m, 64);
    default: return ir_type_void(L->m);
    }
}

static IrType *ir_of_ast_type(Lower *L, const Type *t) {
    if (t == NULL) {
        return ir_type_void(L->m);
    }
    switch (t->kind) {
    case TYPE_CORE: return ir_of_core(L, t->core);
    case TYPE_POINTER: return ir_type_ptr(L->m);
    case TYPE_ARRAY: return ir_type_array(L->m, ir_of_ast_type(L, t->elem), t->length);
    case TYPE_NAME: {
        NamedType *nt = find_named(L, t->name);
        return nt != NULL ? nt->type : ir_type_void(L->m);
    }
    }
    return ir_type_void(L->m);
}

/* The resolved type of an expression. Structs come back as the same IrType
 * the declaration produced, because both look the name up in one table. */
static IrType *ir_of_ty(Lower *L, const Ty *t) {
    if (t == NULL) {
        return ir_type_void(L->m);
    }
    switch (t->kind) {
    case TY_VOID: return ir_type_void(L->m);
    case TY_I8:
    case TY_U8:
    case TY_B8: return ir_type_int(L->m, 8);
    case TY_I32:
    case TY_U32:
    case TY_B32:
    case TY_UNTYPED_BOOL: return ir_type_int(L->m, 32);
    case TY_I64:
    case TY_U64:
    case TY_B64:
    case TY_UNTYPED_INT: return ir_type_int(L->m, 64);
    case TY_F32: return ir_type_float(L->m, 32);
    case TY_F64:
    case TY_UNTYPED_FLOAT: return ir_type_float(L->m, 64);
    case TY_PTR:
    case TY_NULL: return ir_type_ptr(L->m);
    case TY_ARRAY: return ir_type_array(L->m, ir_of_ty(L, t->elem), t->length);
    case TY_ENUM: return ir_of_ty(L, t->base);
    case TY_STRUCT: {
        NamedType *nt = find_named(L, t->name);
        return nt != NULL ? nt->type : ir_type_void(L->m);
    }
    default: return ir_type_void(L->m);
    }
}

static int ty_is_aggregate(const Ty *t) {
    return t != NULL && (t->kind == TY_STRUCT || t->kind == TY_ARRAY);
}

/* --------------------------------------------------------------- lookups */

static Binding *find_local(Lower *L, StrView name) {
    size_t i = L->nlocals;
    while (i > 0) { /* innermost first: a local shadows a global */
        i--;
        if (view_eq(L->locals[i].name, name)) {
            return &L->locals[i];
        }
    }
    return NULL;
}

static Binding *find_global(Lower *L, StrView name) {
    size_t i;
    for (i = 0; i < L->nglobals; i++) {
        if (view_eq(L->globals[i].name, name)) {
            return &L->globals[i];
        }
    }
    return NULL;
}

static IrFunction *find_func(Lower *L, StrView name) {
    size_t i;
    for (i = 0; i < L->nfuncs; i++) {
        if (view_eq(L->funcs[i].name, name)) {
            return L->funcs[i].fn;
        }
    }
    return NULL;
}

/* An enum member is a global constant of its enum type, and sema left the
 * numbering in the type rather than the tree, so this is where the value
 * comes from. */
static int enum_member_value(const Ty *ty, StrView name, uint64_t *out) {
    uint32_t i;
    if (ty == NULL || ty->kind != TY_ENUM) {
        return 0;
    }
    for (i = 0; i < ty->nmembers; i++) {
        if (view_eq(ty->members[i].name, name)) {
            *out = ty->members[i].value;
            return 1;
        }
    }
    return 0;
}

static void push_local(Lower *L, StrView name, IrValue *addr, IrType *type, const Ty *ty) {
    GROW(L, locals, nlocals, clocals, Binding);
    L->locals[L->nlocals].name = name;
    L->locals[L->nlocals].addr = addr;
    L->locals[L->nlocals].type = type;
    L->locals[L->nlocals].ty = ty;
    L->nlocals++;
}

/* -------------------------------------------------------------- runtime */

/* memcpy and memset are declared on demand: a program that never copies an
 * aggregate should not carry a declaration for one. */
static IrFunction *runtime_memcpy(Lower *L) {
    if (L->memcpy_fn == NULL) {
        IrType *params[3];
        params[0] = ir_type_ptr(L->m);
        params[1] = ir_type_ptr(L->m);
        params[2] = ir_type_int(L->m, 64);
        L->memcpy_fn = ir_function_new(L->m, "memcpy", ir_type_ptr(L->m), params, 3, 0, 1);
    }
    return L->memcpy_fn;
}

static IrFunction *runtime_memset(Lower *L) {
    if (L->memset_fn == NULL) {
        IrType *params[3];
        params[0] = ir_type_ptr(L->m);
        params[1] = ir_type_int(L->m, 32);
        params[2] = ir_type_int(L->m, 64);
        L->memset_fn = ir_function_new(L->m, "memset", ir_type_ptr(L->m), params, 3, 0, 1);
    }
    return L->memset_fn;
}

static void emit_memcpy(Lower *L, IrValue *dst, IrValue *src, uint32_t size) {
    IrValue *args[3];
    args[0] = dst;
    args[1] = src;
    args[2] = ir_const_int(L->m, ir_type_int(L->m, 64), size);
    ir_build_call(&L->b, runtime_memcpy(L), args, 3);
}

static void emit_zero(Lower *L, IrValue *dst, uint32_t size) {
    IrValue *args[3];
    args[0] = dst;
    args[1] = ir_const_int(L->m, ir_type_int(L->m, 32), 0);
    args[2] = ir_const_int(L->m, ir_type_int(L->m, 64), size);
    ir_build_call(&L->b, runtime_memset(L), args, 3);
}

/* --------------------------------------------------------- expressions */

static IrValue *lower_expr(Lower *L, Expr *e);
static IrValue *lower_addr(Lower *L, Expr *e);
static void lower_stmt(Lower *L, Stmt *s);
static void lower_block(Lower *L, Block *b);

/* A condition is a bN in slop but an i1 in the IR. */
static IrValue *to_i1(Lower *L, IrValue *v) {
    if (v->type->kind == IR_TY_INT && v->type->bits == 1) {
        return v;
    }
    return ir_build_icmp(&L->b, IR_ICMP_NE, v, ir_const_int(L->m, v->type, 0));
}

/* ...and a comparison is the other way round: i1 out of the IR, bN in slop. */
static IrValue *from_i1(Lower *L, IrValue *v, IrType *want) {
    if (want->kind != IR_TY_INT || want->bits == 1) {
        return v;
    }
    return ir_build_cast(&L->b, IR_ZEXT, v, want);
}

static IrValue *zero_of(Lower *L, IrType *t) {
    if (t->kind == IR_TY_FLOAT) {
        return ir_const_fp(L->m, t, 0.0);
    }
    if (t->kind == IR_TY_PTR) {
        return ir_const_null(L->m);
    }
    return ir_const_int(L->m, t, 0);
}

/* An untyped literal adopts its context type (GRAMMAR.md section 6), and
 * sema records that on the literal rather than wrapping it in a conversion.
 * The IR is stricter than slop here — `add i64 %x, i32 1` is not a thing —
 * so every place that pairs two values has to agree on a width first. */
static IrValue *coerce_to(Lower *L, IrValue *v, IrType *want, const Ty *from) {
    if (want == NULL || v->type == want) {
        return v;
    }
    if (v->type->kind == IR_TY_INT && want->kind == IR_TY_INT) {
        if (v->type->bits == want->bits) {
            return v;
        }
        if (v->type->bits > want->bits) {
            return ir_build_cast(&L->b, IR_TRUNC, v, want);
        }
        return ir_build_cast(&L->b, from != NULL && ty_is_signed(from) ? IR_SEXT : IR_ZEXT, v, want);
    }
    if (v->type->kind == IR_TY_FLOAT && want->kind == IR_TY_FLOAT && v->type->bits != want->bits) {
        return ir_build_cast(&L->b, v->type->bits > want->bits ? IR_FPTRUNC : IR_FPEXT, v, want);
    }
    return v;
}

static IrValue *lower_conversion(Lower *L, IrValue *v, const Ty *from, const Ty *to, SrcPos pos) {
    IrType *dst = ir_of_ty(L, to);
    IrType *src = v->type;

    if (to == NULL || from == NULL) {
        return v;
    }
    if (to->kind == TY_PTR || to->kind == TY_NULL) {
        if (src->kind == IR_TY_PTR) {
            return v; /* opaque pointers: every pointer cast is a no-op */
        }
        return ir_build_cast(&L->b, IR_INTTOPTR, v, dst);
    }
    if (src->kind == IR_TY_PTR && dst->kind == IR_TY_INT) {
        return ir_build_cast(&L->b, IR_PTRTOINT, v, dst);
    }
    if (src->kind == IR_TY_INT && dst->kind == IR_TY_INT) {
        if (src->bits == dst->bits) {
            return v;
        }
        if (src->bits > dst->bits) {
            return ir_build_cast(&L->b, IR_TRUNC, v, dst);
        }
        /* Widening follows the *source*: an i8 sign-extends, a u8 does not. */
        return ir_build_cast(&L->b, ty_is_signed(from) ? IR_SEXT : IR_ZEXT, v, dst);
    }
    if (src->kind == IR_TY_INT && dst->kind == IR_TY_FLOAT) {
        return ir_build_cast(&L->b, ty_is_signed(from) ? IR_SITOFP : IR_UITOFP, v, dst);
    }
    if (src->kind == IR_TY_FLOAT && dst->kind == IR_TY_INT) {
        return ir_build_cast(&L->b, ty_is_signed(to) ? IR_FPTOSI : IR_FPTOUI, v, dst);
    }
    if (src->kind == IR_TY_FLOAT && dst->kind == IR_TY_FLOAT) {
        if (src->bits == dst->bits) {
            return v;
        }
        return ir_build_cast(&L->b, src->bits > dst->bits ? IR_FPTRUNC : IR_FPEXT, v, dst);
    }
    if (src == dst) {
        return v;
    }
    fail(L, pos, "unsupported conversion");
    return v;
}

static IrOpcode int_binop(TokenKind op, int is_signed) {
    switch (op) {
    case TOK_PLUS: return IR_ADD;
    case TOK_MINUS: return IR_SUB;
    case TOK_STAR: return IR_MUL;
    case TOK_SLASH: return is_signed ? IR_SDIV : IR_UDIV;
    case TOK_PERCENT: return is_signed ? IR_SREM : IR_UREM;
    case TOK_AMP: return IR_AND;
    case TOK_PIPE: return IR_OR;
    case TOK_CARET: return IR_XOR;
    case TOK_SHL: return IR_SHL;
    case TOK_SHR: return is_signed ? IR_ASHR : IR_LSHR;
    default: return IR_ADD;
    }
}

static IrOpcode float_binop(TokenKind op) {
    switch (op) {
    case TOK_PLUS: return IR_FADD;
    case TOK_MINUS: return IR_FSUB;
    case TOK_STAR: return IR_FMUL;
    case TOK_SLASH: return IR_FDIV;
    default: return IR_FREM;
    }
}

static IrPred int_pred(TokenKind op, int is_signed) {
    switch (op) {
    case TOK_EQ_EQ: return IR_ICMP_EQ;
    case TOK_BANG_EQ: return IR_ICMP_NE;
    case TOK_LT: return is_signed ? IR_ICMP_SLT : IR_ICMP_ULT;
    case TOK_LE: return is_signed ? IR_ICMP_SLE : IR_ICMP_ULE;
    case TOK_GT: return is_signed ? IR_ICMP_SGT : IR_ICMP_UGT;
    default: return is_signed ? IR_ICMP_SGE : IR_ICMP_UGE;
    }
}

static IrPred float_pred(TokenKind op) {
    switch (op) {
    case TOK_EQ_EQ: return IR_FCMP_OEQ;
    case TOK_BANG_EQ: return IR_FCMP_ONE;
    case TOK_LT: return IR_FCMP_OLT;
    case TOK_LE: return IR_FCMP_OLE;
    case TOK_GT: return IR_FCMP_OGT;
    default: return IR_FCMP_OGE;
    }
}

static int is_comparison(TokenKind op) {
    return op == TOK_EQ_EQ || op == TOK_BANG_EQ || op == TOK_LT || op == TOK_LE || op == TOK_GT ||
           op == TOK_GE;
}

/* p + i and p - i scale by the pointee, which is what getelementptr is for. */
static IrValue *lower_ptr_arith(Lower *L, Expr *e, IrValue *base, IrValue *idx) {
    const Ty *pt = e->u.binary.lhs->sem_type;
    IrType *elem = ir_of_ty(L, pt->elem);
    IrType *i64 = ir_type_int(L->m, 64);

    if (idx->type != i64) {
        idx = ir_build_cast(&L->b, idx->type->bits > 64 ? IR_TRUNC : IR_SEXT, idx, i64);
    }
    if (e->u.binary.op == TOK_MINUS) {
        idx = ir_build_binary(&L->b, IR_SUB, ir_const_int(L->m, i64, 0), idx);
    }
    return ir_build_gep_index(&L->b, elem, base, idx);
}

/* && and || short-circuit, and the IR has no phi, so the result goes through
 * a slot: the branch that ran is the one that wrote it. */
static IrValue *lower_short_circuit(Lower *L, Expr *e) {
    IrType *rt = ir_of_ty(L, e->sem_type);
    IrValue *slot = ir_build_alloca(&L->b, rt, label_name(L, "sc"));
    IrBlock *rhs_bb = ir_block_new(L->cur, label_name(L, "sc.rhs"));
    IrBlock *end_bb = ir_block_new(L->cur, label_name(L, "sc.end"));
    IrValue *lhs = lower_expr(L, e->u.binary.lhs);
    int is_and = e->u.binary.op == TOK_AMP_AMP;
    IrValue *rhs;

    ir_build_store(&L->b, lhs, slot);
    if (is_and) {
        ir_build_condbr(&L->b, to_i1(L, lhs), rhs_bb, end_bb);
    } else {
        ir_build_condbr(&L->b, to_i1(L, lhs), end_bb, rhs_bb);
    }

    ir_builder_position(&L->b, rhs_bb);
    rhs = lower_expr(L, e->u.binary.rhs);
    ir_build_store(&L->b, rhs, slot);
    ir_build_br(&L->b, end_bb);

    ir_builder_position(&L->b, end_bb);
    return ir_build_load(&L->b, rt, slot);
}

static IrValue *lower_binary(Lower *L, Expr *e) {
    TokenKind op = e->u.binary.op;
    const Ty *lt = e->u.binary.lhs->sem_type;
    const Ty *rt = e->u.binary.rhs->sem_type;
    IrValue *lhs, *rhs;

    if (op == TOK_AMP_AMP || op == TOK_PIPE_PIPE) {
        return lower_short_circuit(L, e);
    }

    lhs = lower_expr(L, e->u.binary.lhs);
    rhs = lower_expr(L, e->u.binary.rhs);

    /* Whichever side is untyped takes the other's width; when both are typed
     * sema already made them agree. */
    if (lhs->type != rhs->type && (lt == NULL || !ty_is_untyped(lt))) {
        rhs = coerce_to(L, rhs, lhs->type, rt);
    } else if (lhs->type != rhs->type) {
        lhs = coerce_to(L, lhs, rhs->type, lt);
    }

    if (is_comparison(op)) {
        IrValue *cmp;
        if (lt != NULL && ty_is_float(lt)) {
            cmp = ir_build_fcmp(&L->b, float_pred(op), lhs, rhs);
        } else {
            cmp = ir_build_icmp(&L->b, int_pred(op, lt != NULL && ty_is_signed(lt)), lhs, rhs);
        }
        return from_i1(L, cmp, ir_of_ty(L, e->sem_type));
    }

    if (lt != NULL && lt->kind == TY_PTR) {
        if (rt != NULL && rt->kind == TY_PTR) {
            /* p - q is a count of elements, not of bytes. */
            IrType *i64 = ir_type_int(L->m, 64);
            IrValue *a = ir_build_cast(&L->b, IR_PTRTOINT, lhs, i64);
            IrValue *b2 = ir_build_cast(&L->b, IR_PTRTOINT, rhs, i64);
            IrValue *diff = ir_build_binary(&L->b, IR_SUB, a, b2);
            uint32_t size = ir_type_size(ir_of_ty(L, lt->elem));
            return ir_build_binary(&L->b, IR_SDIV, diff,
                                   ir_const_int(L->m, i64, size != 0 ? size : 1));
        }
        return lower_ptr_arith(L, e, lhs, rhs);
    }

    if (lt != NULL && ty_is_float(lt)) {
        return ir_build_binary(&L->b, float_binop(op), lhs, rhs);
    }
    return ir_build_binary(&L->b, int_binop(op, lt != NULL && ty_is_signed(lt)), lhs, rhs);
}

static IrValue *lower_unary(Lower *L, Expr *e) {
    TokenKind op = e->u.unary.op;
    Expr *operand = e->u.unary.operand;
    IrType *rt = ir_of_ty(L, e->sem_type);
    IrValue *v;

    if (op == TOK_AMP) {
        return lower_addr(L, operand);
    }
    if (op == TOK_STAR) {
        return ir_build_load(&L->b, rt, lower_expr(L, operand));
    }

    v = lower_expr(L, operand);
    switch (op) {
    case TOK_MINUS:
        if (ty_is_float(operand->sem_type)) {
            return ir_build_binary(&L->b, IR_FSUB, ir_const_fp(L->m, v->type, 0.0), v);
        }
        return ir_build_binary(&L->b, IR_SUB, ir_const_int(L->m, v->type, 0), v);
    case TOK_TILDE:
        return ir_build_binary(&L->b, IR_XOR, v, ir_const_int(L->m, v->type, UINT64_MAX));
    case TOK_BANG:
        return from_i1(L, ir_build_icmp(&L->b, IR_ICMP_EQ, v, zero_of(L, v->type)), rt);
    default:
        fail(L, e->pos, "unsupported unary operator");
        return v;
    }
}

/* The address of an lvalue. `p.f` auto-derefs one level of pointer, which is
 * the only place the two spellings of member access differ. */
static IrValue *lower_addr(Lower *L, Expr *e) {
    switch (e->kind) {
    case EXPR_NAME: {
        Binding *b = find_local(L, e->u.sval);
        if (b == NULL) {
            b = find_global(L, e->u.sval);
        }
        if (b == NULL) {
            fail(L, e->pos, "no storage for this name");
            return ir_undef(L->m, ir_type_ptr(L->m));
        }
        return b->addr;
    }
    case EXPR_UNARY:
        if (e->u.unary.op == TOK_STAR) {
            return lower_expr(L, e->u.unary.operand);
        }
        break;
    case EXPR_INDEX: {
        Expr *base = e->u.index.base;
        const Ty *bt = base->sem_type;
        IrValue *ptr;
        IrValue *idx = lower_expr(L, e->u.index.index);
        IrType *i64 = ir_type_int(L->m, 64);
        IrType *elem;

        if (bt != NULL && bt->kind == TY_PTR) {
            ptr = lower_expr(L, base); /* the pointer is the base itself */
            elem = ir_of_ty(L, bt->elem);
        } else if (bt != NULL && bt->kind == TY_ARRAY) {
            ptr = lower_addr(L, base); /* an array decays to where it lives */
            elem = ir_of_ty(L, bt->elem);
        } else {
            fail(L, e->pos, "indexing something that is neither array nor pointer");
            return ir_undef(L->m, ir_type_ptr(L->m));
        }
        if (idx->type != i64) {
            idx = ir_build_cast(&L->b, ty_is_signed(e->u.index.index->sem_type) ? IR_SEXT : IR_ZEXT,
                                idx, i64);
        }
        return ir_build_gep_index(&L->b, elem, ptr, idx);
    }
    case EXPR_FIELD: {
        Expr *base = e->u.field.base;
        const Ty *bt = base->sem_type;
        const Ty *st = bt;
        IrValue *ptr;
        const FieldSym *f;
        uint32_t i, index = 0;

        if (bt != NULL && bt->kind == TY_PTR) {
            st = bt->elem;
            ptr = lower_expr(L, base);
        } else {
            ptr = lower_addr(L, base);
        }
        f = st != NULL ? ty_field(st, e->u.field.name) : NULL;
        if (f == NULL) {
            fail(L, e->pos, "no such field");
            return ir_undef(L->m, ir_type_ptr(L->m));
        }
        for (i = 0; i < st->nfields; i++) {
            if (&st->fields[i] == f) {
                index = i;
                break;
            }
        }
        return ir_build_gep_field(&L->b, ir_of_ty(L, st), ptr, index);
    }
    case EXPR_CONV:
        /* A conversion between pointers changes nothing about where the
         * object is. */
        return lower_addr(L, e->u.conv.operand);
    default: break;
    }
    fail(L, e->pos, "not an lvalue");
    return ir_undef(L->m, ir_type_ptr(L->m));
}

static IrValue *lower_call(Lower *L, Expr *e) {
    Expr *callee = e->u.call.callee;
    IrFunction *fn;
    IrValue **args;
    uint32_t i;
    IrValue *result;

    if (callee->kind != EXPR_NAME) {
        fail(L, e->pos, "only direct calls exist in v0");
        return ir_undef(L->m, ir_of_ty(L, e->sem_type));
    }
    fn = find_func(L, callee->u.sval);
    if (fn == NULL) {
        fail(L, e->pos, "call to an unknown function");
        return ir_undef(L->m, ir_of_ty(L, e->sem_type));
    }

    args = calloc(e->u.call.nargs != 0 ? e->u.call.nargs : 1, sizeof(IrValue *));
    if (args == NULL) {
        fprintf(stderr, "slop: out of memory\n");
        exit(1);
    }
    for (i = 0; i < e->u.call.nargs; i++) {
        args[i] = lower_expr(L, e->u.call.args[i]);
        if (i < fn->nparams) {
            args[i] = coerce_to(L, args[i], fn->params[i]->type, e->u.call.args[i]->sem_type);
        }
    }
    result = ir_build_call(&L->b, fn, args, e->u.call.nargs);
    free(args);
    return result;
}

static IrValue *lower_expr(Lower *L, Expr *e) {
    IrType *rt;

    if (e == NULL) {
        return ir_undef(L->m, ir_type_void(L->m));
    }
    rt = ir_of_ty(L, e->sem_type);

    switch (e->kind) {
    case EXPR_INT:
    case EXPR_CHAR:
        return ir_const_int(L->m, rt, e->u.ival);
    case EXPR_BOOL:
        return ir_const_int(L->m, rt, e->u.bval != 0 ? 1u : 0u);
    case EXPR_FLOAT:
        return ir_const_fp(L->m, rt, e->u.fval);
    case EXPR_NULL:
        return ir_const_null(L->m);
    case EXPR_STRING:
        return ir_string(L->m, e->u.sval.data, (uint32_t)e->u.sval.len);
    case EXPR_SIZEOF:
        return ir_const_int(L->m, rt, ir_type_size(ir_of_ast_type(L, e->u.size_of.type)));

    case EXPR_NAME: {
        Binding *b = find_local(L, e->u.sval);
        uint64_t member;
        if (b == NULL) {
            b = find_global(L, e->u.sval);
        }
        if (b != NULL) {
            /* An array's value *is* its address: there is nothing to load. */
            if (ty_is_aggregate(e->sem_type) && e->sem_type->kind == TY_ARRAY) {
                return b->addr;
            }
            return ir_build_load(&L->b, b->type, b->addr);
        }
        if (enum_member_value(e->sem_type, e->u.sval, &member)) {
            return ir_const_int(L->m, rt, member);
        }
        fail(L, e->pos, "unresolved name");
        return ir_undef(L->m, rt);
    }

    case EXPR_UNARY: return lower_unary(L, e);
    case EXPR_BINARY: return lower_binary(L, e);
    case EXPR_CALL: return lower_call(L, e);

    case EXPR_INDEX:
    case EXPR_FIELD: {
        IrValue *addr = lower_addr(L, e);
        if (ty_is_aggregate(e->sem_type)) {
            return addr; /* aggregates are handled by address throughout */
        }
        return ir_build_load(&L->b, rt, addr);
    }

    case EXPR_CAST:
        /* Sema rewrites `.as(T)` into EXPR_CONV, so this is only reachable if
         * one slipped through; treat it the same way. */
        return lower_conversion(L, lower_expr(L, e->u.cast.operand), e->u.cast.operand->sem_type,
                                e->sem_type, e->pos);

    case EXPR_CONV:
        return lower_conversion(L, lower_expr(L, e->u.conv.operand), e->u.conv.operand->sem_type,
                                e->sem_type, e->pos);
    }

    fail(L, e->pos, "unsupported expression");
    return ir_undef(L->m, rt);
}

/* ---------------------------------------------------------- statements */

/* After a terminator the rest of a block is unreachable, but every block
 * still has to end in one, so open a fresh one and keep going. */
static void ensure_open(Lower *L) {
    if (ir_block_is_terminated(L->b.block)) {
        ir_builder_position(&L->b, ir_block_new(L->cur, label_name(L, "dead")));
    }
}

static void store_value(Lower *L, IrValue *addr, Expr *value, const Ty *ty) {
    if (ty_is_aggregate(ty)) {
        emit_memcpy(L, addr, lower_expr(L, value), ty->size);
        return;
    }
    ir_build_store(&L->b, coerce_to(L, lower_expr(L, value), ir_of_ty(L, ty), value->sem_type),
                   addr);
}

static void lower_var(Lower *L, Stmt *s) {
    IrType *t = ir_of_ast_type(L, s->u.var.type);
    IrValue *slot = ir_build_alloca(&L->b, t, unique_name(L, cstr(L, s->u.var.name)));
    const Ty *ty = s->u.var.init != NULL ? s->u.var.init->sem_type : NULL;

    if (s->u.var.init != NULL) {
        if (ty_is_aggregate(ty)) {
            emit_memcpy(L, slot, lower_expr(L, s->u.var.init), ty->size);
        } else {
            ir_build_store(&L->b, coerce_to(L, lower_expr(L, s->u.var.init), t, ty), slot);
        }
    } else if (t->kind == IR_TY_ARRAY || t->kind == IR_TY_STRUCT) {
        /* "Anything declared without an initializer is zeroed" — GRAMMAR.md
         * section 6, and it applies to aggregates too. */
        emit_zero(L, slot, ir_type_size(t));
    } else {
        ir_build_store(&L->b, zero_of(L, t), slot);
    }
    push_local(L, s->u.var.name, slot, t, ty);
}

static void lower_assign(Lower *L, Stmt *s) {
    IrValue *addr = lower_addr(L, s->u.assign.target);
    const Ty *tt = s->u.assign.target->sem_type;
    IrType *t = ir_of_ty(L, tt);
    TokenKind op = s->u.assign.op;
    IrValue *cur, *rhs, *result;

    if (op == TOK_EQ) {
        store_value(L, addr, s->u.assign.value, tt);
        return;
    }

    cur = ir_build_load(&L->b, t, addr);
    rhs = lower_expr(L, s->u.assign.value);
    if (tt == NULL || tt->kind != TY_PTR) {
        rhs = coerce_to(L, rhs, t, s->u.assign.value->sem_type);
    }

    if (tt != NULL && tt->kind == TY_PTR) {
        /* p += n scales like p + n does. */
        IrType *i64 = ir_type_int(L->m, 64);
        IrType *elem = ir_of_ty(L, tt->elem);
        if (rhs->type != i64) {
            rhs = ir_build_cast(&L->b, IR_SEXT, rhs, i64);
        }
        if (op == TOK_MINUS_EQ) {
            rhs = ir_build_binary(&L->b, IR_SUB, ir_const_int(L->m, i64, 0), rhs);
        }
        result = ir_build_gep_index(&L->b, elem, cur, rhs);
    } else {
        TokenKind base;
        switch (op) {
        case TOK_PLUS_EQ: base = TOK_PLUS; break;
        case TOK_MINUS_EQ: base = TOK_MINUS; break;
        case TOK_STAR_EQ: base = TOK_STAR; break;
        case TOK_SLASH_EQ: base = TOK_SLASH; break;
        case TOK_PERCENT_EQ: base = TOK_PERCENT; break;
        case TOK_AMP_EQ: base = TOK_AMP; break;
        case TOK_PIPE_EQ: base = TOK_PIPE; break;
        case TOK_CARET_EQ: base = TOK_CARET; break;
        case TOK_SHL_EQ: base = TOK_SHL; break;
        default: base = TOK_SHR; break;
        }
        if (tt != NULL && ty_is_float(tt)) {
            result = ir_build_binary(&L->b, float_binop(base), cur, rhs);
        } else {
            result = ir_build_binary(&L->b, int_binop(base, tt != NULL && ty_is_signed(tt)), cur,
                                     rhs);
        }
    }
    ir_build_store(&L->b, result, addr);
}

static void lower_if(Lower *L, Stmt *s) {
    IrBlock *then_bb = ir_block_new(L->cur, label_name(L, "if.then"));
    IrBlock *else_bb = s->u.if_stmt.else_stmt != NULL
                           ? ir_block_new(L->cur, label_name(L, "if.else"))
                           : NULL;
    IrBlock *end_bb = ir_block_new(L->cur, label_name(L, "if.end"));
    IrValue *cond = to_i1(L, lower_expr(L, s->u.if_stmt.cond));

    ir_build_condbr(&L->b, cond, then_bb, else_bb != NULL ? else_bb : end_bb);

    ir_builder_position(&L->b, then_bb);
    lower_block(L, &s->u.if_stmt.then_block);
    if (!ir_block_is_terminated(L->b.block)) {
        ir_build_br(&L->b, end_bb);
    }

    if (else_bb != NULL) {
        ir_builder_position(&L->b, else_bb);
        lower_stmt(L, s->u.if_stmt.else_stmt);
        if (!ir_block_is_terminated(L->b.block)) {
            ir_build_br(&L->b, end_bb);
        }
    }
    ir_builder_position(&L->b, end_bb);
}

static void lower_while(Lower *L, Stmt *s) {
    IrBlock *cond_bb = ir_block_new(L->cur, label_name(L, "while.cond"));
    IrBlock *body_bb = ir_block_new(L->cur, label_name(L, "while.body"));
    IrBlock *end_bb = ir_block_new(L->cur, label_name(L, "while.end"));

    ir_build_br(&L->b, cond_bb);
    ir_builder_position(&L->b, cond_bb);
    ir_build_condbr(&L->b, to_i1(L, lower_expr(L, s->u.while_stmt.cond)), body_bb, end_bb);

    GROW(L, loops, nloops, cloops, Loop);
    L->loops[L->nloops].continue_to = cond_bb;
    L->loops[L->nloops].break_to = end_bb;
    L->nloops++;

    ir_builder_position(&L->b, body_bb);
    lower_block(L, &s->u.while_stmt.body);
    if (!ir_block_is_terminated(L->b.block)) {
        ir_build_br(&L->b, cond_bb);
    }
    L->nloops--;

    ir_builder_position(&L->b, end_bb);
}

/* A match is a chain of equality tests. There is no fallthrough, and a value
 * matching no arm does nothing, so the chain simply ends at the exit. */
static void lower_match(Lower *L, Stmt *s) {
    IrValue *scrutinee = lower_expr(L, s->u.match.scrutinee);
    IrBlock *end_bb = ir_block_new(L->cur, label_name(L, "match.end"));
    IrBlock *wildcard_bb = NULL;
    uint32_t a, l;

    for (a = 0; a < s->u.match.narms; a++) {
        MatchArm *arm = &s->u.match.arms[a];
        IrBlock *body_bb = ir_block_new(L->cur, label_name(L, "match.arm"));
        int is_wildcard = 0;

        for (l = 0; l < arm->nlabels; l++) {
            if (arm->labels[l].is_wildcard) {
                is_wildcard = 1;
            }
        }
        if (is_wildcard) {
            wildcard_bb = body_bb;
        } else {
            for (l = 0; l < arm->nlabels; l++) {
                Expr *label = arm->labels[l].value;
                IrBlock *next_bb = ir_block_new(L->cur, label_name(L, "match.test"));
                IrValue *v = lower_expr(L, label);
                ir_build_condbr(&L->b, ir_build_icmp(&L->b, IR_ICMP_EQ, scrutinee, v), body_bb,
                                next_bb);
                ir_builder_position(&L->b, next_bb);
            }
        }

        {
            /* Lower the body away from the test chain, then come back to
             * where the chain left off. */
            IrBlock *resume = L->b.block;
            size_t mark = L->nlocals;
            ir_builder_position(&L->b, body_bb);
            lower_stmt(L, arm->body);
            if (!ir_block_is_terminated(L->b.block)) {
                ir_build_br(&L->b, end_bb);
            }
            L->nlocals = mark;
            ir_builder_position(&L->b, resume);
        }
    }

    ir_build_br(&L->b, wildcard_bb != NULL ? wildcard_bb : end_bb);
    ir_builder_position(&L->b, end_bb);
}

static void lower_stmt(Lower *L, Stmt *s) {
    if (s == NULL) {
        return;
    }
    ensure_open(L);

    switch (s->kind) {
    case STMT_BLOCK: {
        size_t mark = L->nlocals;
        lower_block(L, &s->u.block);
        L->nlocals = mark;
        break;
    }
    case STMT_VAR: lower_var(L, s); break;
    case STMT_IF: lower_if(L, s); break;
    case STMT_WHILE: lower_while(L, s); break;
    case STMT_MATCH: lower_match(L, s); break;
    case STMT_ASSIGN: lower_assign(L, s); break;
    case STMT_EXPR: (void)lower_expr(L, s->u.expr.expr); break;
    case STMT_EMPTY: break;

    case STMT_RETURN:
        if (s->u.ret.value == NULL) {
            ir_build_ret(&L->b, NULL);
        } else {
            ir_build_ret(&L->b, coerce_to(L, lower_expr(L, s->u.ret.value), L->cur->ret,
                                          s->u.ret.value->sem_type));
        }
        break;

    case STMT_BREAK:
        if (L->nloops == 0) {
            fail(L, s->pos, "break outside a loop");
            break;
        }
        ir_build_br(&L->b, L->loops[L->nloops - 1].break_to);
        break;

    case STMT_CONTINUE:
        if (L->nloops == 0) {
            fail(L, s->pos, "continue outside a loop");
            break;
        }
        ir_build_br(&L->b, L->loops[L->nloops - 1].continue_to);
        break;
    }
}

static void lower_block(Lower *L, Block *b) {
    size_t mark = L->nlocals;
    uint32_t i;
    for (i = 0; i < b->nstmts; i++) {
        lower_stmt(L, b->stmts[i]);
    }
    L->nlocals = mark; /* block scoping is just a stack mark */
}

/* --------------------------------------------------------------- items */

static void declare_function(Lower *L, FnDecl *fn) {
    IrType **params = NULL;
    IrFunction *f;
    uint32_t i;

    if (fn->nparams != 0) {
        params = calloc(fn->nparams, sizeof(IrType *));
        if (params == NULL) {
            fprintf(stderr, "slop: out of memory\n");
            exit(1);
        }
        for (i = 0; i < fn->nparams; i++) {
            params[i] = ir_of_ast_type(L, fn->params[i].type);
        }
    }

    f = ir_function_new(L->m, cstr(L, fn->name), ir_of_ast_type(L, fn->ret), params, fn->nparams,
                        fn->is_variadic, !fn->has_body);
    for (i = 0; i < fn->nparams; i++) {
        ir_param_set_name(f, i, cstr(L, fn->params[i].name));
    }
    free(params);

    GROW(L, funcs, nfuncs, cfuncs, FuncSym);
    L->funcs[L->nfuncs].name = fn->name;
    L->funcs[L->nfuncs].fn = f;
    L->nfuncs++;
}

static void lower_function(Lower *L, FnDecl *fn, IrFunction *f) {
    uint32_t i;

    L->cur = f;
    L->nlocals = 0;
    L->nloops = 0;
    L->nused = 0; /* names are unique per function, not per module */
    ir_builder_position(&L->b, ir_block_new(f, "entry"));

    /* A parameter is a value, but slop lets you assign to a `mut` one, so it
     * gets a slot like any other local. mem2reg removes it again. */
    for (i = 0; i < fn->nparams; i++) {
        IrType *t = ir_of_ast_type(L, fn->params[i].type);
        /* `.addr`, as clang spells it: the slot cannot take the parameter's
         * own name, because the parameter already has it and neither LLVM
         * nor C tolerates the collision. */
        char slot_name[128];
        IrValue *slot;
        snprintf(slot_name, sizeof slot_name, "%.*s.addr", (int)fn->params[i].name.len,
                 fn->params[i].name.data);
        slot = ir_build_alloca(&L->b, t, unique_name(L, slot_name));
        ir_build_store(&L->b, f->params[i], slot);
        push_local(L, fn->params[i].name, slot, t, NULL);
    }

    lower_block(L, &fn->body);

    if (!ir_block_is_terminated(L->b.block)) {
        if (f->ret->kind == IR_TY_VOID) {
            ir_build_ret(&L->b, NULL);
        } else {
            /* Sema proved every path returns, so this is only reachable in
             * the block ensure_open() opened after one. */
            ir_build_unreachable(&L->b);
        }
    }
}

static IrValue *constant_of(Lower *L, Expr *e, IrType *t) {
    if (e == NULL) {
        return NULL; /* zeroinitializer */
    }
    switch (e->kind) {
    case EXPR_INT:
    case EXPR_CHAR: return ir_const_int(L->m, t, e->u.ival);
    case EXPR_BOOL: return ir_const_int(L->m, t, e->u.bval != 0 ? 1u : 0u);
    case EXPR_FLOAT: return ir_const_fp(L->m, t, e->u.fval);
    case EXPR_NULL: return ir_const_null(L->m);
    case EXPR_STRING: return ir_string(L->m, e->u.sval.data, (uint32_t)e->u.sval.len);
    case EXPR_CONV: return constant_of(L, e->u.conv.operand, t);
    default:
        fail(L, e->pos, "global initializers are literals in v0");
        return NULL;
    }
}

static void collect_types(Lower *L, Program *p) {
    uint32_t i;
    /* Two passes: a struct may hold a pointer to one declared later, so every
     * name has to exist before any body is filled in. */
    for (i = 0; i < p->nitems; i++) {
        Item *it = p->items[i];
        GROW(L, types, ntypes, ctypes, NamedType);
        if (it->kind == ITEM_STRUCT) {
            L->types[L->ntypes].name = it->u.struct_decl.name;
            L->types[L->ntypes].type = ir_type_struct(L->m, cstr(L, it->u.struct_decl.name));
            L->types[L->ntypes].decl = it;
            L->ntypes++;
        } else if (it->kind == ITEM_ENUM) {
            /* An enum is its underlying integer as far as the IR cares. */
            L->types[L->ntypes].name = it->u.enum_decl.name;
            L->types[L->ntypes].type = ir_of_core(L, it->u.enum_decl.base);
            L->types[L->ntypes].decl = it;
            L->ntypes++;
        }
    }
}

static void fill_struct_bodies(Lower *L, Program *p) {
    uint32_t i, j;
    for (i = 0; i < p->nitems; i++) {
        Item *it = p->items[i];
        NamedType *nt;
        IrType **fields;

        if (it->kind != ITEM_STRUCT) {
            continue;
        }
        nt = find_named(L, it->u.struct_decl.name);
        if (nt == NULL || nt->decl != it) {
            continue;
        }
        fields = calloc(it->u.struct_decl.nfields != 0 ? it->u.struct_decl.nfields : 1,
                        sizeof(IrType *));
        if (fields == NULL) {
            fprintf(stderr, "slop: out of memory\n");
            exit(1);
        }
        for (j = 0; j < it->u.struct_decl.nfields; j++) {
            fields[j] = ir_of_ast_type(L, it->u.struct_decl.fields[j].type);
        }
        ir_struct_set_body(L->m, nt->type, fields, it->u.struct_decl.nfields);
        free(fields);
    }
}

IrModule *lower_programs(Arena *arena, Program **programs, size_t nprograms) {
    Lower L;
    size_t pi;
    uint32_t i, j;
    size_t fn_cursor = 0;

    memset(&L, 0, sizeof L);
    L.arena = arena;
    L.m = ir_module_new(arena, nprograms != 0 ? programs[0]->file : "module");
    ir_builder_init(&L.b, L.m);

    for (pi = 0; pi < nprograms; pi++) {
        collect_types(&L, programs[pi]);
    }
    for (pi = 0; pi < nprograms; pi++) {
        fill_struct_bodies(&L, programs[pi]);
    }

    /* Globals before bodies, functions before calls: declaration order does
     * not matter in slop, so nothing may depend on the order of this walk. */
    for (pi = 0; pi < nprograms; pi++) {
        Program *p = programs[pi];
        for (i = 0; i < p->nitems; i++) {
            Item *it = p->items[i];
            if (it->kind != ITEM_GLOBAL) {
                continue;
            }
            {
                IrType *t = ir_of_ast_type(&L, it->u.global.type);
                IrValue *init = constant_of(&L, it->u.global.init, t);
                IrGlobal *g = ir_global_new(L.m, cstr(&L, it->u.global.name), t, init);

                /* A global's *value* is what lives at its address, so the
                 * binding holds the address, exactly like a local's alloca. */
                IrValue *addr = arena_alloc(arena, sizeof(IrValue));
                addr->kind = IRV_GLOBAL;
                addr->type = ir_type_ptr(L.m);
                addr->name = g->name;

                GROW(&L, globals, nglobals, cglobals, Binding);
                L.globals[L.nglobals].name = it->u.global.name;
                L.globals[L.nglobals].addr = addr;
                L.globals[L.nglobals].type = t;
                L.globals[L.nglobals].ty = NULL;
                L.nglobals++;
            }
        }
    }

    for (pi = 0; pi < nprograms; pi++) {
        Program *p = programs[pi];
        for (i = 0; i < p->nitems; i++) {
            Item *it = p->items[i];
            if (it->kind == ITEM_FN) {
                declare_function(&L, &it->u.fn);
            } else if (it->kind == ITEM_EXTERN) {
                for (j = 0; j < it->u.extern_block.nfns; j++) {
                    declare_function(&L, &it->u.extern_block.fns[j]);
                }
            }
        }
    }

    /* Bodies, in the same order the declarations were made, so the two walks
     * line up without a second lookup. */
    for (pi = 0; pi < nprograms; pi++) {
        Program *p = programs[pi];
        for (i = 0; i < p->nitems; i++) {
            Item *it = p->items[i];
            if (it->kind == ITEM_FN) {
                lower_function(&L, &it->u.fn, L.funcs[fn_cursor].fn);
                fn_cursor++;
            } else if (it->kind == ITEM_EXTERN) {
                fn_cursor += it->u.extern_block.nfns;
            }
        }
    }

    free(L.globals);
    free(L.locals);
    free(L.types);
    free(L.funcs);
    free(L.loops);
    free(L.used_names);

    if (L.errors != 0) {
        return NULL;
    }
    return L.m;
}
