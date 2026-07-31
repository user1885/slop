#ifndef SLOP_SUPPORT_SRCPOS_H
#define SLOP_SUPPORT_SRCPOS_H

#include <stdint.h>

/* Where something in the source came from. `file` is the path the pass was
 * handed and is borrowed, not owned; line and column are 1-based and the
 * column counts bytes, matching what the lexer reports. */
typedef struct {
    const char *file;
    int32_t line;
    int32_t col;
} SrcPos;

#endif /* SLOP_SUPPORT_SRCPOS_H */
