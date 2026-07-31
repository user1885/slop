#ifndef SLOP_SUPPORT_DIAG_H
#define SLOP_SUPPORT_DIAG_H

#include "support/srcpos.h"

/* Error reporting for one pass over one file.
 *
 * A pass owns a Diag, reports through it and reads the count at the end.
 * Keeping the counter and the printing together is what lets a pass keep
 * going after an error instead of stopping at the first one: the tree it
 * produces is known to be untrustworthy without every caller having to
 * thread a bool through its recursion. */

/* Past this many, errors are counted but no longer printed. A file with
 * hundreds of them is one mistake plus cascade, and the cascade is noise. */
#define DIAG_MAX_REPORTED 20

typedef struct {
    const char *file; /* used when a position carries no file of its own */
    int count;        /* errors seen */
    int reported;     /* errors printed, plus one for the cutoff notice */
    int last_printed; /* did the most recent error reach stderr? */
} Diag;

void diag_init(Diag *d, const char *file);

/* Prints `file:line:col: error: ...` to stderr and bumps the count. */
void diag_error(Diag *d, SrcPos pos, const char *fmt, ...);

/* A follow-up line for the error just reported: where the first declaration
 * was, where a binding was introduced. Never counts as an error, and prints
 * only when that error printed, so a suppressed cascade does not leak
 * notes. */
void diag_note(Diag *d, SrcPos pos, const char *fmt, ...);

int diag_error_count(const Diag *d);

#endif /* SLOP_SUPPORT_DIAG_H */
