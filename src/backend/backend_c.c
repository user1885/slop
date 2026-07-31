/* C backend: prints the module as C99.
 *
 * This exists to keep the backend interface honest. LLVM IR and C are about
 * as far apart as two textual targets get — SSA with basic blocks versus
 * declarations and statements — so if both can be written against `IrModule`
 * without either one reaching back into the AST, the seam is real.
 *
 * It is also useful: a C backend bootstraps slop anywhere a C compiler
 * exists, without an LLVM install.
 *
 * Two things make SSA-to-C straightforward here:
 *
 *   The IR has no phi nodes (every local is an alloca), so a block is just a
 *   label and its instructions are just statements. Control flow becomes
 *   goto, which is exactly what C's goto is for.
 *
 *   Every pointer is emitted as `void *` and every address computation as
 *   byte arithmetic on `char *`. Nothing depends on C's typed pointer
 *   arithmetic, so LLVM's opaque pointers survive the trip intact.
 *
 * Compile the output with **-fno-builtin**. slop's `u8*` becomes `void *`,
 * so the prototype emitted for an extern like `printf` does not match the
 * one the C compiler has built in, and gcc and clang both warn about the
 * mismatch. The declaration is ABI-compatible — `void *` and `const char *`
 * are passed identically — so the warning is about C's type rules, not about
 * the generated code being wrong.
 */
#include "backend/backend.h"

#include <inttypes.h>
#include <string.h>

typedef struct {
    FILE *out;
    int problems;
} CEmit;

static void unsupported(CEmit *e, const char *what) {
    e->problems++;
    fprintf(e->out, "#error slop C backend: %s\n", what);
}

/* LLVM names may contain characters C identifiers may not — string constants
 * are called `.str.0`. Map anything unusable to '_'. */
static void print_ident(FILE *out, const char *name) {
    const char *p;
    for (p = name; *p != '\0'; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_';
        fputc(ok ? c : '_', out);
    }
}

/* The C spelling of a type as it appears in a declaration. Integers are
 * signed here; operations that are unsigned in the IR cast at the use site,
 * which is the only place the IR records signedness. */
static const char *scalar_type(const IrType *t) {
    switch (t->kind) {
    case IR_TY_VOID: return "void";
    case IR_TY_PTR: return "void *";
    case IR_TY_FLOAT: return t->bits == 32 ? "float" : "double";
    case IR_TY_INT:
        switch (t->bits) {
        case 1: return "_Bool";
        case 8: return "int8_t";
        case 32: return "int32_t";
        case 64: return "int64_t";
        default: return "int64_t";
        }
    default: return NULL; /* aggregates have no scalar spelling */
    }
}

static const char *unsigned_type(const IrType *t) {
    if (t->kind != IR_TY_INT) {
        return "uint64_t";
    }
    switch (t->bits) {
    case 1: return "_Bool";
    case 8: return "uint8_t";
    case 32: return "uint32_t";
    default: return "uint64_t";
    }
}

static void print_value(FILE *out, const IrValue *v) {
    switch (v->kind) {
    case IRV_CONST_INT:
        if (v->type->kind == IR_TY_INT && v->type->bits == 64) {
            fprintf(out, "INT64_C(%" PRId64 ")", ir_const_signed(v));
        } else {
            fprintf(out, "%" PRId64, ir_const_signed(v));
        }
        break;
    case IRV_CONST_FP:
        fprintf(out, "%.17g", v->fval);
        break;
    case IRV_CONST_NULL:
        fputs("NULL", out);
        break;
    case IRV_UNDEF:
        fputs("0", out);
        break;
    case IRV_LOCAL:
    case IRV_GLOBAL:
    case IRV_FUNC:
        print_ident(out, v->name);
        break;
    }
}

/* Field offsets follow the same rule as ir_type_size: declaration order,
 * C alignment, no reordering. */
static uint32_t field_offset(const IrType *st, uint32_t field) {
    uint32_t off = 0, i;
    for (i = 0; i < field && i < st->nfields; i++) {
        uint32_t a = ir_type_align(st->fields[i]);
        off = (off + a - 1u) & ~(a - 1u);
        off += ir_type_size(st->fields[i]);
    }
    if (field < st->nfields) {
        uint32_t a = ir_type_align(st->fields[field]);
        off = (off + a - 1u) & ~(a - 1u);
    }
    return off;
}

static int is_unsigned_op(IrOpcode op) {
    return op == IR_UDIV || op == IR_UREM || op == IR_LSHR;
}

static int is_unsigned_pred(IrPred p) {
    return p == IR_ICMP_ULT || p == IR_ICMP_ULE || p == IR_ICMP_UGT || p == IR_ICMP_UGE;
}

