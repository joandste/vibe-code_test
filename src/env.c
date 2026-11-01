/*
 * env.c - lexical environments.
 *
 * MIT License. See LICENSE.
 *
 * A frame owns a linked list of bindings ([KW-002], [KW-003], [KW-007]).
 * `def` at the top level pushes a *new* frame instead of replacing an existing
 * binding, so closures created before a redefinition keep observing the value
 * that was visible when they were created ([KW-002]).
 *
 * A lookup that fails in the captured chain falls back to the newest top-level
 * frame. That keeps forward references working:
 *
 *   (defn f [] (g))   ; g is not defined yet
 *   (defn g [] 42)
 *   (f)               ; => 42
 */
#include "internal.h"

#include <string.h>

Env *env_new(Lang *L, Env *parent) {
    Env *e = lang_alloc(L, sizeof *e);
    e->parent = parent;
    return e;
}

Binding *env_define(Lang *L, Env *env, const char *name, Value v, bool mutable_,
                    bool is_macro) {
    Binding *b = lang_alloc(L, sizeof *b);
    b->name = name;
    b->value = v;
    b->mutable_ = mutable_;
    b->is_macro = is_macro;
    b->defined = true;
    b->next = env->bindings;
    env->bindings = b;
    return b;
}

static Binding *find_in_chain(Env *env, const char *name) {
    for (; env; env = env->parent) {
        for (Binding *b = env->bindings; b; b = b->next) {
            if (b->name == name) return b;
        }
    }
    return NULL;
}

Binding *env_find(Lang *L, Env *env, const char *name) {
    Binding *b = find_in_chain(env, name);
    if (b) return b;
    if (env != L->global) return find_in_chain(L->global, name);
    return NULL;
}
