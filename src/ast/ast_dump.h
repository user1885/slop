#ifndef SLOP_AST_AST_DUMP_H
#define SLOP_AST_AST_DUMP_H

#include "ast/ast.h"

#include <stdio.h>

/* Debug rendering of a tree, kept out of ast.c so the node definitions stay
 * free of any opinion about how they are displayed. Both of these tolerate
 * the NULL children that error recovery leaves behind. */

/* Writes a type back in source syntax (`i32*[10]`). */
void ast_print_type(FILE *out, const Type *t);

/* Indented tree dump, one node per line. Round-tripping is not a goal; this
 * exists so the parser can be eyeballed before sema exists. */
void ast_dump(FILE *out, const Program *prog);

#endif /* SLOP_AST_AST_DUMP_H */
