/* slop v0 — lexes and parses a file, then dumps the AST.
 * `--tokens` stops after the lexer and dumps the token stream instead. */
#include "ast/ast_dump.h"
#include "driver/source_file.h"
#include "driver/token_dump.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "support/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int parse_file(const char *path) {
    size_t len = 0;
    char *src = source_file_read(path, &len);
    if (src == NULL) {
        return 1;
    }

    Lexer lx;
    lexer_init(&lx, path, src, len);

    Token *tokens = NULL;
    size_t count = 0;
    (void)lexer_lex_all(&lx, &tokens, &count);

    /* Lexical errors ride the token stream as TOK_ERROR; the parser reports
     * them as it walks past, so both kinds come out in source order. */
    Arena *arena = arena_new(0);
    int errors = 0;
    Program *prog = parse_program(arena, tokens, count, path, &errors);

    if (errors == 0) {
        ast_dump(stdout, prog, NULL);
    } else {
        fprintf(stderr, "%s: %d error(s)\n", path, errors);
    }

    /* The AST points into `src` for identifiers, so that buffer is freed
     * last. String literals were copied into the arena, so the tree does not
     * depend on the lexer's pool. */
    arena_free(arena);
    free(tokens);
    lexer_free(&lx);
    free(src);
    return errors != 0;
}

int main(int argc, char **argv) {
    int dump_tokens = 0;
    int files = 0;
    int failed = 0;

    for (int i = 1; i < argc; i++) {
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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) {
            continue;
        }
        failed |= dump_tokens ? lex_file(argv[i]) : parse_file(argv[i]);
    }
    return failed;
}
