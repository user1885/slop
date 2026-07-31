/* slop v0 — for now just a token dumper for the lexer. */
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: slop <file.slop>...\n");
        return 2;
    }

    int failed = 0;
    for (int i = 1; i < argc; i++) {
        failed |= lex_file(argv[i]);
    }
    return failed;
}
