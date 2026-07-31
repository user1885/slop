#ifndef SLOP_SEMA_TYPES_H
#define SLOP_SEMA_TYPES_H

#include "lexer/lexer.h"
#include "support/arena.h"
#include "support/srcpos.h"
#include "support/strview.h"
#include "support/vec.h"

#include <stddef.h>
#include <stdint.h>

/* The resolved type universe.
 *
 * The AST's `Type` is syntax: a `TYPE_NAME` holds a name nobody has looked
 * up and nothing knows a size. This is the other side of that -- every type
 * resolved, sized and laid out. The names are deliberately different so the
 * two cannot be mixed up silently.
 *
 * Types are interned: exactly one Ty per distinct type, so `a == b` *is*
 * type equality and there is no ty_equal() to get wrong. */

typedef enum {
    /* core, in this order: ty_from_token() and the numeric predicates
     * depend on the grouping */
    TY_VOID,
    TY_I8,
    TY_U8,
    TY_I32,
    TY_U32,
    TY_I64,
    TY_U64,
    TY_F32,
    TY_F64,
    TY_B8,
    TY_B32,
    TY_B64,

    TY_PTR,
    TY_ARRAY,
    TY_STRUCT,
    TY_ENUM,

    /* An integer or char literal that has not met its context yet, `true` /
     * `false` and the result of a comparison, and the type of `null`. Each
     * adopts a real type at the point of use and never reaches the backend. */
    TY_UNTYPED_INT,
    TY_UNTYPED_FLOAT,
    TY_UNTYPED_BOOL,
    TY_NULL,

    /* The result of an expression that already produced a diagnostic.
     * Assignable to everything and produces none of its own, so one mistake
     * yields one message instead of a cascade. */
    TY_POISON,

    TY__COUNT
} TyKind;

typedef struct Ty Ty;

typedef struct {
    StrView name;
    Ty *type;
    uint32_t offset;
    SrcPos pos;
} FieldSym;

typedef struct {
    StrView name;
    uint64_t value;
    SrcPos pos;
} EnumSym;

/* Struct layout state, for telling recursion-by-value from recursion through
 * a pointer. */
typedef enum { LAYOUT_UNSTARTED, LAYOUT_IN_PROGRESS, LAYOUT_DONE } LayoutState;

struct Ty {
    TyKind kind;
    uint32_t size;  /* bytes; 0 for void and for a struct still opening */
    uint32_t align; /* bytes; at least 1 for anything sized */

    Ty *elem;        /* TY_PTR, TY_ARRAY */
    uint64_t length; /* TY_ARRAY */

    StrView name; /* TY_STRUCT, TY_ENUM */
    SrcPos pos;   /* where it was declared */

    FieldSym *fields; /* TY_STRUCT */
    uint32_t nfields;
    LayoutState state;

    EnumSym *members; /* TY_ENUM */
    uint32_t nmembers;
    Ty *base; /* TY_ENUM: the underlying core type */
};

/* Owns the interned types. One per compilation. */
typedef struct {
    Arena *arena;
    Ty *core[TY__COUNT]; /* the singletons, indexed by kind */
    Vec derived;         /* Ty* -- pointers, arrays, structs, enums */
} TypeTable;

void types_init(TypeTable *tt, Arena *arena);
void types_free(TypeTable *tt);

Ty *ty_core(TypeTable *tt, TyKind kind);

/* TOK_KW_I32 -> the i32 singleton. NULL for a token that is not a core
 * type, which the parser guarantees cannot happen in type position. */
Ty *ty_from_token(TypeTable *tt, TokenKind kind);

Ty *ty_ptr(TypeTable *tt, Ty *elem);
Ty *ty_array(TypeTable *tt, Ty *elem, uint64_t length);

/* Declared empty by pass 2 and filled in by pass 3. */
Ty *ty_struct_new(TypeTable *tt, StrView name, SrcPos pos);
Ty *ty_enum_new(TypeTable *tt, StrView name, Ty *base, SrcPos pos);

const FieldSym *ty_field(const Ty *t, StrView name);

int ty_is_integer(const Ty *t);
int ty_is_signed(const Ty *t); /* signed integer, not float */
int ty_is_float(const Ty *t);
int ty_is_bool(const Ty *t);
int ty_is_numeric(const Ty *t); /* integer, float, or an untyped one of those */
int ty_is_untyped(const Ty *t);
int ty_is_sized(const Ty *t); /* has a size: not void, not poison */

/* Does `value` fit in integer type `t`? Values are carried unsigned, as the
 * lexer produces them; there are no negative literals in the grammar. */
int ty_int_fits(const Ty *t, uint64_t value);

/* Renders in source syntax (`i32*[10]`, `Arena*`) into `buf`, and returns
 * it, so it can be used straight inside a diagnostic. */
const char *ty_str(const Ty *t, char *buf, size_t cap);

#endif /* SLOP_SEMA_TYPES_H */
