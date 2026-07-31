#include "driver/token_dump.h"

#include <stdio.h>

static void print_escaped(const char *s, int32_t len) {
    for (int32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        case '\0':
            fputs("\\0", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '"':
            fputs("\\\"", stdout);
            break;
        default:
            if (c >= 0x20 && c < 0x7f) {
                putchar((int)c);
            } else {
                printf("\\x%02X", c);
            }
        }
    }
}

void token_dump(const char *file, const Token *t) {
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
