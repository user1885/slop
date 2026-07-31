#include "support/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_DEFAULT_BLOCK (64u * 1024u)
#define ARENA_ALIGN         16u

typedef struct Block {
    struct Block *next;
    size_t used;
    size_t cap;
    /* payload follows */
} Block;

struct Arena {
    Block *head;
    size_t block_size;
    size_t used;
};

static size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static void *block_data(Block *b) {
    return (char *)b + align_up(sizeof(Block), ARENA_ALIGN);
}

static Block *block_new(size_t cap) {
    size_t header = align_up(sizeof(Block), ARENA_ALIGN);
    Block *b = malloc(header + cap);
    if (b == NULL) {
        fprintf(stderr, "slop: out of memory (%zu bytes)\n", header + cap);
        exit(1);
    }
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

Arena *arena_new(size_t block_size) {
    Arena *a = malloc(sizeof(Arena));
    if (a == NULL) {
        fprintf(stderr, "slop: out of memory\n");
        exit(1);
    }
    a->block_size = block_size != 0 ? block_size : ARENA_DEFAULT_BLOCK;
    a->head = block_new(a->block_size);
    a->used = 0;
    return a;
}

void arena_free(Arena *a) {
    Block *b;
    if (a == NULL) {
        return;
    }
    b = a->head;
    while (b != NULL) {
        Block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

void *arena_alloc(Arena *a, size_t size) {
    size_t need = align_up(size, ARENA_ALIGN);
    char *p;

    if (need == 0) {
        need = ARENA_ALIGN;
    }
    if (a->head->used + need > a->head->cap) {
        size_t cap = need > a->block_size ? need : a->block_size;
        Block *b = block_new(cap);
        b->next = a->head;
        a->head = b;
    }
    p = (char *)block_data(a->head) + a->head->used;
    a->head->used += need;
    a->used += need;
    memset(p, 0, size);
    return p;
}

void *arena_dup(Arena *a, const void *src, size_t size) {
    void *p;
    if (size == 0) {
        return NULL;
    }
    p = arena_alloc(a, size);
    memcpy(p, src, size);
    return p;
}

char *arena_strndup(Arena *a, const char *src, size_t len) {
    char *p = arena_alloc(a, len + 1);
    if (len != 0) {
        memcpy(p, src, len);
    }
    p[len] = '\0';
    return p;
}

size_t arena_used(const Arena *a) {
    return a->used;
}
