/* LLVM IR backend: prints the module as textual LLVM IR.
 *
 * Because the IR is modelled on LLVM's, this is a printer and not a
 * translation — every construct maps one to one. Output targets LLVM 15 or
 * newer, which is where pointers became opaque.
 *
 * Feed the result to any of:
 *   llc  -filetype=obj out.ll -o out.o     native object
 *   clang out.ll -o a.out                  straight to an executable
 *   opt  -O2 -S out.ll                     optimised, still readable
 *   lli  out.ll                            just run it
 */
#include "backend/backend.h"

#include <inttypes.h>
#include <string.h>

static void print_type(FILE *out, const IrType *t) {
    switch (t->kind) {
    case IR_TY_VOID:
        fputs("void", out);
        break;
    case IR_TY_INT:
        fprintf(out, "i%u", t->bits);
        break;
    case IR_TY_FLOAT:
        fputs(t->bits == 32 ? "float" : "double", out);
        break;
    case IR_TY_PTR:
        fputs("ptr", out);
        break;
    case IR_TY_ARRAY:
        fprintf(out, "[%" PRIu64 " x ", t->length);
        print_type(out, t->elem);
        fputc(']', out);
        break;
    case IR_TY_STRUCT:
        fprintf(out, "%%%s", t->name);
        break;
    }
}

/* LLVM writes a double as a 16-digit hex bit pattern, which is exact. Doing
 * the same avoids any question of what %f rounds to. */
static void print_fp(FILE *out, const IrValue *v) {
    double d = v->fval;
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    fprintf(out, "0x%016" PRIX64, bits);
}

static void print_value(FILE *out, const IrValue *v) {
    switch (v->kind) {
    case IRV_CONST_INT:
        if (v->type->kind == IR_TY_INT && v->type->bits == 1) {
            fputs(v->ival != 0 ? "true" : "false", out);
        } else {
            /* LLVM integers are signless, but a literal must still fit the
             * width, so print the value the bit pattern has when read as
             * signed: 0xFF at i8 is -1, and `i8 255` is rejected. */
            fprintf(out, "%" PRId64, ir_const_signed(v));
        }
        break;
    case IRV_CONST_FP:
        print_fp(out, v);
        break;
    case IRV_CONST_NULL:
        fputs("null", out);
        break;
    case IRV_UNDEF:
        fputs("undef", out);
        break;
    case IRV_LOCAL:
        fprintf(out, "%%%s", v->name);
        break;
    case IRV_GLOBAL:
    case IRV_FUNC:
        fprintf(out, "@%s", v->name);
        break;
    }
}

static void print_typed(FILE *out, const IrValue *v) {
    print_type(out, v->type);
    fputc(' ', out);
    print_value(out, v);
}

static void print_string_bytes(FILE *out, const char *s, uint32_t len) {
    uint32_t i;
    fputc('"', out);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        /* LLVM only requires escaping " and \, but anything outside printable
         * ASCII is escaped too so the .ll stays diffable. */
        if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7f) {
            fprintf(out, "\\%02X", c);
        } else {
            fputc((char)c, out);
        }
    }
    fputs("\\00\"", out); /* the NUL slop guarantees on string literals */
}

