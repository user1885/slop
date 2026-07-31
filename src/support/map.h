#ifndef SLOP_SUPPORT_MAP_H
#define SLOP_SUPPORT_MAP_H

#include "support/strview.h"

#include <stddef.h>
#include <stdint.h>

/* A hash map from a name to a pointer the caller owns.
 *
 * Open addressing with linear probing, FNV-1a over the key bytes, and no
 * deletion -- nothing in a compiler front end ever un-declares a name. The
 * table itself is malloc-backed because it rehashes as it grows; what it
 * points at is expected to live in an arena.
 *
 * A key must have a non-NULL `data`, which every name lexed out of a source
 * buffer does: an empty slot is exactly a NULL key. */
typedef struct {
    StrView *keys;
    void **vals;
    uint32_t cap; /* a power of two, or 0 before the first insert */
    uint32_t len;
} Map;

void map_init(Map *m);
void map_free(Map *m);

/* The stored pointer, or NULL when the key is absent. */
void *map_get(const Map *m, StrView key);

/* Inserts unless the key is already present. Returns 1 when it inserted and
 * 0 when the key was taken, which is what makes duplicate detection a single
 * call with no separate lookup. */
int map_put(Map *m, StrView key, void *val);

uint32_t map_len(const Map *m);

#endif /* SLOP_SUPPORT_MAP_H */
