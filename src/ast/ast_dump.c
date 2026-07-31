#include "ast/ast_dump.h"

#include <ctype.h>

static void print_view(FILE *out, StrView v) {
    if (v.data == NULL) {
        fputs("<none>", out);
        return;
    }
    fprintf(out, "%.*s", (int)v.len, v.data);
}

static void print_quoted(FILE *out, StrView v) {
    int32_t i;
    fputc('"', out);
    for (i = 0; i < v.len; i++) {
        unsigned char c = (unsigned char)v.data[i];
        switch (c) {
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        default:
            if (isprint(c)) {
                fputc((char)c, out);
            } else {
                fprintf(out, "\\x%02X", c);
            }
        }
    }
    fputc('"', out);
}

void ast_print_type(FILE *out, const Type *t) {
    if (t == NULL) {
        fputs("<error-type>", out);
        return;
    }
    switch (t->kind) {
    case TYPE_CORE:
        fputs(token_kind_name(t->core), out);
        break;
    case TYPE_NAME:
        print_view(out, t->name);
        break;
    case TYPE_POINTER:
        ast_print_type(out, t->elem);
        fputc('*', out);
        break;
    case TYPE_ARRAY:
        ast_print_type(out, t->elem);
        fprintf(out, "[%llu]", (unsigned long long)t->length);
        break;
    }
}

static void indent(FILE *out, int depth) {
    int i;
    for (i = 0; i < depth; i++) {
        fputs("  ", out);
    }
}

/* Keyword and punctuation kinds are named by their spelling, which is what
 * an operator or a core type should print as. */
static const char *op_str(TokenKind op) {
    return token_kind_name(op);
}

static void dump_expr(FILE *out, const Expr *e, int depth);
static void dump_stmt(FILE *out, const Stmt *s, int depth);
static void dump_block(FILE *out, const Block *b, int depth);

/* The type an expression got from sema, printed after its header line.
 * Set once by ast_dump(); a dumper is called from one place at a time and
 * threading it through every recursive call would be noise. */
static AstTypePrinter type_printer;

static void endline(FILE *out, const Expr *e) {
    if (type_printer != NULL && e->sem_type != NULL) {
        fputs(" : ", out);
        type_printer(out, e->sem_type);
    }
    fputc('\n', out);
}

static void dump_expr(FILE *out, const Expr *e, int depth) {
    uint32_t i;

    indent(out, depth);
    if (e == NULL) {
        fputs("<error-expr>\n", out);
        return;
    }

    switch (e->kind) {
    case EXPR_INT:
        fprintf(out, "int %llu", (unsigned long long)e->u.ival);
        endline(out, e);
        break;
    case EXPR_FLOAT:
        fprintf(out, "float %g", e->u.fval);
        endline(out, e);
        break;
    case EXPR_CHAR:
        fprintf(out, "char %llu", (unsigned long long)e->u.ival);
        endline(out, e);
        break;
    case EXPR_STRING:
        fputs("string ", out);
        print_quoted(out, e->u.sval);
        endline(out, e);
        break;
    case EXPR_BOOL:
        fprintf(out, "bool %s", e->u.bval ? "true" : "false");
        endline(out, e);
        break;
    case EXPR_NULL:
        fputs("null", out);
        endline(out, e);
        break;
    case EXPR_NAME:
        fputs("name ", out);
        print_view(out, e->u.sval);
        endline(out, e);
        break;
    case EXPR_UNARY:
        fprintf(out, "unary %s", op_str(e->u.unary.op));
        endline(out, e);
        dump_expr(out, e->u.unary.operand, depth + 1);
        break;
    case EXPR_BINARY:
        fprintf(out, "binary %s", op_str(e->u.binary.op));
        endline(out, e);
        dump_expr(out, e->u.binary.lhs, depth + 1);
        dump_expr(out, e->u.binary.rhs, depth + 1);
        break;
    case EXPR_CALL:
        fprintf(out, "call (%u arg%s)", e->u.call.nargs, e->u.call.nargs == 1 ? "" : "s");
        endline(out, e);
        dump_expr(out, e->u.call.callee, depth + 1);
        for (i = 0; i < e->u.call.nargs; i++) {
            dump_expr(out, e->u.call.args[i], depth + 1);
        }
        break;
    case EXPR_INDEX:
        fputs("index", out);
        endline(out, e);
        dump_expr(out, e->u.index.base, depth + 1);
        dump_expr(out, e->u.index.index, depth + 1);
        break;
    case EXPR_FIELD:
        fputs("field .", out);
        print_view(out, e->u.field.name);
        endline(out, e);
        dump_expr(out, e->u.field.base, depth + 1);
        break;
    case EXPR_CAST:
        fputs("cast to ", out);
        ast_print_type(out, e->u.cast.type);
        endline(out, e);
        dump_expr(out, e->u.cast.operand, depth + 1);
        break;
    case EXPR_SIZEOF:
        fputs("sizeof ", out);
        ast_print_type(out, e->u.size_of.type);
        endline(out, e);
        break;
    case EXPR_CONV:
        fputs("conv", out);
        endline(out, e);
        dump_expr(out, e->u.conv.operand, depth + 1);
        break;
    }
}

