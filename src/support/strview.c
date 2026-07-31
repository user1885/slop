#include "support/strview.h"

#include <string.h>

int strview_eq(StrView v, const char *cstr) {
    size_t n = strlen(cstr);
    if (v.len < 0 || (size_t)v.len != n) {
        return 0;
    }
    return memcmp(v.data, cstr, n) == 0;
}

int strview_eq_view(StrView a, StrView b) {
    return a.len == b.len && memcmp(a.data, b.data, (size_t)(a.len > 0 ? a.len : 0)) == 0;
}
