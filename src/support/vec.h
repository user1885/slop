#ifndef SLOP_SUPPORT_VEC_H
#define SLOP_SUPPORT_VEC_H

#include "support/arena.h"

#include <stddef.h>
#include <stdint.h>

/* A growable scratch array for building node lists whose final length is
 * only known at the end.
 *
 * It is malloc-backed on purpose: growing means reallocating, and an arena
 * cannot reclaim the abandoned block. vec_take() copies the finished run
 * into the arena and drops the scratch buffer, so the arena only ever sees
 * exact-sized allocations.
 *
 * Slots are handed out zeroed, and a pointer from vec_push() is only valid
 * until the next push. */
typedef struct {
    char *data;
    uint32_t len;
    uint32_t cap;
    size_t elem;
} Vec;

void vec_init(Vec *v, size_t elem);

/* Appends one zeroed slot and returns it. Dies on OOM. */
void *vec_push(Vec *v);
void vec_push_ptr(Vec *v, void *item);

/* Copies the contents into `a`, releases the scratch buffer and leaves the
 * Vec empty and reusable. Returns NULL for an empty Vec. */
void *vec_take(Arena *a, Vec *v, uint32_t *out_len);

/* Throws the contents away -- for an error path that abandons what it has
 * collected so far. */
void vec_drop(Vec *v);

#endif /* SLOP_SUPPORT_VEC_H */
