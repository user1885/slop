#include "ir/ir.h"

#include <string.h>

/* ------------------------------------------------------------------ names */

static char *fmt_name(Arena *a, const char *prefix, uint32_t n) {
    char buf[32];
    size_t len;
    int i = 0;
    char digits[24];
    uint32_t v = n;

    do {
        digits[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);

    len = strlen(prefix);
    memcpy(buf, prefix, len);
    while (i > 0) {
        buf[len++] = digits[--i];
    }
    buf[len] = '\0';
    return arena_strndup(a, buf, len);
}

static const char *dup_cstr(Arena *a, const char *s) {
    return arena_strndup(a, s, strlen(s));
}

/* ------------------------------------------------------------------ types */

static IrType *type_new(IrModule *m, IrTypeKind kind) {
    IrType *t = arena_alloc(m->arena, sizeof(IrType));
    t->kind = kind;
    return t;
}

IrType *ir_type_void(IrModule *m) {
    return m->ty_void;
}

IrType *ir_type_int(IrModule *m, uint32_t bits) {
    switch (bits) {
    case 1: return m->ty_i1;
    case 8: return m->ty_i8;
    case 32: return m->ty_i32;
    case 64: return m->ty_i64;
    default: break;
    }
    /* Widths outside the set slop uses are legal LLVM but never produced by
     * lowering; make one rather than silently returning the wrong type. */
    {
        IrType *t = type_new(m, IR_TY_INT);
        t->bits = bits;
        return t;
    }
}

IrType *ir_type_float(IrModule *m, uint32_t bits) {
    return bits == 32 ? m->ty_f32 : m->ty_f64;
}

IrType *ir_type_ptr(IrModule *m) {
    return m->ty_ptr;
}

IrType *ir_type_array(IrModule *m, IrType *elem, uint64_t length) {
    IrType *t = type_new(m, IR_TY_ARRAY);
    t->elem = elem;
    t->length = length;
    return t;
}

IrType *ir_type_struct(IrModule *m, const char *name) {
    IrType *t = type_new(m, IR_TY_STRUCT);
    t->name = dup_cstr(m->arena, name);

    if (m->nstructs == m->nstructs_cap) {
        uint32_t cap = m->nstructs_cap == 0 ? 8 : m->nstructs_cap * 2;
        IrType **grown = arena_alloc(m->arena, cap * sizeof(IrType *));
        if (m->nstructs != 0) {
            memcpy(grown, m->structs, m->nstructs * sizeof(IrType *));
        }
        m->structs = grown;
        m->nstructs_cap = cap;
    }
    m->structs[m->nstructs++] = t;
    return t;
}

void ir_struct_set_body(IrModule *m, IrType *st, IrType **fields, uint32_t n) {
    st->fields = arena_alloc(m->arena, (n == 0 ? 1 : n) * sizeof(IrType *));
    if (n != 0) {
        memcpy(st->fields, fields, n * sizeof(IrType *));
    }
    st->nfields = n;
}

static uint32_t align_up32(uint32_t n, uint32_t align) {
    return (n + align - 1u) & ~(align - 1u);
}

uint32_t ir_type_align(const IrType *t) {
    switch (t->kind) {
    case IR_TY_VOID: return 1;
    case IR_TY_INT: return t->bits <= 8 ? 1 : t->bits / 8u;
    case IR_TY_FLOAT: return t->bits / 8u;
    case IR_TY_PTR: return 8;
    case IR_TY_ARRAY: return ir_type_align(t->elem);
    case IR_TY_STRUCT: {
        uint32_t a = 1, i;
        for (i = 0; i < t->nfields; i++) {
            uint32_t fa = ir_type_align(t->fields[i]);
            if (fa > a) {
                a = fa;
            }
        }
        return a;
    }
    }
    return 1;
}

uint32_t ir_type_size(const IrType *t) {
    switch (t->kind) {
    case IR_TY_VOID: return 0;
    case IR_TY_INT: return t->bits <= 8 ? 1 : t->bits / 8u;
    case IR_TY_FLOAT: return t->bits / 8u;
    case IR_TY_PTR: return 8;
    case IR_TY_ARRAY: return (uint32_t)t->length * ir_type_size(t->elem);
    case IR_TY_STRUCT: {
        /* Declaration order, no reordering: GRAMMAR.md section 6 makes this
         * a language guarantee, not a layout choice. */
        uint32_t off = 0, i;
        for (i = 0; i < t->nfields; i++) {
            off = align_up32(off, ir_type_align(t->fields[i]));
            off += ir_type_size(t->fields[i]);
        }
        return align_up32(off, ir_type_align(t));
    }
    }
    return 0;
}

/* ----------------------------------------------------------------- values */

static IrValue *value_new(IrModule *m, IrValueKind kind, IrType *type) {
    IrValue *v = arena_alloc(m->arena, sizeof(IrValue));
    v->kind = kind;
    v->type = type;
    return v;
}

IrValue *ir_const_int(IrModule *m, IrType *t, uint64_t v) {
    IrValue *val = value_new(m, IRV_CONST_INT, t);
    val->ival = v;
    return val;
}

IrValue *ir_const_bool(IrModule *m, int b) {
    return ir_const_int(m, m->ty_i1, b ? 1u : 0u);
}

IrValue *ir_const_fp(IrModule *m, IrType *t, double v) {
    IrValue *val = value_new(m, IRV_CONST_FP, t);
    val->fval = v;
    return val;
}

IrValue *ir_const_null(IrModule *m) {
    return value_new(m, IRV_CONST_NULL, m->ty_ptr);
}

IrValue *ir_undef(IrModule *m, IrType *t) {
    return value_new(m, IRV_UNDEF, t);
}

int64_t ir_const_signed(const IrValue *v) {
    uint32_t bits = v->type->kind == IR_TY_INT ? v->type->bits : 64;
    uint64_t raw = v->ival;
    uint64_t sign;

    if (bits >= 64) {
        return (int64_t)raw;
    }
    /* Sign-extend by hand: shifting a signed value left over its sign bit is
     * undefined, so the arithmetic stays unsigned until the last step. */
    sign = (uint64_t)1 << (bits - 1u);
    raw &= (sign << 1u) - 1u;
    if ((raw & sign) != 0) {
        raw |= ~((sign << 1u) - 1u);
    }
    return (int64_t)raw;
}

const char *ir_opcode_name(IrOpcode op) {
    static const char *const names[IR_OP__COUNT] = {
#define X(o, spelling) spelling,
        SLOP_IR_OPCODES(X)
#undef X
    };
    if (op < 0 || op >= IR_OP__COUNT) {
        return "<bad-op>";
    }
    return names[op];
}

const char *ir_pred_name(IrPred p) {
    static const char *const names[IR_PRED__COUNT] = {
#define X(pr, spelling) spelling,
        SLOP_IR_PREDS(X)
#undef X
    };
    if (p < 0 || p >= IR_PRED__COUNT) {
        return "<bad-pred>";
    }
    return names[p];
}

/* ---------------------------------------------------------------- module */

IrModule *ir_module_new(Arena *arena, const char *name) {
    IrModule *m = arena_alloc(arena, sizeof(IrModule));
    m->arena = arena;
    m->name = dup_cstr(arena, name);

    m->ty_void = type_new(m, IR_TY_VOID);
    m->ty_ptr = type_new(m, IR_TY_PTR);
    m->ty_i1 = type_new(m, IR_TY_INT);
    m->ty_i1->bits = 1;
    m->ty_i8 = type_new(m, IR_TY_INT);
    m->ty_i8->bits = 8;
    m->ty_i32 = type_new(m, IR_TY_INT);
    m->ty_i32->bits = 32;
    m->ty_i64 = type_new(m, IR_TY_INT);
    m->ty_i64->bits = 64;
    m->ty_f32 = type_new(m, IR_TY_FLOAT);
    m->ty_f32->bits = 32;
    m->ty_f64 = type_new(m, IR_TY_FLOAT);
    m->ty_f64->bits = 64;
    return m;
}

IrFunction *ir_function_new(IrModule *m, const char *name, IrType *ret, IrType **param_types,
                            uint32_t nparams, int is_vararg, int is_declaration) {
    IrFunction *f = arena_alloc(m->arena, sizeof(IrFunction));
    uint32_t i;

    f->name = dup_cstr(m->arena, name);
    f->ret = ret;
    f->nparams = nparams;
    f->is_vararg = is_vararg;
    f->is_declaration = is_declaration;
    f->module = m;
    f->params = arena_alloc(m->arena, (nparams == 0 ? 1 : nparams) * sizeof(IrValue *));
    for (i = 0; i < nparams; i++) {
        IrValue *p = value_new(m, IRV_LOCAL, param_types[i]);
        p->name = fmt_name(m->arena, "arg", i);
        f->params[i] = p;
    }

    if (m->last_function == NULL) {
        m->functions = f;
    } else {
        m->last_function->next = f;
    }
    m->last_function = f;
    return f;
}

void ir_param_set_name(IrFunction *f, uint32_t i, const char *name) {
    if (i < f->nparams) {
        f->params[i]->name = dup_cstr(f->module->arena, name);
    }
}

static void global_append(IrModule *m, IrGlobal *g) {
    if (m->last_global == NULL) {
        m->globals = g;
    } else {
        m->last_global->next = g;
    }
    m->last_global = g;
}

IrGlobal *ir_global_new(IrModule *m, const char *name, IrType *type, IrValue *init) {
    IrGlobal *g = arena_alloc(m->arena, sizeof(IrGlobal));
    g->name = dup_cstr(m->arena, name);
    g->type = type;
    g->init = init;
    global_append(m, g);
    return g;
}

IrGlobal *ir_global_extern(IrModule *m, const char *name, IrType *type) {
    IrGlobal *g = ir_global_new(m, name, type, NULL);
    g->is_external = 1;
    return g;
}

IrValue *ir_string(IrModule *m, const char *bytes, uint32_t len) {
    IrGlobal *g = arena_alloc(m->arena, sizeof(IrGlobal));
    IrValue *v;

    g->name = fmt_name(m->arena, ".str.", m->next_string++);
    /* The array holds the NUL too, which is why slop string literals can be
     * handed to C without copying. */
    g->type = ir_type_array(m, m->ty_i8, (uint64_t)len + 1u);
    g->str = arena_strndup(m->arena, bytes, len);
    g->strlen_ = len;
    g->is_const = 1;
    g->is_private = 1;
    global_append(m, g);

    v = value_new(m, IRV_GLOBAL, m->ty_ptr);
    v->name = g->name;
    return v;
}

IrBlock *ir_block_new(IrFunction *f, const char *name) {
    IrBlock *b = arena_alloc(f->module->arena, sizeof(IrBlock));
    b->name = dup_cstr(f->module->arena, name);
    b->parent = f;
    if (f->last == NULL) {
        f->first = b;
    } else {
        f->last->next = b;
    }
    f->last = b;
    return b;
}

static int is_terminator(IrOpcode op) {
    return op == IR_RET || op == IR_BR || op == IR_CONDBR || op == IR_UNREACHABLE;
}

int ir_block_is_terminated(const IrBlock *b) {
    return b->last != NULL && is_terminator(b->last->op);
}

/* ---------------------------------------------------------------- builder */

void ir_builder_init(IrBuilder *b, IrModule *m) {
    b->module = m;
    b->fn = NULL;
    b->block = NULL;
}

void ir_builder_position(IrBuilder *b, IrBlock *block) {
    b->block = block;
    b->fn = block != NULL ? block->parent : NULL;
}

static IrInstr *emit(IrBuilder *b, IrOpcode op, IrType *result_type, IrValue **args,
                     uint32_t nargs) {
    IrModule *m = b->module;
    IrInstr *in = arena_alloc(m->arena, sizeof(IrInstr));
    uint32_t i;

    in->op = op;
    in->nargs = nargs;
    in->args = arena_alloc(m->arena, (nargs == 0 ? 1 : nargs) * sizeof(IrValue *));
    for (i = 0; i < nargs; i++) {
        in->args[i] = args[i];
    }

    if (result_type != NULL && result_type->kind != IR_TY_VOID) {
        in->result = value_new(m, IRV_LOCAL, result_type);
        in->result->name = fmt_name(m->arena, "t", b->fn->next_temp++);
    }

    if (b->block->last == NULL) {
        b->block->first = in;
    } else {
        b->block->last->next = in;
    }
    b->block->last = in;
    return in;
}

IrValue *ir_build_alloca(IrBuilder *b, IrType *t, const char *name) {
    IrInstr *in = emit(b, IR_ALLOCA, b->module->ty_ptr, NULL, 0);
    in->type_arg = t;
    if (name != NULL) {
        in->result->name = dup_cstr(b->module->arena, name);
    }
    return in->result;
}

IrValue *ir_build_load(IrBuilder *b, IrType *t, IrValue *ptr) {
    IrInstr *in = emit(b, IR_LOAD, t, &ptr, 1);
    in->type_arg = t;
    return in->result;
}

void ir_build_store(IrBuilder *b, IrValue *val, IrValue *ptr) {
    IrValue *args[2];
    IrInstr *in;
    args[0] = val;
    args[1] = ptr;
    in = emit(b, IR_STORE, NULL, args, 2);
    in->type_arg = val->type;
}

IrValue *ir_build_gep_index(IrBuilder *b, IrType *elem, IrValue *base, IrValue *index) {
    IrValue *args[2];
    IrInstr *in;
    args[0] = base;
    args[1] = index;
    in = emit(b, IR_GEP, b->module->ty_ptr, args, 2);
    in->type_arg = elem;
    in->gep = IR_GEP_INDEX;
    return in->result;
}

IrValue *ir_build_gep_field(IrBuilder *b, IrType *st, IrValue *base, uint32_t field) {
    IrValue *args[2];
    IrInstr *in;
    args[0] = base;
    args[1] = ir_const_int(b->module, b->module->ty_i32, field);
    in = emit(b, IR_GEP, b->module->ty_ptr, args, 2);
    in->type_arg = st;
    in->gep = IR_GEP_FIELD;
    return in->result;
}

IrValue *ir_build_binary(IrBuilder *b, IrOpcode op, IrValue *lhs, IrValue *rhs) {
    IrValue *args[2];
    args[0] = lhs;
    args[1] = rhs;
    return emit(b, op, lhs->type, args, 2)->result;
}

static IrValue *build_cmp(IrBuilder *b, IrOpcode op, IrPred p, IrValue *lhs, IrValue *rhs) {
    IrValue *args[2];
    IrInstr *in;
    args[0] = lhs;
    args[1] = rhs;
    in = emit(b, op, b->module->ty_i1, args, 2);
    in->pred = p;
    in->type_arg = lhs->type;
    return in->result;
}

IrValue *ir_build_icmp(IrBuilder *b, IrPred p, IrValue *lhs, IrValue *rhs) {
    return build_cmp(b, IR_ICMP, p, lhs, rhs);
}

IrValue *ir_build_fcmp(IrBuilder *b, IrPred p, IrValue *lhs, IrValue *rhs) {
    return build_cmp(b, IR_FCMP, p, lhs, rhs);
}

IrValue *ir_build_cast(IrBuilder *b, IrOpcode op, IrValue *v, IrType *to) {
    IrInstr *in = emit(b, op, to, &v, 1);
    in->type_arg = v->type;
    return in->result;
}

IrValue *ir_build_call(IrBuilder *b, IrFunction *callee, IrValue **args, uint32_t nargs) {
    IrInstr *in = emit(b, IR_CALL, callee->ret, args, nargs);
    in->fn = callee;
    in->type_arg = callee->ret;
    in->is_vararg_call = callee->is_vararg;
    return in->result; /* NULL when the callee returns void */
}

void ir_build_ret(IrBuilder *b, IrValue *v) {
    IrInstr *in;
    if (v == NULL) {
        in = emit(b, IR_RET, NULL, NULL, 0);
        in->type_arg = b->module->ty_void;
    } else {
        in = emit(b, IR_RET, NULL, &v, 1);
        in->type_arg = v->type;
    }
}

void ir_build_br(IrBuilder *b, IrBlock *dest) {
    IrInstr *in = emit(b, IR_BR, NULL, NULL, 0);
    in->dest = dest;
}

void ir_build_condbr(IrBuilder *b, IrValue *cond, IrBlock *t, IrBlock *f) {
    IrInstr *in = emit(b, IR_CONDBR, NULL, &cond, 1);
    in->dest = t;
    in->dest2 = f;
}

void ir_build_unreachable(IrBuilder *b) {
    emit(b, IR_UNREACHABLE, NULL, NULL, 0);
}

/* --------------------------------------------------------------- verifier */

typedef struct {
    FILE *out;
    const IrFunction *fn;
    int problems;
} Verifier;

static void problem(Verifier *v, const char *what) {
    v->problems++;
    if (v->out != NULL) {
        fprintf(v->out, "ir: %s: %s\n", v->fn != NULL ? v->fn->name : "<module>", what);
    }
}

static int block_in_function(const IrFunction *f, const IrBlock *target) {
    const IrBlock *b;
    for (b = f->first; b != NULL; b = b->next) {
        if (b == target) {
            return 1;
        }
    }
    return 0;
}

static int types_equal(const IrType *a, const IrType *b) {
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL || a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
    case IR_TY_INT:
    case IR_TY_FLOAT: return a->bits == b->bits;
    case IR_TY_ARRAY: return a->length == b->length && types_equal(a->elem, b->elem);
    case IR_TY_STRUCT: return strcmp(a->name, b->name) == 0;
    default: return 1;
    }
}

