#ifndef SLOP_SEMA_SCOPE_H
#define SLOP_SEMA_SCOPE_H

#include "ast/ast.h"
#include "sema/types.h"
#include "support/map.h"
#include "support/vec.h"

/* Names, in two namespaces that cannot collide.
 *
 * Types are struct and enum names and are only ever consulted from type
 * position; values are functions, globals, enum members, parameters and
 * locals. So `struct Token` and a variable called `Token` coexist, exactly
 * as in C, and neither use site is ambiguous. */

typedef enum { SYM_FN, SYM_GLOBAL, SYM_ENUM_MEMBER, SYM_LOCAL, SYM_PARAM } SymKind;

typedef struct {
    SymKind kind;
    StrView name;
    SrcPos pos;
    Ty *type;   /* the value's type; for SYM_FN, the return type */
    int is_mut; /* variables and parameters */

    uint64_t value; /* SYM_ENUM_MEMBER */

    FnDecl *decl; /* SYM_FN: signature and body */
    Ty **params;  /* SYM_FN: resolved in pass 3 */
    uint32_t nparams;
    int is_variadic;
    int is_extern;

    Item *item; /* SYM_GLOBAL: the declaration, for pass 3 */
} Sym;

typedef struct {
    Map types;  /* StrView -> Ty*  */
    Map values; /* StrView -> Sym* */
} Globals;

void globals_init(Globals *g);
void globals_free(Globals *g);

Ty *globals_find_type(const Globals *g, StrView name);
Sym *globals_find_value(const Globals *g, StrView name);

/* Return 0 when the name is already taken, which is the duplicate
 * diagnostic; the caller reports it against the existing declaration. */
int globals_add_type(Globals *g, StrView name, Ty *type);
int globals_add_value(Globals *g, StrView name, Sym *sym);

/* Lexical scopes: one stack of symbols with a mark per open scope. Bodies
 * hold a handful of locals, so scanning inward beats a hash table per
 * block, and popping a scope is truncating the stack. */
typedef struct {
    Vec syms;  /* Sym*     */
    Vec marks; /* uint32_t */
} Scopes;

void scopes_init(Scopes *sc);
void scopes_free(Scopes *sc);
void scope_push(Scopes *sc);
void scope_pop(Scopes *sc);

/* Innermost first: an inner scope shadows an outer one. */
Sym *scope_find(const Scopes *sc, StrView name);

/* The innermost scope only -- what redeclaration is checked against. */
Sym *scope_find_current(const Scopes *sc, StrView name);

void scope_declare(Scopes *sc, Sym *sym);

#endif /* SLOP_SEMA_SCOPE_H */