static const char *binary_operator(IrOpcode op) {
    switch (op) {
    case IR_ADD:
    case IR_FADD: return "+";
    case IR_SUB:
    case IR_FSUB: return "-";
    case IR_MUL:
    case IR_FMUL: return "*";
    case IR_SDIV:
    case IR_UDIV:
    case IR_FDIV: return "/";
    case IR_SREM:
    case IR_UREM:
    case IR_FREM: return "%";
    case IR_AND: return "&";
    case IR_OR: return "|";
    case IR_XOR: return "^";
    case IR_SHL: return "<<";
    case IR_LSHR:
    case IR_ASHR: return ">>";
    default: return NULL;
    }
}

static const char *pred_operator(IrPred p) {
    switch (p) {
    case IR_ICMP_EQ:
    case IR_FCMP_OEQ: return "==";
    case IR_ICMP_NE:
    case IR_FCMP_ONE: return "!=";
    case IR_ICMP_SLT:
    case IR_ICMP_ULT:
    case IR_FCMP_OLT: return "<";
    case IR_ICMP_SLE:
    case IR_ICMP_ULE:
    case IR_FCMP_OLE: return "<=";
    case IR_ICMP_SGT:
    case IR_ICMP_UGT:
    case IR_FCMP_OGT: return ">";
    case IR_ICMP_SGE:
    case IR_ICMP_UGE:
    case IR_FCMP_OGE: return ">=";
    default: return "==";
    }
}

static void print_instr(CEmit *e, const IrInstr *in) {
    FILE *out = e->out;
    uint32_t i;

    fputs("    ", out);

    switch (in->op) {
    case IR_ALLOCA:
        /* The storage was declared at the top of the function; here the
         * pointer just starts pointing at it. */
        print_ident(out, in->result->name);
        fputs(" = ", out);
        fputs("&storage_", out);
        print_ident(out, in->result->name);
        fputs(";\n", out);
        return;

    case IR_LOAD: {
        const char *ct = scalar_type(in->type_arg);
        if (ct == NULL) {
            unsupported(e, "load of an aggregate; lowering should emit a memcpy");
            return;
        }
        print_ident(out, in->result->name);
        fprintf(out, " = *(%s *)", ct);
        print_value(out, in->args[0]);
        fputs(";\n", out);
        return;
    }

    case IR_STORE: {
        const char *ct = scalar_type(in->type_arg);
        if (ct == NULL) {
            unsupported(e, "store of an aggregate; lowering should emit a memcpy");
            return;
        }
        fprintf(out, "*(%s *)", ct);
        print_value(out, in->args[1]);
        fputs(" = ", out);
        print_value(out, in->args[0]);
        fputs(";\n", out);
        return;
    }

    case IR_GEP:
        print_ident(out, in->result->name);
        fputs(" = (char *)", out);
        print_value(out, in->args[0]);
        if (in->gep == IR_GEP_FIELD) {
            fprintf(out, " + %u;\n", field_offset(in->type_arg, (uint32_t)in->args[1]->ival));
        } else {
            fprintf(out, " + (%u * (int64_t)", ir_type_size(in->type_arg));
            print_value(out, in->args[1]);
            fputs(");\n", out);
        }
        return;

    case IR_ICMP:
    case IR_FCMP:
        print_ident(out, in->result->name);
        fputs(" = (", out);
        if (in->op == IR_ICMP && is_unsigned_pred(in->pred)) {
            fprintf(out, "(%s)", unsigned_type(in->args[0]->type));
        }
        print_value(out, in->args[0]);
        fprintf(out, " %s ", pred_operator(in->pred));
        if (in->op == IR_ICMP && is_unsigned_pred(in->pred)) {
            fprintf(out, "(%s)", unsigned_type(in->args[1]->type));
        }
        print_value(out, in->args[1]);
        fputs(");\n", out);
        return;

    case IR_TRUNC:
    case IR_SEXT:
    case IR_FPTRUNC:
    case IR_FPEXT:
    case IR_FPTOSI:
    case IR_SITOFP:
        print_ident(out, in->result->name);
        fprintf(out, " = (%s)", scalar_type(in->result->type));
        print_value(out, in->args[0]);
        fputs(";\n", out);
        return;

    case IR_ZEXT:
    case IR_FPTOUI:
    case IR_UITOFP:
        /* Go through the unsigned type of the *source* first, so the value is
         * widened without sign extension. */
        print_ident(out, in->result->name);
        fprintf(out, " = (%s)(%s)", scalar_type(in->result->type),
                unsigned_type(in->args[0]->type));
        print_value(out, in->args[0]);
        fputs(";\n", out);
        return;

    case IR_PTRTOINT:
        print_ident(out, in->result->name);
        fprintf(out, " = (%s)(uintptr_t)", scalar_type(in->result->type));
        print_value(out, in->args[0]);
        fputs(";\n", out);
        return;

    case IR_INTTOPTR:
        print_ident(out, in->result->name);
        fputs(" = (void *)(uintptr_t)", out);
        print_value(out, in->args[0]);
        fputs(";\n", out);
        return;

    case IR_CALL:
        if (in->result != NULL) {
            print_ident(out, in->result->name);
            fputs(" = ", out);
        }
        print_ident(out, in->fn->name);
        fputc('(', out);
        for (i = 0; i < in->nargs; i++) {
            if (i != 0) {
                fputs(", ", out);
            }
            print_value(out, in->args[i]);
        }
        fputs(");\n", out);
        return;

    case IR_RET:
        if (in->nargs == 0) {
            fputs("return;\n", out);
        } else {
            fputs("return ", out);
            print_value(out, in->args[0]);
            fputs(";\n", out);
        }
        return;

    case IR_BR:
        fputs("goto ", out);
        print_ident(out, in->dest->name);
        fputs(";\n", out);
        return;

    case IR_CONDBR:
        fputs("if (", out);
        print_value(out, in->args[0]);
        fputs(") goto ", out);
        print_ident(out, in->dest->name);
        fputs("; else goto ", out);
        print_ident(out, in->dest2->name);
        fputs(";\n", out);
        return;

    case IR_UNREACHABLE:
        fputs("/* unreachable */;\n", out);
        return;

    default: {
        const char *cop = binary_operator(in->op);
        if (cop == NULL) {
            unsupported(e, "unhandled opcode");
            return;
        }
        print_ident(out, in->result->name);
        fputs(" = ", out);
        if (is_unsigned_op(in->op)) {
            /* Cast to the unsigned type, do the operation there, and let the
             * assignment convert back — the IR's signedness lives in the
             * opcode, and this is where it has to be honoured. */
            fprintf(out, "(%s)((%s)", scalar_type(in->result->type),
                    unsigned_type(in->args[0]->type));
            print_value(out, in->args[0]);
            fprintf(out, " %s (%s)", cop, unsigned_type(in->args[1]->type));
            print_value(out, in->args[1]);
            fputs(");\n", out);
        } else {
            print_value(out, in->args[0]);
            fprintf(out, " %s ", cop);
            print_value(out, in->args[1]);
            fputs(";\n", out);
        }
        return;
    }
    }
}

