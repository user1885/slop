/* slop v0 — lexes and parses a file, then dumps the AST.
 * `--tokens` stops after the lexer and dumps the token stream instead. */
#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "slop: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "slop: cannot seek %s\n", path);
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "slop: cannot size %s\n", path);
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fprintf(stderr, "slop: out of memory\n");
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static void print_escaped(const char *s, int32_t len) {
    for (int32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        case '\0': fputs("\\0", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '"': fputs("\\\"", stdout); break;
        default:
            if (c >= 0x20 && c < 0x7f) {
                putchar((int)c);
            } else {
                printf("\\x%02X", c);
            }
        }
    }
}

static void print_token(const char *file, const Token *t) {
    if (t->kind == TOK_ERROR) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", file, t->line, t->col, t->val.err);
        return;
    }

    printf("%4d:%-3d %-16s ", t->line, t->col, token_kind_name(t->kind));
    switch (t->kind) {
    case TOK_EOF:
        break;
    case TOK_INT:
        printf("%.*s  = %llu", t->len, t->text, (unsigned long long)t->val.ival);
        break;
    case TOK_FLOAT:
        printf("%.*s  = %g", t->len, t->text, t->val.fval);
        break;
    case TOK_CHAR:
        printf("%.*s  = %llu", t->len, t->text, (unsigned long long)t->val.ival);
        break;
    case TOK_STRING:
        printf("%.*s  = \"", t->len, t->text);
        print_escaped(t->val.str.ptr, t->val.str.len);
        printf("\" (%d bytes)", t->val.str.len);
        break;
    default:
        printf("%.*s", t->len, t->text);
        break;
    }
    putchar('\n');
}

static int lex_file(const char *path) {
    size_t len = 0;
    char *src = read_file(path, &len);
    if (src == NULL) {
        return 1;
    }

    Lexer lx;
    lexer_init(&lx, path, src, len);

    Token *tokens = NULL;
    size_t count = 0;
    int32_t errors = lexer_lex_all(&lx, &tokens, &count);

    for (size_t i = 0; i < count; i++) {
        print_token(path, &tokens[i]);
    }
    printf("%s: %zu tokens, %d error(s)\n", path, count, errors);

    free(tokens);
    lexer_free(&lx);
    free(src);
    return errors != 0;
}

static int parse_file(const char *path) {
    size_t len = 0;
    char *src = read_file(path, &len);
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
        ast_dump(stdout, prog);
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
