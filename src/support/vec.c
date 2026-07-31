#include "support/vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vec_init(Vec *v, size_t elem) {
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem = elem;
}

void *vec_push(Vec *v) {
    void *slot;
    if (v->len == v->cap) {
        uint32_t cap = v->cap != 0 ? v->cap * 2 : 8;
        char *data = realloc(v->data, (size_t)cap * v->elem);
        if (data == NULL) {
            fprintf(stderr, "slop: out of memory\n");
            exit(1);
        }
        v->data = data;
        v->cap = cap;
    }
    slot = v->data + (size_t)v->len * v->elem;
    memset(slot, 0, v->elem);
    v->len++;
    return slot;
}

void vec_push_ptr(Vec *v, void *item) {
    *(void **)vec_push(v) = item;
}

void *vec_take(Arena *a, Vec *v, uint32_t *out_len) {
    void *out = NULL;
    *out_len = v->len;
    if (v->len != 0) {
        out = arena_dup(a, v->data, (size_t)v->len * v->elem);
    }
    free(v->data);
    vec_init(v, v->elem);
    return out;
}

void vec_drop(Vec *v) {
    free(v->data);
    vec_init(v, v->elem);
}
