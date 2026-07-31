#include "support/diag.h"

#include <stdarg.h>
#include <stdio.h>

void diag_init(Diag *d, const char *file) {
    d->file = file;
    d->count = 0;
    d->reported = 0;
}

void diag_error(Diag *d, SrcPos pos, const char *fmt, ...) {
    va_list ap;

    d->count++;
    if (d->reported >= DIAG_MAX_REPORTED) {
        if (d->reported == DIAG_MAX_REPORTED) {
            d->reported++;
            fprintf(stderr, "%s: too many errors; further diagnostics suppressed\n", d->file);
        }
        return;
    }
    d->reported++;

    fprintf(stderr, "%s:%d:%d: error: ", pos.file != NULL ? pos.file : d->file, pos.line, pos.col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void diag_note(Diag *d, SrcPos pos, const char *fmt, ...) {
    va_list ap;

    if (!d->last_printed) {
        return;
    }
    fprintf(stderr, "%s:%d:%d: note: ", pos.file != NULL ? pos.file : d->file, pos.line, pos.col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int diag_error_count(const Diag *d) {
    return d->count;
}
