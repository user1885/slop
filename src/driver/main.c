/* slop v0 — lexes, parses and checks its arguments, then dumps the AST.
 * `--tokens` stops after the lexer and dumps the token stream instead. */
#include "ast/ast_dump.h"
#include "driver/source_file.h"
#include "driver/token_dump.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "support/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One source file on its way through the front end. Everything here has to
 * stay alive until the last tree built from it is gone: names in the AST
 * point into `src`. */
typedef struct {
    const char *path;
    char *src;
    Lexer lexer;
    Token *tokens;
    size_t ntokens;
    Program *prog;
} Unit;

static int lex_file(const char *path) {
    size_t len = 0;
    char *src = source_file_read(path, &len);
    if (src == NULL) {
        return 1;
    }

    Lexer lx;
    lexer_init(&lx, path, src, len);

    Token *tokens = NULL;
    size_t count = 0;
    int32_t errors = lexer_lex_all(&lx, &tokens, &count);

    for (size_t i = 0; i < count; i++) {
        token_dump(path, &tokens[i]);
    }
    printf("%s: %zu tokens, %d error(s)\n", path, count, errors);

    free(tokens);
    lexer_free(&lx);
    free(src);
    return errors != 0;
}

static void unit_free(Unit *u) {
    free(u->tokens);
    lexer_free(&u->lexer);
    free(u->src);
}

/* Every file goes through together: declaration order does not matter in
 * slop, so sema cannot check one body until it has seen every top level. */
static int compile(char **paths, int npaths) {
    Arena *arena = arena_new(0);
    Unit *units = calloc((size_t)npaths, sizeof(Unit));
    Program **progs = calloc((size_t)npaths, sizeof(Program *));
    int nunits = 0;
    int errors = 0;
    int i;

    if (units == NULL || progs == NULL) {
        fprintf(stderr, "slop: out of memory\n");
        exit(1);
    }

    for (i = 0; i < npaths; i++) {
        Unit *u = &units[nunits];
        size_t len = 0;
        int parse_errors = 0;

        u->path = paths[i];
        u->src = source_file_read(u->path, &len);
        if (u->src == NULL) {
            errors++;
            continue;
        }
        lexer_init(&u->lexer, u->path, u->src, len);
        (void)lexer_lex_all(&u->lexer, &u->tokens, &u->ntokens);

        /* Lexical errors ride the token stream as TOK_ERROR; the parser
         * reports them as it walks past, so both kinds come out in source
         * order. */
        u->prog = parse_program(arena, u->tokens, u->ntokens, u->path, &parse_errors);
        errors += parse_errors;
        progs[nunits] = u->prog;
        nunits++;
    }

    /* A tree the parser recovered in has NULL children; sema would have to
     * guess at what they were, so it does not run at all. */
    if (errors == 0) {
        errors = sema_check(arena, progs, (size_t)nunits);
        if (errors == 0) {
            for (i = 0; i < nunits; i++) {
                ast_dump(stdout, progs[i], sema_print_type);
            }
        }
    }
    if (errors != 0) {
        fprintf(stderr, "%d error(s)\n", errors);
    }

    for (i = 0; i < nunits; i++) {
        unit_free(&units[i]);
    }
    free(units);
    free(progs);
    arena_free(arena);
    return errors != 0;
}

int main(int argc, char **argv) {
    int dump_tokens = 0;
    int files = 0;
    int failed = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) {
            dump_tokens = 1;
        } else {
            files++;
        }
    }
    if (files == 0) {
        fprintf(stderr, "usage: slop [--tokens] <file.slop>...\n");
        return 2;
    }

    if (dump_tokens) {
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--tokens") != 0) {
                failed |= lex_file(argv[i]);
            }
        }
        return failed;
    }

    {
        char **paths = calloc((size_t)files, sizeof(char *));
        int n = 0;
        if (paths == NULL) {
            fprintf(stderr, "slop: out of memory\n");
            return 1;
        }
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--tokens") != 0) {
                paths[n++] = argv[i];
            }
        }
        failed = compile(paths, n);
        free(paths);
    }
    return failed;
}
