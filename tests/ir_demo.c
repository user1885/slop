/* Builds a module by hand and emits it through a named backend.
 *
 * Lowering from the AST needs sema's resolved types, which do not exist yet,
 * so this is how the IR and the backends are exercised in the meantime. It
 * is not throwaway: it is the only place the IR is built without going
 * through lowering, which makes it the regression test for the builder API
 * itself.
 *
 * The module is what this slop program should lower to:
 *
 *     extern "c" { fn printf(u8* fmt, ...) -> i32; }
 *
 *     struct Point { i32 x; i32 y; }
 *     let mut i32 counter = 0;
 *
 *     fn sum_to(i32 n) -> i32 {
 *         let mut i32 total = 0;
 *         let mut i32 i = 0;
 *         while i < n { total += i; i += 1; }
 *         return total;
 *     }
 *     fn point_x(Point* p) -> i32 { return p.x; }
 *     fn elem(i32* a, i64 i) -> i32 { return a[i]; }
 *     fn widen(i8 c) -> i64 { return c.as(i64); }
 *
 *     fn main() -> i32 {
 *         printf("slop\n");
 *         let mut Point p; p.x = 3; p.y = 4;
 *         let mut i32[4] a; a[2] = 5;
 *         let i8 c = -1;
 *         return sum_to(10) - point_x(&p) - elem(&a[0], 2) + widen(c).as(i32);
 *     }
 *
 * sum_to(10) is 45, so main returns 45 - 3 - 5 + -1 == 36. The C backend's
 * output is compiled and run in the tests, and 36 is what it must exit with:
 * a golden file proves the text did not change, but running it proves the
 * text means the right thing.
 */
#include "backend/backend.h"
#include "ir/ir.h"
#include "support/arena.h"

#include <stdio.h>
#include <string.h>

static IrFunction *g_printf;
static IrFunction *g_sum_to;
static IrFunction *g_point_x;
static IrFunction *g_elem;
static IrFunction *g_widen;
static IrType *g_point;

static void build_sum_to(IrModule *m) {
    IrBuilder b;
    IrBlock *entry, *cond, *body, *done;
    IrValue *total, *i, *iv, *tv, *cmp, *sum, *next, *result;
    IrType *i32 = ir_type_int(m, 32);

    ir_builder_init(&b, m);
    entry = ir_block_new(g_sum_to, "entry");
    cond = ir_block_new(g_sum_to, "cond");
    body = ir_block_new(g_sum_to, "body");
    done = ir_block_new(g_sum_to, "done");

    ir_builder_position(&b, entry);
    total = ir_build_alloca(&b, i32, "total");
    i = ir_build_alloca(&b, i32, "i");
    ir_build_store(&b, ir_const_int(m, i32, 0), total);
    ir_build_store(&b, ir_const_int(m, i32, 0), i);
    ir_build_br(&b, cond);

    ir_builder_position(&b, cond);
    iv = ir_build_load(&b, i32, i);
    cmp = ir_build_icmp(&b, IR_ICMP_SLT, iv, g_sum_to->params[0]);
    ir_build_condbr(&b, cmp, body, done);

    ir_builder_position(&b, body);
    tv = ir_build_load(&b, i32, total);
    iv = ir_build_load(&b, i32, i);
    sum = ir_build_binary(&b, IR_ADD, tv, iv);
    ir_build_store(&b, sum, total);
    next = ir_build_binary(&b, IR_ADD, iv, ir_const_int(m, i32, 1));
    ir_build_store(&b, next, i);
    ir_build_br(&b, cond);

    ir_builder_position(&b, done);
    result = ir_build_load(&b, i32, total);
    ir_build_ret(&b, result);
}

static void build_accessors(IrModule *m) {
    IrBuilder b;
    IrBlock *entry;
    IrValue *addr, *v;
    IrType *i32 = ir_type_int(m, 32);
    IrType *i64 = ir_type_int(m, 64);

    ir_builder_init(&b, m);

    entry = ir_block_new(g_point_x, "entry");
    ir_builder_position(&b, entry);
    addr = ir_build_gep_field(&b, g_point, g_point_x->params[0], 0);
    v = ir_build_load(&b, i32, addr);
    ir_build_ret(&b, v);

    entry = ir_block_new(g_elem, "entry");
    ir_builder_position(&b, entry);
    addr = ir_build_gep_index(&b, i32, g_elem->params[0], g_elem->params[1]);
    v = ir_build_load(&b, i32, addr);
    ir_build_ret(&b, v);

    entry = ir_block_new(g_widen, "entry");
    ir_builder_position(&b, entry);
    v = ir_build_cast(&b, IR_SEXT, g_widen->params[0], i64);
    ir_build_ret(&b, v);
}

