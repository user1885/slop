#include "support/strview.h"

#include <string.h>

int strview_eq(StrView v, const char *cstr) {
    size_t n = strlen(cstr);
    if (v.len < 0 || (size_t)v.len != n) {
        return 0;
    }
    return memcmp(v.data, cstr, n) == 0;
}
