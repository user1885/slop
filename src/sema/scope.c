#include "sema/scope.h"

void globals_init(Globals *g) {
    map_init(&g->types);
    map_init(&g->values);
}

void globals_free(Globals *g) {
    map_free(&g->types);
    map_free(&g->values);
}

Ty *globals_find_type(const Globals *g, StrView name) {
    return map_get(&g->types, name);
}

Sym *globals_find_value(const Globals *g, StrView name) {
    return map_get(&g->values, name);
}

int globals_add_type(Globals *g, StrView name, Ty *type) {
    return map_put(&g->types, name, type);
}

int globals_add_value(Globals *g, StrView name, Sym *sym) {
    return map_put(&g->values, name, sym);
}

/* ------------------------------------------------------------- lexical */

static Sym **syms_at(const Scopes *sc, uint32_t i) {
    return &((Sym **)sc->syms.data)[i];
}

static uint32_t current_mark(const Scopes *sc) {
    if (sc->marks.len == 0) {
        return 0;
    }
    return ((uint32_t *)sc->marks.data)[sc->marks.len - 1];
}

void scopes_init(Scopes *sc) {
    vec_init(&sc->syms, sizeof(Sym *));
    vec_init(&sc->marks, sizeof(uint32_t));
}

void scopes_free(Scopes *sc) {
    vec_drop(&sc->syms);
    vec_drop(&sc->marks);
}

void scope_push(Scopes *sc) {
    uint32_t *mark = vec_push(&sc->marks);
    *mark = sc->syms.len;
}

void scope_pop(Scopes *sc) {
    if (sc->marks.len == 0) {
        return;
    }
    sc->syms.len = current_mark(sc);
    sc->marks.len--;
}

Sym *scope_find(const Scopes *sc, StrView name) {
    uint32_t i = sc->syms.len;
    while (i > 0) {
        Sym *s = *syms_at(sc, --i);
        if (strview_eq_view(s->name, name)) {
            return s;
        }
    }
    return NULL;
}

Sym *scope_find_current(const Scopes *sc, StrView name) {
    uint32_t start = current_mark(sc);
    uint32_t i = sc->syms.len;
    while (i > start) {
        Sym *s = *syms_at(sc, --i);
        if (strview_eq_view(s->name, name)) {
            return s;
        }
    }
    return NULL;
}

void scope_declare(Scopes *sc, Sym *sym) {
    vec_push_ptr(&sc->syms, sym);
}