static void print_signature(FILE *out, const IrFunction *f) {
    uint32_t i;
    fprintf(out, "%s ", scalar_type(f->ret));
    print_ident(out, f->name);
    fputc('(', out);
    if (f->nparams == 0 && !f->is_vararg) {
        fputs("void", out);
    }
    for (i = 0; i < f->nparams; i++) {
        if (i != 0) {
            fputs(", ", out);
        }
        fprintf(out, "%s ", scalar_type(f->params[i]->type));
        print_ident(out, f->params[i]->name);
    }
    if (f->is_vararg) {
        fputs(f->nparams != 0 ? ", ..." : "...", out);
    }
    fputc(')', out);
}

/* Whether any instruction in the function reads this value. Quadratic in the
 * size of a function, which is fine at v0 sizes and keeps the backend free of
 * its own allocator; revisit if a function ever gets big enough to notice. */
static int value_is_used(const IrFunction *f, const IrValue *v) {
    const IrBlock *b;
    for (b = f->first; b != NULL; b = b->next) {
        const IrInstr *in;
        for (in = b->first; in != NULL; in = in->next) {
            uint32_t i;
            for (i = 0; i < in->nargs; i++) {
                if (in->args[i] == v) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int block_is_target(const IrFunction *f, const IrBlock *target) {
    const IrBlock *b;
    for (b = f->first; b != NULL; b = b->next) {
        const IrInstr *in;
        for (in = b->first; in != NULL; in = in->next) {
            if (in->dest == target || in->dest2 == target) {
                return 1;
            }
        }
    }
    return 0;
}

static void print_body(CEmit *e, const IrFunction *f) {
    FILE *out = e->out;
    const IrBlock *b;

    /* C wants every declaration before the statements that use them, and an
     * SSA value is used only after it is defined, so hoisting all of them to
     * the top is always safe. */
    for (b = f->first; b != NULL; b = b->next) {
        const IrInstr *in;
        for (in = b->first; in != NULL; in = in->next) {
            if (in->op == IR_ALLOCA) {
                uint32_t size = ir_type_size(in->type_arg);
                uint32_t slots = (size + 7u) / 8u;
                /* uint64_t backing gives 8-byte alignment, which covers every
                 * type slop has. */
                fprintf(out, "    uint64_t storage_");
                print_ident(out, in->result->name);
                fprintf(out, "[%u];\n", slots != 0 ? slots : 1u);
            }
            if (in->result != NULL) {
                const char *ct = scalar_type(in->result->type);
                fprintf(out, "    %s ", ct != NULL ? ct : "void *");
                print_ident(out, in->result->name);
                fputs(";\n", out);
            }
        }
    }

    for (b = f->first; b != NULL; b = b->next) {
        const IrInstr *in;
        /* The entry block is reached by falling in, so labelling it when
         * nothing branches back to it only earns an unused-label warning. */
        if (b != f->first || block_is_target(f, b)) {
            print_ident(out, b->name);
            fputs(":;\n", out);
        }
        for (in = b->first; in != NULL; in = in->next) {
            print_instr(e, in);
            /* A result nothing reads is a warning in every C compiler worth
             * using. Calls are kept for their side effects; the rest are
             * silenced rather than dropped, so the shape of the IR survives
             * into the C for anyone reading both. */
            if (in->result != NULL && !value_is_used(f, in->result)) {
                fputs("    (void)", out);
                print_ident(out, in->result->name);
                fputs(";\n", out);
            }
        }
    }
}

static int c_emit(const IrModule *m, FILE *out) {
    CEmit e;
    const IrGlobal *g;
    const IrFunction *f;
    uint32_t i;

    e.out = out;
    e.problems = 0;

    fprintf(out, "/* generated from %s by the slop C backend */\n", m->name);
    fputs("#include <stdint.h>\n#include <stddef.h>\n\n", out);

    for (i = 0; i < m->nstructs; i++) {
        const IrType *st = m->structs[i];
        uint32_t j;
        /* Emitted for readability only: the generated code addresses fields
         * by byte offset, so it does not depend on the C compiler laying
         * these out the way slop does. */
        fputs("struct ", out);
        print_ident(out, st->name);
        fputs(" {", out);
        for (j = 0; j < st->nfields; j++) {
            const char *ct = scalar_type(st->fields[j]);
            fprintf(out, " %s f%u;", ct != NULL ? ct : "void *", j);
        }
        fputs(" };\n", out);
    }
    if (m->nstructs != 0) {
        fputc('\n', out);
    }

    for (g = m->globals; g != NULL; g = g->next) {
        if (g->str != NULL) {
            uint32_t k;
            /* Not const: every pointer in the generated code is `void *`, and
             * passing a `const char *` to one is a constraint violation. */
            fputs("static char ", out);
            print_ident(out, g->name);
            fputs("[] = \"", out);
            for (k = 0; k < g->strlen_; k++) {
                unsigned char c = (unsigned char)g->str[k];
                if (c == '"' || c == '\\') {
                    fprintf(out, "\\%c", c);
                } else if (c >= 0x20 && c < 0x7f) {
                    fputc((char)c, out);
                } else {
                    fprintf(out, "\\x%02X", c);
                }
            }
            fputs("\";\n", out);
            continue;
        }
        if (g->is_external) {
            fputs("extern ", out);
        }
        {
            const char *ct = scalar_type(g->type);
            if (ct != NULL) {
                fprintf(out, "%s ", ct);
                print_ident(out, g->name);
            } else {
                /* An aggregate global is a byte blob, for the same reason
                 * allocas are: layout is slop's, not the C compiler's. */
                fputs("uint64_t ", out);
                print_ident(out, g->name);
                fprintf(out, "[%u]", (ir_type_size(g->type) + 7u) / 8u);
            }
        }
        if (!g->is_external && g->init != NULL && scalar_type(g->type) != NULL) {
            fputs(" = ", out);
            print_value(out, g->init);
        }
        fputs(";\n", out);
    }
    if (m->globals != NULL) {
        fputc('\n', out);
    }

    for (f = m->functions; f != NULL; f = f->next) {
        print_signature(out, f);
        fputs(";\n", out);
    }

    for (f = m->functions; f != NULL; f = f->next) {
        if (f->is_declaration) {
            continue;
        }
        fputc('\n', out);
        print_signature(out, f);
        fputs(" {\n", out);
        print_body(&e, f);
        fputs("}\n", out);
    }

    return e.problems;
}

const Backend backend_c = {
    "c", "C99 source, for bootstrapping without LLVM", ".c", c_emit,
};