static void dump_block(FILE *out, const Block *b, int depth) {
    uint32_t i;
    indent(out, depth);
    fprintf(out, "block (%u)\n", b->nstmts);
    for (i = 0; i < b->nstmts; i++) {
        dump_stmt(out, b->stmts[i], depth + 1);
    }
}

static void dump_stmt(FILE *out, const Stmt *s, int depth) {
    uint32_t i, j;

    if (s == NULL) {
        indent(out, depth);
        fputs("<error-stmt>\n", out);
        return;
    }

    switch (s->kind) {
    case STMT_BLOCK:
        dump_block(out, &s->u.block, depth);
        break;
    case STMT_VAR:
        indent(out, depth);
        fputs("let ", out);
        if (s->u.var.is_mut) {
            fputs("mut ", out);
        }
        ast_print_type(out, s->u.var.type);
        fputc(' ', out);
        print_view(out, s->u.var.name);
        fputc('\n', out);
        if (s->u.var.init != NULL) {
            dump_expr(out, s->u.var.init, depth + 1);
        }
        break;
    case STMT_IF:
        indent(out, depth);
        fputs("if\n", out);
        dump_expr(out, s->u.if_stmt.cond, depth + 1);
        dump_block(out, &s->u.if_stmt.then_block, depth + 1);
        if (s->u.if_stmt.else_stmt != NULL) {
            indent(out, depth);
            fputs("else\n", out);
            dump_stmt(out, s->u.if_stmt.else_stmt, depth + 1);
        }
        break;
    case STMT_WHILE:
        indent(out, depth);
        fputs("while\n", out);
        dump_expr(out, s->u.while_stmt.cond, depth + 1);
        dump_block(out, &s->u.while_stmt.body, depth + 1);
        break;
    case STMT_MATCH:
        indent(out, depth);
        fprintf(out, "match (%u arms)\n", s->u.match.narms);
        dump_expr(out, s->u.match.scrutinee, depth + 1);
        for (i = 0; i < s->u.match.narms; i++) {
            const MatchArm *arm = &s->u.match.arms[i];
            indent(out, depth + 1);
            fputs("arm", out);
            for (j = 0; j < arm->nlabels; j++) {
                const MatchLabel *label = &arm->labels[j];
                fputc(j == 0 ? ' ' : ',', out);
                if (label->is_wildcard) {
                    fputc('_', out);
                } else if (label->value == NULL) {
                    fputs("<error>", out);
                } else if (label->value->kind == EXPR_NAME) {
                    print_view(out, label->value->u.sval);
                } else {
                    fprintf(out, "%llu", (unsigned long long)label->value->u.ival);
                }
            }
            fputc('\n', out);
            dump_stmt(out, arm->body, depth + 2);
        }
        break;
    case STMT_RETURN:
        indent(out, depth);
        fputs("return\n", out);
        if (s->u.ret.value != NULL) {
            dump_expr(out, s->u.ret.value, depth + 1);
        }
        break;
    case STMT_BREAK:
        indent(out, depth);
        fputs("break\n", out);
        break;
    case STMT_CONTINUE:
        indent(out, depth);
        fputs("continue\n", out);
        break;
    case STMT_EXPR:
        indent(out, depth);
        fputs("expr\n", out);
        dump_expr(out, s->u.expr.expr, depth + 1);
        break;
    case STMT_ASSIGN:
        indent(out, depth);
        fprintf(out, "assign %s\n", op_str(s->u.assign.op));
        dump_expr(out, s->u.assign.target, depth + 1);
        dump_expr(out, s->u.assign.value, depth + 1);
        break;
    case STMT_EMPTY:
        indent(out, depth);
        fputs("empty\n", out);
        break;
    }
}

