#ifndef SLOP_SEMA_SEMA_H
#define SLOP_SEMA_SEMA_H

#include "ast/ast.h"
#include "support/arena.h"

#include <stddef.h>
#include <stdio.h>

/* Passes 2 to 4 of GRAMMAR.md section 7: global name collection, type
 * resolution, body checking.
 *
 * Every file that was parsed goes in together, because declaration order is
 * irrelevant in slop and a body may call a function declared in another
 * file. Returns the number of errors; 0 means every Expr has a sem_type and
 * the trees are safe to lower. Anything else means stop -- a tree that
 * produced errors is not a tree the backend should see.
 *
 * `arena` must be the one the trees live in: the types sema builds and the
 * conversion nodes it inserts go there and outlive the call. */
int sema_check(Arena *arena, Program **programs, size_t nprograms);

/* Renders a resolved type, for ast_dump's AstTypePrinter. */
void sema_print_type(FILE *out, const struct Ty *ty);

#endif /* SLOP_SEMA_SEMA_H */
