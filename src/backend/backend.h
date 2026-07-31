/* The backend interface.
 *
 * A backend is a name and a function that turns a verified IrModule into
 * text. That is the whole contract — a backend cannot see the AST, the token
 * stream or sema's tables, so adding one can never disturb the front end,
 * and swapping one is a command-line flag.
 *
 * To add a backend:
 *
 *   1. write `int emit(const IrModule *, FILE *)` in src/backend/,
 *   2. add one line to the table in backend.c,
 *   3. add its source to CMakeLists.txt.
 *
 * There is deliberately no registration call, no constructor and no dynamic
 * loading: the set of backends is known at compile time, so a static table
 * is both the simplest and the most legible way to say what exists.
 */
#ifndef SLOP_BACKEND_H
#define SLOP_BACKEND_H

#include "ir/ir.h"

#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *name;        /* as spelled in --backend=<name> */
    const char *description; /* one line, for --list-backends */
    const char *extension;   /* conventional output suffix, including the dot */
    /* Writes the module to `out`. Returns 0 on success. The module has been
     * verified; a backend is not expected to defend against a malformed one,
     * but it must not crash on a construct it does not support — report and
     * return non-zero instead. */
    int (*emit)(const IrModule *module, FILE *out);
} Backend;

/* NULL if there is no backend by that name. */
const Backend *backend_find(const char *name);

/* The one used when --backend is not given. */
const Backend *backend_default(void);

/* Every backend, in table order, for --list-backends and for tests that want
 * to run all of them without knowing what they are. */
size_t backend_count(void);
const Backend *backend_at(size_t i);

/* One line per backend, as --list-backends prints it. */
void backend_list(FILE *out);

#endif /* SLOP_BACKEND_H */
