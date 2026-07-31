#include "backend/backend.h"

#include <string.h>

/* Defined by each backend's own file. Adding a backend means adding one
 * extern here and one row below — there is no registration protocol to get
 * wrong, and the linker catches a backend that was declared but not built. */
extern const Backend backend_llvm;
extern const Backend backend_c;

/* The first entry is the default. */
static const Backend *const backends[] = {
    &backend_llvm,
    &backend_c,
};

#define BACKEND_COUNT (sizeof backends / sizeof backends[0])

const Backend *backend_find(const char *name) {
    size_t i;
    for (i = 0; i < BACKEND_COUNT; i++) {
        if (strcmp(backends[i]->name, name) == 0) {
            return backends[i];
        }
    }
    return NULL;
}

const Backend *backend_default(void) {
    return backends[0];
}

size_t backend_count(void) {
    return BACKEND_COUNT;
}

const Backend *backend_at(size_t i) {
    return i < BACKEND_COUNT ? backends[i] : NULL;
}

void backend_list(FILE *out) {
    size_t i;
    for (i = 0; i < BACKEND_COUNT; i++) {
        fprintf(out, "  %-6s %-10s %s%s\n", backends[i]->name, backends[i]->extension,
                backends[i]->description, i == 0 ? " (default)" : "");
    }
}
