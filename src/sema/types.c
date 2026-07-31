#include "sema/types.h"

#include <stdio.h>
#include <string.h>

/* Pointers are 8 bytes because the only target in sight is x86-64. When that
 * stops being true this becomes a target property -- it is deliberately the
 * only place in the compiler that writes the number down. */
#define PTR_SIZE  8u
#define PTR_ALIGN 8u

typedef struct {
    TyKind kind;
    uint32_t size;
    uint32_t align;
} CoreLayout;

static const CoreLayout core_layout[] = {
    {TY_VOID, 0, 1},         {TY_I8, 1, 1},
    {TY_U8, 1, 1},           {TY_I32, 4, 4},
    {TY_U32, 4, 4},          {TY_I64, 8, 8},
    {TY_U64, 8, 8},          {TY_F32, 4, 4},
    {TY_F64, 8, 8},          {TY_B8, 1, 1},
    {TY_B32, 4, 4},          {TY_B64, 8, 8},
    {TY_UNTYPED_INT, 0, 1},  {TY_UNTYPED_FLOAT, 0, 1},
    {TY_UNTYPED_BOOL, 0, 1}, {TY_NULL, PTR_SIZE, PTR_ALIGN},
    {TY_POISON, 0, 1},
};

static Ty *ty_new(TypeTable *tt, TyKind kind) {
    Ty *t = arena_alloc(tt->arena, sizeof(Ty));
    t->kind = kind;
    t->align = 1;
    t->state = LAYOUT_UNSTARTED;
    return t;
}

void types_init(TypeTable *tt, Arena *arena) {
    size_t i;

    tt->arena = arena;
    memset(tt->core, 0, sizeof(tt->core));
    vec_init(&tt->derived, sizeof(Ty *));

    for (i = 0; i < sizeof(core_layout) / sizeof(core_layout[0]); i++) {
        Ty *t = ty_new(tt, core_layout[i].kind);
        t->size = core_layout[i].size;
        t->align = core_layout[i].align;
        tt->core[core_layout[i].kind] = t;
    }
}

void types_free(TypeTable *tt) {
    vec_drop(&tt->derived);
}

Ty *ty_core(TypeTable *tt, TyKind kind) {
    return tt->core[kind];
}

Ty *ty_from_token(TypeTable *tt, TokenKind kind) {
    switch (kind) {
    case TOK_KW_VOID:
        return ty_core(tt, TY_VOID);
    case TOK_KW_I8:
        return ty_core(tt, TY_I8);
    case TOK_KW_U8:
        return ty_core(tt, TY_U8);
    case TOK_KW_I32:
        return ty_core(tt, TY_I32);
    case TOK_KW_U32:
        return ty_core(tt, TY_U32);
    case TOK_KW_I64:
        return ty_core(tt, TY_I64);
    case TOK_KW_U64:
        return ty_core(tt, TY_U64);
    case TOK_KW_F32:
        return ty_core(tt, TY_F32);
    case TOK_KW_F64:
        return ty_core(tt, TY_F64);
    case TOK_KW_B8:
        return ty_core(tt, TY_B8);
    case TOK_KW_B32:
        return ty_core(tt, TY_B32);
    case TOK_KW_B64:
        return ty_core(tt, TY_B64);
    default:
        return NULL;
    }
}

static Ty **derived_at(TypeTable *tt, uint32_t i) {
    return &((Ty **)tt->derived.data)[i];
}

/* Interning is a linear scan. A program has tens of distinct pointer and
 * array types, not thousands, and a scan over an array beats a hash of a
 * composite key at that size. */
Ty *ty_ptr(TypeTable *tt, Ty *elem) {
    uint32_t i;
    Ty *t;

    for (i = 0; i < tt->derived.len; i++) {
        Ty *d = *derived_at(tt, i);
        if (d->kind == TY_PTR && d->elem == elem) {
            return d;
        }
    }
    t = ty_new(tt, TY_PTR);
    t->elem = elem;
    t->size = PTR_SIZE;
    t->align = PTR_ALIGN;
    vec_push_ptr(&tt->derived, t);
    return t;
}

Ty *ty_array(TypeTable *tt, Ty *elem, uint64_t length) {
    uint32_t i;
    Ty *t;

    for (i = 0; i < tt->derived.len; i++) {
        Ty *d = *derived_at(tt, i);
        if (d->kind == TY_ARRAY && d->elem == elem && d->length == length) {
            return d;
        }
    }
    t = ty_new(tt, TY_ARRAY);
    t->elem = elem;
    t->length = length;
    t->size = (uint32_t)(length * elem->size);
    t->align = elem->align;
    vec_push_ptr(&tt->derived, t);
    return t;
}

Ty *ty_struct_new(TypeTable *tt, StrView name, SrcPos pos) {
    Ty *t = ty_new(tt, TY_STRUCT);
    t->name = name;
    t->pos = pos;
    vec_push_ptr(&tt->derived, t);
    return t;
}

Ty *ty_enum_new(TypeTable *tt, StrView name, Ty *base, SrcPos pos) {
    Ty *t = ty_new(tt, TY_ENUM);
    t->name = name;
    t->pos = pos;
    t->base = base;
    t->size = base->size;
    t->align = base->align;
    t->state = LAYOUT_DONE;
    vec_push_ptr(&tt->derived, t);
    return t;
}

