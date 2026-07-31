#ifndef SLOP_ARENA_H
#define SLOP_ARENA_H

#include <stddef.h>

/* Bump allocator. Nothing is freed individually; the whole arena dies at
 * once. The AST, the token buffers and every string the front end interns
 * live here, so no pass ever has to own or copy a node. */
typedef struct Arena Arena;

/* block_size == 0 selects the default (64 KiB). Allocations larger than a
 * block get a block of their own. Dies on OOM. */
Arena *arena_new(size_t block_size);
void arena_free(Arena *a);

/* Zeroed and suitably aligned for any type. */
void *arena_alloc(Arena *a, size_t size);
void *arena_dup(Arena *a, const void *src, size_t size);
char *arena_strndup(Arena *a, const char *src, size_t len);

/* Total bytes handed out, for diagnostics. */
size_t arena_used(const Arena *a);

#endif /* SLOP_ARENA_H */