/* The instruction that produced a value, or NULL when it is not one.
 * Linear, but verification is not on any hot path. */
static const IrInstr *defining_instr(const IrFunction *f, const IrValue *val) {
    const IrBlock *b;
    for (b = f->first; b != NULL; b = b->next) {
        const IrInstr *in;
        for (in = b->first; in != NULL; in = in->next) {
            if (in->result == val) {
                return in;
            }
        }
    }
    return NULL;
}

/* Opaque pointers mean LLVM accepts a store of any type through any pointer,
 * so a slot written wider than it was allocated is a type error nowhere and
 * a stack buffer overflow at run time. When the destination is visibly an
 * alloca, that is checkable, and this is the only place that will catch it. */
static void verify_slot_store(Verifier *v, const IrInstr *store) {
    const IrInstr *alloc = defining_instr(v->fn, store->args[1]);
    const IrType *slot;

    if (alloc == NULL || alloc->op != IR_ALLOCA) {
        return;
    }
    slot = alloc->type_arg;
    if (slot->kind == IR_TY_ARRAY || slot->kind == IR_TY_STRUCT) {
        return; /* an aggregate slot is written through a gep or a memcpy */
    }
    if (ir_type_size(store->args[0]->type) > ir_type_size(slot)) {
        problem(v, "store is wider than the slot it writes to");
    }
}

