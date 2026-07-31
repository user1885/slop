#ifndef SLOP_DRIVER_TOKEN_DUMP_H
#define SLOP_DRIVER_TOKEN_DUMP_H

#include "lexer/lexer.h"

/* Prints one token of the `--tokens` listing on stdout, or, for TOK_ERROR,
 * the message it carries on stderr.
 *
 * This lives in the driver rather than next to the lexer because no pass
 * calls it: it exists to serve one command-line flag, and src/lexer/ is
 * another agent's area (see COLLABORATION.md). */
void token_dump(const char *file, const Token *t);

#endif /* SLOP_DRIVER_TOKEN_DUMP_H */