static void print_instr(FILE *out, const IrInstr *in) {
    uint32_t i;

    fputs("  ", out);
    if (in->result != NULL) {
        fprintf(out, "%%%s = ", in->result->name);
    }

    switch (in->op) {
    case IR_ALLOCA:
        fputs("alloca ", out);
        print_type(out, in->type_arg);
        fprintf(out, ", align %u", ir_type_align(in->type_arg));
        break;

    case IR_LOAD:
        fputs("load ", out);
        print_type(out, in->type_arg);
        fputs(", ", out);
        print_typed(out, in->args[0]);
        fprintf(out, ", align %u", ir_type_align(in->type_arg));
        break;

    case IR_STORE:
        fputs("store ", out);
        print_typed(out, in->args[0]);
        fputs(", ", out);
        print_typed(out, in->args[1]);
        fprintf(out, ", align %u", ir_type_align(in->type_arg));
        break;

    case IR_GEP:
        /* Field selection needs the leading zero index that steps through the
         * pointer itself; plain indexing does not. */
        fputs("getelementptr inbounds ", out);
        print_type(out, in->type_arg);
        fputs(", ", out);
        print_typed(out, in->args[0]);
        if (in->gep == IR_GEP_FIELD) {
            fputs(", i32 0, ", out);
        } else {
            fputs(", ", out);
        }
        print_typed(out, in->args[1]);
        break;

    case IR_ICMP:
    case IR_FCMP:
        fprintf(out, "%s %s ", ir_opcode_name(in->op), ir_pred_name(in->pred));
        print_typed(out, in->args[0]);
        fputs(", ", out);
        print_value(out, in->args[1]);
        break;

    case IR_TRUNC:
    case IR_ZEXT:
    case IR_SEXT:
    case IR_FPTRUNC:
    case IR_FPEXT:
    case IR_FPTOSI:
    case IR_FPTOUI:
    case IR_SITOFP:
    case IR_UITOFP:
    case IR_PTRTOINT:
    case IR_INTTOPTR:
        fprintf(out, "%s ", ir_opcode_name(in->op));
        print_typed(out, in->args[0]);
        fputs(" to ", out);
        print_type(out, in->result->type);
        break;

    case IR_CALL:
        fputs("call ", out);
        print_type(out, in->fn->ret);
        /* A variadic callee must be spelled with its full signature at the
         * call site, or LLVM cannot tell which arguments are promoted. */
        if (in->fn->is_vararg) {
            fputs(" (", out);
            for (i = 0; i < in->fn->nparams; i++) {
                if (i != 0) {
                    fputs(", ", out);
                }
                print_type(out, in->fn->params[i]->type);
            }
            fputs(in->fn->nparams != 0 ? ", ...)" : "...)", out);
        }
        fprintf(out, " @%s(", in->fn->name);
        for (i = 0; i < in->nargs; i++) {
            if (i != 0) {
                fputs(", ", out);
            }
            print_typed(out, in->args[i]);
        }
        fputc(')', out);
        break;

    case IR_RET:
        if (in->nargs == 0) {
            fputs("ret void", out);
        } else {
            fputs("ret ", out);
            print_typed(out, in->args[0]);
        }
        break;

    case IR_BR:
        fprintf(out, "br label %%%s", in->dest->name);
        break;

    case IR_CONDBR:
        fputs("br ", out);
        print_typed(out, in->args[0]);
        fprintf(out, ", label %%%s, label %%%s", in->dest->name, in->dest2->name);
        break;

    case IR_UNREACHABLE:
        fputs("unreachable", out);
        break;

    default:
        /* Every remaining opcode is a binary operator, and LLVM spells them
         * all the same way. */
        fprintf(out, "%s ", ir_opcode_name(in->op));
        print_typed(out, in->args[0]);
        fputs(", ", out);
        print_value(out, in->args[1]);
        break;
    }
    fputc('\n', out);
}

static void print_signature(FILE *out, const IrFunction *f) {
    uint32_t i;
    print_type(out, f->ret);
    fprintf(out, " @%s(", f->name);
    for (i = 0; i < f->nparams; i++) {
        if (i != 0) {
            fputs(", ", out);
        }
        print_type(out, f->params[i]->type);
        if (!f->is_declaration) {
            fprintf(out, " %%%s", f->params[i]->name);
        }
    }
    if (f->is_vararg) {
        fputs(f->nparams != 0 ? ", ..." : "...", out);
    }
    fputc(')', out);
}

static int llvm_emit(const IrModule *m, FILE *out) {
    const IrGlobal *g;
    const IrFunction *f;
    uint32_t i;

    fprintf(out, "; ModuleID = '%s'\n", m->name);
    fprintf(out, "source_filename = \"%s\"\n", m->name);

    if (m->nstructs != 0) {
        fputc('\n', out);
        for (i = 0; i < m->nstructs; i++) {
            const IrType *st = m->structs[i];
            uint32_t j;
            fprintf(out, "%%%s = type { ", st->name);
            for (j = 0; j < st->nfields; j++) {
                if (j != 0) {
                    fputs(", ", out);
                }
                print_type(out, st->fields[j]);
            }
            fputs(st->nfields != 0 ? " }\n" : "}\n", out);
        }
    }

    if (m->globals != NULL) {
        fputc('\n', out);
    }
    for (g = m->globals; g != NULL; g = g->next) {
        fprintf(out, "@%s = ", g->name);
        if (g->is_external) {
            fputs("external global ", out);
            print_type(out, g->type);
            fputc('\n', out);
            continue;
        }
        if (g->is_private) {
            fputs("private unnamed_addr ", out);
        }
        fputs(g->is_const ? "constant " : "global ", out);
        print_type(out, g->type);
        fputc(' ', out);
        if (g->str != NULL) {
            fputc('c', out);
            print_string_bytes(out, g->str, g->strlen_);
        } else if (g->init != NULL) {
            print_value(out, g->init);
        } else {
            fputs("zeroinitializer", out);
        }
        fprintf(out, ", align %u\n", ir_type_align(g->type));
    }

    for (f = m->functions; f != NULL; f = f->next) {
        if (!f->is_declaration) {
            continue;
        }
        fputs("\ndeclare ", out);
        print_signature(out, f);
        fputc('\n', out);
    }

    for (f = m->functions; f != NULL; f = f->next) {
        const IrBlock *b;
        if (f->is_declaration) {
            continue;
        }
        fputs("\ndefine ", out);
        print_signature(out, f);
        fputs(" {\n", out);
        for (b = f->first; b != NULL; b = b->next) {
            const IrInstr *in;
            fprintf(out, "%s:\n", b->name);
            for (in = b->first; in != NULL; in = in->next) {
                print_instr(out, in);
            }
        }
        fputs("}\n", out);
    }
    return 0;
}

const Backend backend_llvm = {
    "llvm", "textual LLVM IR, for llc/clang/opt/lli", ".ll", llvm_emit,
};