static void verify_instr(Verifier *v, const IrInstr *in) {
    switch (in->op) {
    case IR_STORE:
    case IR_LOAD:
    case IR_GEP:
        if (in->args[in->op == IR_STORE ? 1 : 0]->type->kind != IR_TY_PTR) {
            problem(v, "memory operand is not a pointer");
        }
        if (in->op == IR_STORE) {
            verify_slot_store(v, in);
        }
        break;
    case IR_ADD:
    case IR_SUB:
    case IR_MUL:
    case IR_SDIV:
    case IR_UDIV:
    case IR_SREM:
    case IR_UREM:
    case IR_AND:
    case IR_OR:
    case IR_XOR:
    case IR_SHL:
    case IR_LSHR:
    case IR_ASHR:
        if (!types_equal(in->args[0]->type, in->args[1]->type)) {
            problem(v, "binary operands differ in type");
        }
        if (in->args[0]->type->kind != IR_TY_INT) {
            problem(v, "integer op on a non-integer");
        }
        break;
    case IR_FADD:
    case IR_FSUB:
    case IR_FMUL:
    case IR_FDIV:
    case IR_FREM:
        if (in->args[0]->type->kind != IR_TY_FLOAT) {
            problem(v, "float op on a non-float");
        }
        break;
    case IR_ICMP:
    case IR_FCMP:
        if (!types_equal(in->args[0]->type, in->args[1]->type)) {
            problem(v, "comparison operands differ in type");
        }
        break;
    case IR_CONDBR:
        if (in->args[0]->type->kind != IR_TY_INT || in->args[0]->type->bits != 1) {
            problem(v, "conditional branch on a non-i1");
        }
        if (!block_in_function(v->fn, in->dest) || !block_in_function(v->fn, in->dest2)) {
            problem(v, "branch leaves the function");
        }
        break;
    case IR_BR:
        if (!block_in_function(v->fn, in->dest)) {
            problem(v, "branch leaves the function");
        }
        break;
    case IR_RET:
        if (in->nargs == 0) {
            if (v->fn->ret->kind != IR_TY_VOID) {
                problem(v, "ret void in a function that returns a value");
            }
        } else if (!types_equal(in->args[0]->type, v->fn->ret)) {
            problem(v, "returned value does not match the return type");
        }
        break;
    case IR_CALL:
        if (in->fn == NULL) {
            problem(v, "call without a callee");
        } else if (!in->fn->is_vararg && in->nargs != in->fn->nparams) {
            problem(v, "call passes the wrong number of arguments");
        } else if (in->fn->is_vararg && in->nargs < in->fn->nparams) {
            problem(v, "variadic call is missing a fixed argument");
        }
        break;
    default: break;
    }
}

int ir_verify(const IrModule *m, FILE *out) {
    Verifier v;
    const IrFunction *f;

    v.out = out;
    v.fn = NULL;
    v.problems = 0;

    for (f = m->functions; f != NULL; f = f->next) {
        const IrBlock *b;
        v.fn = f;

        if (f->is_declaration) {
            if (f->first != NULL) {
                problem(&v, "declaration has a body");
            }
            continue;
        }
        if (f->first == NULL) {
            problem(&v, "definition has no blocks");
            continue;
        }

        for (b = f->first; b != NULL; b = b->next) {
            const IrInstr *in;
            int seen_terminator = 0;

            for (in = b->first; in != NULL; in = in->next) {
                if (seen_terminator) {
                    problem(&v, "instruction after a terminator");
                    break;
                }
                if (is_terminator(in->op)) {
                    seen_terminator = 1;
                }
                verify_instr(&v, in);
            }
            if (!seen_terminator) {
                problem(&v, "block is not terminated");
            }
        }
    }
    return v.problems;
}
