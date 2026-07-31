/* The slop IR: an in-memory model of LLVM IR.
 *
 * The IR *is* LLVM IR — same value model, same type system, same instruction
 * set — so the LLVM backend is a printer and nothing is lost in translation.
 * Keeping it in memory rather than emitting text directly is what makes a
 * second backend possible at all: `src/backend/` has two consumers of this
 * structure already, and neither one knows the AST exists.
 *
 * Two deliberate simplifications, both of which a backend may rely on:
 *
 *   No phi nodes. Every local variable is an `alloca` written with `store`
 *   and read with `load`. That is what clang -O0 emits and what
 *   `opt -mem2reg` promotes away, so nothing is lost against LLVM; against a
 *   hand-written backend it removes SSA construction and phi elimination,
 *   which are the two things that make a naive backend hard.
 *
 *   Every value carries an explicit name. LLVM's implicit `%0, %1, ...`
 *   numbering counts unnamed basic blocks too, so emitting it correctly
 *   couples the printer to traversal order. Named values cannot desync.
 *
 * Everything here is arena-allocated and lives as long as the arena. Nothing
 * is freed individually.
 */
#ifndef SLOP_IR_H
#define SLOP_IR_H

#include "support/arena.h"

#include <stdint.h>
#include <stdio.h>

typedef struct IrType IrType;
typedef struct IrValue IrValue;
typedef struct IrInstr IrInstr;
typedef struct IrBlock IrBlock;
typedef struct IrGlobal IrGlobal;
typedef struct IrFunction IrFunction;
typedef struct IrModule IrModule;

/* ------------------------------------------------------------------ types */

typedef enum {
    IR_TY_VOID,
    IR_TY_INT,   /* bits: 1, 8, 32, 64 — i1 is the type of a comparison */
    IR_TY_FLOAT, /* bits: 32, 64 */
    IR_TY_PTR,   /* opaque, as in LLVM 15+: a pointer has no pointee type */
    IR_TY_ARRAY,
    IR_TY_STRUCT
} IrTypeKind;

struct IrType {
    IrTypeKind kind;
    uint32_t bits;    /* IR_TY_INT, IR_TY_FLOAT */
    IrType *elem;     /* IR_TY_ARRAY */
    uint64_t length;  /* IR_TY_ARRAY */
    const char *name; /* IR_TY_STRUCT: printed as %name */
    IrType **fields;  /* IR_TY_STRUCT */
    uint32_t nfields;
};

/* The primitives are one-per-module singletons, so `a == b` is type equality
 * for them. Arrays and structs are not interned: compare structurally if you
 * ever need to, or better, do not need to. */
IrType *ir_type_void(IrModule *m);
IrType *ir_type_int(IrModule *m, uint32_t bits);
IrType *ir_type_float(IrModule *m, uint32_t bits);
IrType *ir_type_ptr(IrModule *m);
IrType *ir_type_array(IrModule *m, IrType *elem, uint64_t length);
/* Declares a named struct on the module; fill it with ir_struct_set_body. */
IrType *ir_type_struct(IrModule *m, const char *name);
void ir_struct_set_body(IrModule *m, IrType *st, IrType **fields, uint32_t n);

uint32_t ir_type_size(const IrType *t);  /* bytes; structs are C-laid-out */
uint32_t ir_type_align(const IrType *t); /* bytes */

/* ----------------------------------------------------------------- values */

typedef enum {
    IRV_CONST_INT,
    IRV_CONST_FP,
    IRV_CONST_NULL,
    IRV_UNDEF,
    IRV_LOCAL,  /* an instruction result or a parameter: %name */
    IRV_GLOBAL, /* a global variable or string constant: @name */
    IRV_FUNC    /* a function, as a call target: @name */
} IrValueKind;

struct IrValue {
    IrValueKind kind;
    IrType *type;
    uint64_t ival; /* IRV_CONST_INT, already truncated to `type` */
    double fval;   /* IRV_CONST_FP */
    const char *name;
};

IrValue *ir_const_int(IrModule *m, IrType *t, uint64_t v);
IrValue *ir_const_bool(IrModule *m, int b);
IrValue *ir_const_fp(IrModule *m, IrType *t, double v);
IrValue *ir_const_null(IrModule *m);
IrValue *ir_undef(IrModule *m, IrType *t);

/* An integer constant's bit pattern read as a signed value of its own width.
 * Both backends need this: LLVM rejects `i8 255`, and C would warn on it. */
int64_t ir_const_signed(const IrValue *v);

/* ----------------------------------------------------------- instructions */

/* Spelling doubles as the LLVM mnemonic, so the printer needs no table of
 * its own and a new opcode cannot be added without naming it. */
