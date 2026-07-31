/* Lowering: checked AST to IR.
 *
 * Pass 5 of GRAMMAR.md section 7. Runs only after `sema_check` returned 0 —
 * a tree that produced errors has NULL children and untyped expressions, and
 * nothing here defends against either.
 *
 * Every expression carries its resolved `sem_type`, which is where the
 * signedness of an operation, the width of a conversion and the element type
 * of an index all come from. Declarations do not carry one, so their types
 * are converted from the syntax instead; the two agree because struct layout
 * is a guarantee in GRAMMAR.md section 6, not an implementation detail.
 */
#ifndef SLOP_BACKEND_LOWERING_H
#define SLOP_BACKEND_LOWERING_H

#include "ast/ast.h"
#include "ir/ir.h"
#include "support/arena.h"

#include <stddef.h>

/* Lowers every program into one module, named after the first file. Returns
 * NULL if lowering hit something it cannot express, having reported it to
 * stderr. The module lives in `arena`. */
IrModule *lower_programs(Arena *arena, Program **programs, size_t nprograms);

#endif /* SLOP_BACKEND_LOWERING_H */
