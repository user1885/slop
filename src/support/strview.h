#ifndef SLOP_SUPPORT_STRVIEW_H
#define SLOP_SUPPORT_STRVIEW_H

#include <stdint.h>

/* A borrowed slice of text. Whoever hands one out says how long the memory
 * behind it lives; a StrView never owns it and is not NUL-terminated unless
 * its producer says so. */
typedef struct {
    const char *data;
    int32_t len;
} StrView;

int strview_eq(StrView v, const char *cstr);
int strview_eq_view(StrView a, StrView b);

#endif /* SLOP_SUPPORT_STRVIEW_H */