#define SLOP_IR_OPCODES(X)                                                                         \
    X(IR_ALLOCA, "alloca")                                                                         \
    X(IR_LOAD, "load")                                                                             \
    X(IR_STORE, "store")                                                                           \
    X(IR_GEP, "getelementptr")                                                                     \
    X(IR_ADD, "add")                                                                               \
    X(IR_SUB, "sub")                                                                               \
    X(IR_MUL, "mul")                                                                               \
    X(IR_SDIV, "sdiv")                                                                             \
    X(IR_UDIV, "udiv")                                                                             \
    X(IR_SREM, "srem")                                                                             \
    X(IR_UREM, "urem")                                                                             \
    X(IR_AND, "and")                                                                               \
    X(IR_OR, "or")                                                                                 \
    X(IR_XOR, "xor")                                                                               \
    X(IR_SHL, "shl")                                                                               \
    X(IR_LSHR, "lshr")                                                                             \
    X(IR_ASHR, "ashr")                                                                             \
    X(IR_FADD, "fadd")                                                                             \
    X(IR_FSUB, "fsub")                                                                             \
    X(IR_FMUL, "fmul")                                                                             \
    X(IR_FDIV, "fdiv")                                                                             \
    X(IR_FREM, "frem")                                                                             \
    X(IR_ICMP, "icmp")                                                                             \
    X(IR_FCMP, "fcmp")                                                                             \
    X(IR_TRUNC, "trunc")                                                                           \
    X(IR_ZEXT, "zext")                                                                             \
    X(IR_SEXT, "sext")                                                                             \
    X(IR_FPTRUNC, "fptrunc")                                                                       \
    X(IR_FPEXT, "fpext")                                                                           \
    X(IR_FPTOSI, "fptosi")                                                                         \
    X(IR_FPTOUI, "fptoui")                                                                         \
    X(IR_SITOFP, "sitofp")                                                                         \
    X(IR_UITOFP, "uitofp")                                                                         \
    X(IR_PTRTOINT, "ptrtoint")                                                                     \
    X(IR_INTTOPTR, "inttoptr")                                                                     \
    X(IR_CALL, "call")                                                                             \
    X(IR_RET, "ret")                                                                               \
    X(IR_BR, "br")                                                                                 \
    X(IR_CONDBR, "br")                                                                             \
    X(IR_UNREACHABLE, "unreachable")

typedef enum {
#define X(op, spelling) op,
    SLOP_IR_OPCODES(X)
#undef X
    IR_OP__COUNT
} IrOpcode;

const char *ir_opcode_name(IrOpcode op);

/* LLVM's comparison predicates, spelled as LLVM spells them. */
#define SLOP_IR_PREDS(X)                                                                           \
    X(IR_ICMP_EQ, "eq")                                                                            \
    X(IR_ICMP_NE, "ne")                                                                            \
    X(IR_ICMP_SLT, "slt")                                                                          \
    X(IR_ICMP_SLE, "sle")                                                                          \
    X(IR_ICMP_SGT, "sgt")                                                                          \
    X(IR_ICMP_SGE, "sge")                                                                          \
    X(IR_ICMP_ULT, "ult")                                                                          \
    X(IR_ICMP_ULE, "ule")                                                                          \
    X(IR_ICMP_UGT, "ugt")                                                                          \
    X(IR_ICMP_UGE, "uge")                                                                          \
    X(IR_FCMP_OEQ, "oeq")                                                                          \
    X(IR_FCMP_ONE, "one")                                                                          \
    X(IR_FCMP_OLT, "olt")                                                                          \
    X(IR_FCMP_OLE, "ole")                                                                          \
    X(IR_FCMP_OGT, "ogt")                                                                          \
    X(IR_FCMP_OGE, "oge")

typedef enum {
#define X(pred, spelling) pred,
    SLOP_IR_PREDS(X)
#undef X
    IR_PRED__COUNT
} IrPred;

const char *ir_pred_name(IrPred p);

/* A getelementptr is either indexing a pointer/array or selecting a struct
 * field. LLVM spells both the same way; a backend that is not LLVM needs to
 * tell them apart to emit `&p[i]` versus `&p->f2`. */
typedef enum { IR_GEP_INDEX, IR_GEP_FIELD } IrGepKind;

struct IrInstr {
    IrOpcode op;
    IrValue *result; /* NULL for store/br/ret-void/unreachable */
    IrValue **args;
    uint32_t nargs;
    IrType *type_arg;  /* alloca: allocated type. load/gep: pointee type.
                        * store: stored type. call: return type. */
    IrPred pred;       /* IR_ICMP, IR_FCMP */
    IrGepKind gep;     /* IR_GEP */
    IrBlock *dest;     /* IR_BR, IR_CONDBR (true edge) */
    IrBlock *dest2;    /* IR_CONDBR (false edge) */
    IrFunction *fn;    /* IR_CALL */
    int is_vararg_call;
    IrInstr *next;
};

