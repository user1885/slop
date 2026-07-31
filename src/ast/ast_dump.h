#ifndef SLOP_AST_AST_DUMP_H
#define SLOP_AST_AST_DUMP_H

#include "ast/ast.h"

#include <stdio.h>

/* Debug rendering of a tree, kept out of ast.c so the node definitions stay
 * free of any opinion about how they are displayed. Both of these tolerate
 * the NULL children that error recovery leaves behind. */

/* Writes a type back in source syntax (`i32*[10]`). */
void ast_print_type(FILE *out, const Type *t);

/* How to render a sema type, for the ` : <type>` an expression gets once it
 * has one. The dumper cannot know -- `struct Ty` is sema's -- so the driver
 * supplies it, and the dependency keeps pointing away from the passes. */
typedef void (*AstTypePrinter)(FILE *out, const struct Ty *ty);

/* Indented tree dump, one node per line. Round-tripping is not a goal; this
 * exists so the tree can be eyeballed. `print_type` may be NULL, which is
 * what to pass before sema has run. */
void ast_dump(FILE *out, const Program *prog, AstTypePrinter print_type);

#endif /* SLOP_AST_AST_DUMP_H */