const FieldSym *ty_field(const Ty *t, StrView name) {
    uint32_t i;
    if (t == NULL || t->kind != TY_STRUCT) {
        return NULL;
    }
    for (i = 0; i < t->nfields; i++) {
        if (strview_eq_view(t->fields[i].name, name)) {
            return &t->fields[i];
        }
    }
    return NULL;
}

int ty_is_integer(const Ty *t) {
    switch (t->kind) {
    case TY_I8:
    case TY_U8:
    case TY_I32:
    case TY_U32:
    case TY_I64:
    case TY_U64:
        return 1;
    default:
        return 0;
    }
}

int ty_is_signed(const Ty *t) {
    return t->kind == TY_I8 || t->kind == TY_I32 || t->kind == TY_I64;
}

int ty_is_float(const Ty *t) {
    return t->kind == TY_F32 || t->kind == TY_F64;
}

int ty_is_bool(const Ty *t) {
    return t->kind == TY_B8 || t->kind == TY_B32 || t->kind == TY_B64;
}

int ty_is_numeric(const Ty *t) {
    return ty_is_integer(t) || ty_is_float(t) || t->kind == TY_UNTYPED_INT ||
           t->kind == TY_UNTYPED_FLOAT;
}

int ty_is_untyped(const Ty *t) {
    return t->kind == TY_UNTYPED_INT || t->kind == TY_UNTYPED_FLOAT || t->kind == TY_UNTYPED_BOOL ||
           t->kind == TY_NULL;
}

int ty_is_sized(const Ty *t) {
    switch (t->kind) {
    case TY_VOID:
    case TY_POISON:
    case TY_UNTYPED_INT:
    case TY_UNTYPED_FLOAT:
    case TY_UNTYPED_BOOL:
        return 0;
    case TY_STRUCT:
        return t->state == LAYOUT_DONE;
    default:
        return 1;
    }
}

int ty_int_fits(const Ty *t, uint64_t value) {
    switch (t->kind) {
    case TY_I8:
        return value <= 0x7Full;
    case TY_U8:
        return value <= 0xFFull;
    case TY_I32:
        return value <= 0x7FFFFFFFull;
    case TY_U32:
        return value <= 0xFFFFFFFFull;
    case TY_I64:
        return value <= 0x7FFFFFFFFFFFFFFFull;
    case TY_U64:
        return 1;
    case TY_F32:
    case TY_F64:
        return 1;
    case TY_ENUM:
        return ty_int_fits(t->base, value);
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ names */

static void append(char *buf, size_t cap, size_t *at, const char *s, size_t n) {
    size_t room;
    if (cap == 0 || *at + 1 >= cap) {
        return;
    }
    room = cap - 1 - *at;
    if (n > room) {
        n = room;
    }
    memcpy(buf + *at, s, n);
    *at += n;
    buf[*at] = '\0';
}

static void render(const Ty *t, char *buf, size_t cap, size_t *at) {
    const char *name;

    if (t == NULL) {
        append(buf, cap, at, "<none>", 6);
        return;
    }
    switch (t->kind) {
    case TY_PTR:
        render(t->elem, buf, cap, at);
        append(buf, cap, at, "*", 1);
        return;
    case TY_ARRAY: {
        char n[32];
        int len = snprintf(n, sizeof(n), "[%llu]", (unsigned long long)t->length);
        render(t->elem, buf, cap, at);
        append(buf, cap, at, n, len > 0 ? (size_t)len : 0);
        return;
    }
    case TY_STRUCT:
    case TY_ENUM:
        append(buf, cap, at, t->name.data, (size_t)(t->name.len > 0 ? t->name.len : 0));
        return;
    default:
        break;
    }

    switch (t->kind) {
    case TY_VOID:
        name = "void";
        break;
    case TY_I8:
        name = "i8";
        break;
    case TY_U8:
        name = "u8";
        break;
    case TY_I32:
        name = "i32";
        break;
    case TY_U32:
        name = "u32";
        break;
    case TY_I64:
        name = "i64";
        break;
    case TY_U64:
        name = "u64";
        break;
    case TY_F32:
        name = "f32";
        break;
    case TY_F64:
        name = "f64";
        break;
    case TY_B8:
        name = "b8";
        break;
    case TY_B32:
        name = "b32";
        break;
    case TY_B64:
        name = "b64";
        break;
    case TY_UNTYPED_INT:
        name = "integer literal";
        break;
    case TY_UNTYPED_FLOAT:
        name = "float literal";
        break;
    case TY_UNTYPED_BOOL:
        name = "bool";
        break;
    case TY_NULL:
        name = "null";
        break;
    default:
        name = "<error>";
        break;
    }
    append(buf, cap, at, name, strlen(name));
}

const char *ty_str(const Ty *t, char *buf, size_t cap) {
    size_t at = 0;
    if (cap == 0) {
        return buf;
    }
    buf[0] = '\0';
    render(t, buf, cap, &at);
    return buf;
}