static void build_main(IrModule *m, IrFunction *fn, IrValue *msg) {
    IrBuilder b;
    IrBlock *entry;
    IrValue *p, *arr, *cell, *cvar, *args[2];
    IrValue *sum, *x, *e, *cv, *w, *wt, *r;
    IrType *i8 = ir_type_int(m, 8);
    IrType *i32 = ir_type_int(m, 32);
    IrType *i64 = ir_type_int(m, 64);

    ir_builder_init(&b, m);
    entry = ir_block_new(fn, "entry");
    ir_builder_position(&b, entry);

    args[0] = msg;
    ir_build_call(&b, g_printf, args, 1);

    p = ir_build_alloca(&b, g_point, "p");
    ir_build_store(&b, ir_const_int(m, i32, 3), ir_build_gep_field(&b, g_point, p, 0));
    ir_build_store(&b, ir_const_int(m, i32, 4), ir_build_gep_field(&b, g_point, p, 1));

    arr = ir_build_alloca(&b, ir_type_array(m, i32, 4), "a");
    cell = ir_build_gep_index(&b, i32, arr, ir_const_int(m, i64, 2));
    ir_build_store(&b, ir_const_int(m, i32, 5), cell);

    cvar = ir_build_alloca(&b, i8, "c");
    ir_build_store(&b, ir_const_int(m, i8, 0xFFu), cvar);

    args[0] = ir_const_int(m, i32, 10);
    sum = ir_build_call(&b, g_sum_to, args, 1);

    args[0] = p;
    x = ir_build_call(&b, g_point_x, args, 1);

    args[0] = arr;
    args[1] = ir_const_int(m, i64, 2);
    e = ir_build_call(&b, g_elem, args, 2);

    cv = ir_build_load(&b, i8, cvar);
    args[0] = cv;
    w = ir_build_call(&b, g_widen, args, 1);
    wt = ir_build_cast(&b, IR_TRUNC, w, i32);

    r = ir_build_binary(&b, IR_SUB, sum, x);
    r = ir_build_binary(&b, IR_SUB, r, e);
    r = ir_build_binary(&b, IR_ADD, r, wt);
    ir_build_ret(&b, r);
}

static IrModule *build_module(Arena *arena) {
    IrModule *m = ir_module_new(arena, "demo.slop");
    IrType *i8 = ir_type_int(m, 8);
    IrType *i32 = ir_type_int(m, 32);
    IrType *i64 = ir_type_int(m, 64);
    IrType *ptr = ir_type_ptr(m);
    IrType *fields[2];
    IrType *params[2];
    IrFunction *fn_main;
    IrValue *msg;

    g_point = ir_type_struct(m, "Point");
    fields[0] = i32;
    fields[1] = i32;
    ir_struct_set_body(m, g_point, fields, 2);

    ir_global_new(m, "counter", i32, ir_const_int(m, i32, 0));
    msg = ir_string(m, "slop\n", 5);

    params[0] = ptr;
    g_printf = ir_function_new(m, "printf", i32, params, 1, 1, 1);

    params[0] = i32;
    g_sum_to = ir_function_new(m, "sum_to", i32, params, 1, 0, 0);
    ir_param_set_name(g_sum_to, 0, "n");

    params[0] = ptr;
    g_point_x = ir_function_new(m, "point_x", i32, params, 1, 0, 0);
    ir_param_set_name(g_point_x, 0, "p");

    params[0] = ptr;
    params[1] = i64;
    g_elem = ir_function_new(m, "elem", i32, params, 2, 0, 0);

    params[0] = i8;
    g_widen = ir_function_new(m, "widen", i64, params, 1, 0, 0);

    fn_main = ir_function_new(m, "main", i32, NULL, 0, 0, 0);

    build_sum_to(m);
    build_accessors(m);
    build_main(m, fn_main, msg);
    return m;
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : NULL;
    const Backend *backend = name != NULL ? backend_find(name) : backend_default();
    Arena *arena;
    IrModule *m;
    int rc;

    if (backend == NULL) {
        fprintf(stderr, "ir_demo: no backend named '%s'. Available:\n", name);
        backend_list(stderr);
        return 2;
    }

    arena = arena_new(0);
    m = build_module(arena);

    if (ir_verify(m, stderr) != 0) {
        arena_free(arena);
        return 1;
    }

    rc = backend->emit(m, stdout);
    arena_free(arena);
    return rc;
}