static void dump_signature(FILE *out, const FnDecl *fn, int depth) {
    uint32_t i;

    indent(out, depth);
    fputs("fn ", out);
    print_view(out, fn->name);
    fputc('(', out);
    for (i = 0; i < fn->nparams; i++) {
        if (i != 0) {
            fputs(", ", out);
        }
        if (fn->params[i].is_mut) {
            fputs("mut ", out);
        }
        ast_print_type(out, fn->params[i].type);
        fputc(' ', out);
        print_view(out, fn->params[i].name);
    }
    if (fn->is_variadic) {
        fputs(fn->nparams != 0 ? ", ..." : "...", out);
    }
    fputs(") -> ", out);
    ast_print_type(out, fn->ret);
    fputc('\n', out);
}

static void dump_item(FILE *out, const Item *it, int depth) {
    uint32_t i;

    if (it == NULL) {
        indent(out, depth);
        fputs("<error-item>\n", out);
        return;
    }

    switch (it->kind) {
    case ITEM_FN:
        dump_signature(out, &it->u.fn, depth);
        dump_block(out, &it->u.fn.body, depth + 1);
        break;
    case ITEM_STRUCT:
        indent(out, depth);
        fputs("struct ", out);
        print_view(out, it->u.struct_decl.name);
        fputc('\n', out);
        for (i = 0; i < it->u.struct_decl.nfields; i++) {
            indent(out, depth + 1);
            ast_print_type(out, it->u.struct_decl.fields[i].type);
            fputc(' ', out);
            print_view(out, it->u.struct_decl.fields[i].name);
            fputc('\n', out);
        }
        break;
    case ITEM_ENUM:
        indent(out, depth);
        fputs("enum ", out);
        print_view(out, it->u.enum_decl.name);
        fprintf(out, " : %s\n", op_str(it->u.enum_decl.base));
        for (i = 0; i < it->u.enum_decl.nmembers; i++) {
            const EnumMember *m = &it->u.enum_decl.members[i];
            indent(out, depth + 1);
            print_view(out, m->name);
            if (m->has_value) {
                fprintf(out, " = %llu", (unsigned long long)m->value);
            }
            fputc('\n', out);
        }
        break;
    case ITEM_GLOBAL:
        indent(out, depth);
        fputs("let ", out);
        if (it->u.global.is_mut) {
            fputs("mut ", out);
        }
        ast_print_type(out, it->u.global.type);
        fputc(' ', out);
        print_view(out, it->u.global.name);
        fputc('\n', out);
        if (it->u.global.init != NULL) {
            dump_expr(out, it->u.global.init, depth + 1);
        }
        break;
    case ITEM_EXTERN:
        indent(out, depth);
        fputs("extern ", out);
        print_quoted(out, it->u.extern_block.abi);
        fputc('\n', out);
        for (i = 0; i < it->u.extern_block.nfns; i++) {
            dump_signature(out, &it->u.extern_block.fns[i], depth + 1);
        }
        break;
    }
}

void ast_dump(FILE *out, const Program *prog, AstTypePrinter print_type) {
    uint32_t i;

    type_printer = print_type;

    if (prog == NULL) {
        fputs("<no program>\n", out);
        return;
    }
    fprintf(out, "program %s (%u items)\n", prog->file != NULL ? prog->file : "<unknown>",
            prog->nitems);
    for (i = 0; i < prog->nitems; i++) {
        dump_item(out, prog->items[i], 1);
    }
}