struct IrBlock {
    const char *name;
    IrInstr *first;
    IrInstr *last;
    IrBlock *next;
    IrFunction *parent;
};

/* A block is terminated once a terminator has been built into it. Lowering
 * uses this to avoid emitting dead code after a `return`. */
int ir_block_is_terminated(const IrBlock *b);

/* -------------------------------------------------- functions and globals */

struct IrFunction {
    const char *name;
    IrType *ret;
    IrValue **params;
    uint32_t nparams;
    int is_vararg;
    int is_declaration; /* an extern: no body */
    IrBlock *first;
    IrBlock *last;
    uint32_t next_temp; /* names instruction results %t0, %t1, ... */
    IrFunction *next;
    IrModule *module;
};

struct IrGlobal {
    const char *name;
    IrType *type;
    IrValue *init;  /* NULL means zeroinitializer */
    const char *str;/* non-NULL for a string constant: raw bytes */
    uint32_t strlen_;
    int is_const;
    int is_private;
    int is_external; /* declared elsewhere, no definition here */
    IrGlobal *next;
};

struct IrModule {
    Arena *arena;
    const char *name; /* the source file this came from */
    IrGlobal *globals;
    IrGlobal *last_global;
    IrFunction *functions;
    IrFunction *last_function;
    IrType **structs; /* named struct types, in declaration order */
    uint32_t nstructs;
    uint32_t nstructs_cap;
    uint32_t next_string;
    IrType *ty_void;
    IrType *ty_i1;
    IrType *ty_i8;
    IrType *ty_i32;
    IrType *ty_i64;
    IrType *ty_f32;
    IrType *ty_f64;
    IrType *ty_ptr;
};

IrModule *ir_module_new(Arena *arena, const char *name);

IrFunction *ir_function_new(IrModule *m, const char *name, IrType *ret, IrType **param_types,
                            uint32_t nparams, int is_vararg, int is_declaration);
/* Parameters are %arg0, %arg1, ... unless renamed. */
void ir_param_set_name(IrFunction *f, uint32_t i, const char *name);

IrGlobal *ir_global_new(IrModule *m, const char *name, IrType *type, IrValue *init);
IrGlobal *ir_global_extern(IrModule *m, const char *name, IrType *type);
/* Interns a NUL-terminated string constant and returns a pointer to it. */
IrValue *ir_string(IrModule *m, const char *bytes, uint32_t len);

IrBlock *ir_block_new(IrFunction *f, const char *name);

/* ---------------------------------------------------------------- builder */

typedef struct {
    IrModule *module;
    IrFunction *fn;
    IrBlock *block;
} IrBuilder;

void ir_builder_init(IrBuilder *b, IrModule *m);
void ir_builder_position(IrBuilder *b, IrBlock *block);

IrValue *ir_build_alloca(IrBuilder *b, IrType *t, const char *name);
IrValue *ir_build_load(IrBuilder *b, IrType *t, IrValue *ptr);
void ir_build_store(IrBuilder *b, IrValue *val, IrValue *ptr);
IrValue *ir_build_gep_index(IrBuilder *b, IrType *elem, IrValue *base, IrValue *index);
IrValue *ir_build_gep_field(IrBuilder *b, IrType *st, IrValue *base, uint32_t field);
IrValue *ir_build_binary(IrBuilder *b, IrOpcode op, IrValue *lhs, IrValue *rhs);
IrValue *ir_build_icmp(IrBuilder *b, IrPred p, IrValue *lhs, IrValue *rhs);
IrValue *ir_build_fcmp(IrBuilder *b, IrPred p, IrValue *lhs, IrValue *rhs);
IrValue *ir_build_cast(IrBuilder *b, IrOpcode op, IrValue *v, IrType *to);
IrValue *ir_build_call(IrBuilder *b, IrFunction *callee, IrValue **args, uint32_t nargs);
void ir_build_ret(IrBuilder *b, IrValue *v); /* v == NULL: ret void */
void ir_build_br(IrBuilder *b, IrBlock *dest);
void ir_build_condbr(IrBuilder *b, IrValue *cond, IrBlock *t, IrBlock *f);
void ir_build_unreachable(IrBuilder *b);

/* --------------------------------------------------------------- checking */

/* Structural check of a finished module: every block terminated exactly
 * once, every branch target in the same function, operand types consistent,
 * no value used outside the function that defines it. Reports to `out` and
 * returns the number of problems. Cheap enough to run always; a backend is
 * entitled to assume a verified module. */
int ir_verify(const IrModule *m, FILE *out);

#endif /* SLOP_IR_H */
