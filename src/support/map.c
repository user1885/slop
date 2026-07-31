#include "support/map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_INITIAL_CAP 64u

static uint64_t hash_key(StrView k) {
    uint64_t h = 0xCBF29CE484222325ull;
    int32_t i;
    for (i = 0; i < k.len; i++) {
        h ^= (unsigned char)k.data[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static int key_eq(StrView a, StrView b) {
    return a.len == b.len && memcmp(a.data, b.data, (size_t)a.len) == 0;
}

void map_init(Map *m) {
    m->keys = NULL;
    m->vals = NULL;
    m->cap = 0;
    m->len = 0;
}

void map_free(Map *m) {
    free(m->keys);
    free(m->vals);
    map_init(m);
}

/* Where `key` lives, or where it would go. The table is never full when this
 * is called -- grow() keeps at least a quarter of the slots empty -- so the
 * probe always terminates. */
static uint32_t slot_of(const StrView *keys, uint32_t cap, StrView key) {
    uint32_t mask = cap - 1;
    uint32_t i = (uint32_t)hash_key(key) & mask;
    while (keys[i].data != NULL && !key_eq(keys[i], key)) {
        i = (i + 1) & mask;
    }
    return i;
}

static void map_grow(Map *m) {
    uint32_t new_cap = m->cap != 0 ? m->cap * 2 : MAP_INITIAL_CAP;
    StrView *keys = calloc(new_cap, sizeof(StrView));
    void **vals = calloc(new_cap, sizeof(void *));
    uint32_t i;

    if (keys == NULL || vals == NULL) {
        fprintf(stderr, "slop: out of memory\n");
        exit(1);
    }
    for (i = 0; i < m->cap; i++) {
        if (m->keys[i].data != NULL) {
            uint32_t j = slot_of(keys, new_cap, m->keys[i]);
            keys[j] = m->keys[i];
            vals[j] = m->vals[i];
        }
    }
    free(m->keys);
    free(m->vals);
    m->keys = keys;
    m->vals = vals;
    m->cap = new_cap;
}

void *map_get(const Map *m, StrView key) {
    uint32_t i;
    if (m->len == 0) {
        return NULL;
    }
    i = slot_of(m->keys, m->cap, key);
    return m->keys[i].data != NULL ? m->vals[i] : NULL;
}

int map_put(Map *m, StrView key, void *val) {
    uint32_t i;

    /* Grow at three quarters full: linear probing degrades badly past that,
     * and the front end inserts far more often than it grows. */
    if (m->cap == 0 || (m->len + 1) * 4 > m->cap * 3) {
        map_grow(m);
    }
    i = slot_of(m->keys, m->cap, key);
    if (m->keys[i].data != NULL) {
        return 0;
    }
    m->keys[i] = key;
    m->vals[i] = val;
    m->len++;
    return 1;
}

uint32_t map_len(const Map *m) {
    return m->len;
}
