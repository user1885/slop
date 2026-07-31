#ifndef SLOP_PARSER_H
#define SLOP_PARSER_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"

#include <stddef.h>

/* Pass 1: one token stream to one AST.
 *
 * The parser knows nothing about types or names -- `let` makes declarations
 * unambiguous, so no pre-scan for type names is needed and no table is
 * consulted. Everything it builds lives in `arena`.
 *
 * `tokens`/`count` is what lexer_lex_all() produced, TOK_EOF included. Any
 * TOK_ERROR in the stream is stepped over and counted, but not printed: it
 * already carries the lexer's message for the driver to report.
 *
 * On a malformed file the parser reports what it can (panic-mode recovery at
 * statement, member and item boundaries) and keeps going, so one run lists
 * many errors instead of only the first. A tree is always returned; check
 * *out_errors before handing it to a later pass, because error recovery can
 * leave NULL children behind.
 *
 * Lifetimes: identifiers in the tree point into the source buffer, so it has
 * to outlive the arena. String literals do not -- they are copied out of the
 * lexer's pool, so the AST survives lexer_free(). `file` is used in
 * diagnostics and stored in the Program; it must outlive the arena too.
 * `out_errors` may be NULL. */
Program *parse_program(Arena *arena, const Token *tokens, size_t count, const char *file,
                       int *out_errors);

#endif /* SLOP_PARSER_H */
