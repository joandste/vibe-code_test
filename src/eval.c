/*
 * eval.c - tree-walking evaluator: special forms, macros and `recur`.
 *
 * MIT License. See LICENSE.
 *
 * The evaluator works directly on the runtime values produced by the reader
 * ([P-007]): a paren tuple whose head is a special form or a macro is
 * dispatched at evaluation time, everything else is a call.
 *
 * `recur` is validated when a `fn` value is created ([I-004]-[I-007]); the
 * evaluation of `recur` hands control back to the function's call loop, so the
 * jump does not grow the C stack ([I-005]).
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CALL_DEPTH 2000
#define MAX_MACRO_DEPTH 1000

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static bool sym_is(Value v, Value sym) {
    return v.tag == V_SYMBOL && v.as.str.data == sym.as.str.data;
}

static bool is_reserved(Lang *L, Value v) {
    if (v.tag != V_SYMBOL) return false;
    const Value *reserved[] = {
        &L->s_def,       &L->s_var,  &L->s_set,         &L->s_fn,
        &L->s_defn,      &L->s_defmacro, &L->s_macroexpand, &L->s_if,
        &L->s_do,        &L->s_let,  &L->s_recur,       &L->s_quote,
        &L->s_quasiquote, &L->s_unquote, &L->s_and,     &L->s_or,
        &L->s_amp,       &L->s_ampopt};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
        if (v.as.str.data == reserved[i]->as.str.data) return true;
    return false;
}

static Value eval_symbol(Lang *L, Value sym, Env *env) {
    Binding *b = env_find(L, env, sym.as.str.data);
    if (!b)
        lang_error(L, "unbound symbol '%s'", sym.as.str.data);
    if (b->is_macro)
        lang_error(L, "macro '%s' used as a value", sym.as.str.data);
    if (!b->defined)
        lang_error(L, "symbol '%s' used before its definition", sym.as.str.data);
    return b->value;
}

/* Is `env` one of the top-level frames? Used by `def` to publish new
 * top-level bindings so that forward references resolve ([KW-002]). */
static bool is_global_frame(Lang *L, Env *env) {
    for (Env *e = L->global; e; e = e->parent)
        if (e == env) return true;
    return false;
}

/* Copy forms [from, len) of `form` into a mutable array value. */
static Value forms_slice(Lang *L, Value form, size_t from) {
    size_t n = seq_len(form);
    size_t count = n > from ? n - from : 0;
    Value *elems = count ? lang_alloc(L, count * sizeof(Value)) : NULL;
    for (size_t i = 0; i < count; i++) elems[i] = seq_at(form, from + i);
    return v_seq_from(L, elems, count, true, false);
}

/* ------------------------------------------------------------------ */
/* Parameters ([KW-005], §9.5)                                        */
/* ------------------------------------------------------------------ */

typedef struct Params {
    int nfixed, nopt;
    bool has_rest;
    int total;
    Value *names;
} Params;

static void check_param_name(Lang *L, Value name, Params *p, int count) {
    for (int i = 0; i < count; i++) {
        if (p->names[i].as.str.data == name.as.str.data)
            lang_error(L, "duplicate parameter name '%s'", name.as.str.data);
    }
    if (is_reserved(L, name))
        lang_error(L, "reserved symbol '%s' cannot be a parameter",
                   name.as.str.data);
}

static void parse_params(Lang *L, Value params, Params *p) {
    memset(p, 0, sizeof *p);
    if (!(params.tag == V_TUPLE && !params.as.seq->paren))
        lang_error(L, "parameter list must be a bracketed tuple");
    size_t n = params.as.seq->len;
    p->names = n ? lang_alloc(L, n * sizeof(Value)) : NULL;

    int phase = 0; /* 0 = fixed, 1 = optional, 2 = rest seen */
    int count = 0;
    for (size_t i = 0; i < n;) {
        Value v = seq_at(params, i);
        if (v.tag != V_SYMBOL)
            lang_error(L, "parameter must be a symbol");
        if (sym_is(v, L->s_ampopt)) {
            if (phase != 0)
                lang_error(L, "'&opt' may appear only once and before '&'");
            phase = 1;
            i++;
            continue;
        }
        if (sym_is(v, L->s_amp)) {
            if (phase == 2) lang_error(L, "'&' may appear only once");
            if (i + 1 >= n)
                lang_error(L, "'&' must be followed by a parameter name");
            Value rest = seq_at(params, i + 1);
            if (rest.tag != V_SYMBOL)
                lang_error(L, "rest parameter must be a symbol");
            if (i + 2 != n)
                lang_error(L, "rest parameter must be the last parameter");
            check_param_name(L, rest, p, count);
            p->names[count++] = rest;
            p->has_rest = true;
            phase = 2;
            i += 2;
            continue;
        }
        check_param_name(L, v, p, count);
        p->names[count++] = v;
        if (phase == 0)
            p->nfixed++;
        else if (phase == 1)
            p->nopt++;
        else
            lang_error(L, "unexpected parameter after '&'");
        i++;
    }
    p->total = count;
}

/* ------------------------------------------------------------------ */
/* Static validation of `recur` ([I-004]-[I-008])                     */
/* ------------------------------------------------------------------ */

static void check_form(Lang *L, Value f, bool tail, bool in_fn, bool in_macro,
                       int nparams);

static void check_body(Lang *L, Value body, bool in_fn, bool in_macro,
                       int nparams) {
    size_t n = seq_len(body);
    for (size_t i = 0; i < n; i++)
        check_form(L, seq_at(body, i), i + 1 == n, in_fn, in_macro, nparams);
}

static void check_fn_forms(Lang *L, Value form, size_t params_index,
                           bool in_macro) {
    Params p;
    parse_params(L, seq_at(form, params_index), &p);
    size_t n = seq_len(form);
    for (size_t i = params_index + 1; i < n; i++)
        check_form(L, seq_at(form, i), i + 1 == n, true, in_macro, p.total);
}

static void check_form(Lang *L, Value f, bool tail, bool in_fn, bool in_macro,
                       int nparams) {
    if (f.tag == V_SYMBOL) {
        if (sym_is(f, L->s_recur)) {
            if (in_macro)
                lang_error(L, "recur is not allowed inside a macro");
            if (!in_fn) lang_error(L, "recur used outside of a fn");
            if (!tail) lang_error(L, "recur used in non-tail position");
        }
        return;
    }
    if (f.tag == V_TUPLE && f.as.seq->paren) {
        size_t n = seq_len(f);
        if (n == 0) return;
        Value head = seq_at(f, 0);
        if (head.tag == V_SYMBOL) {
            const char *nm = head.as.str.data;
            if (nm == L->s_recur.as.str.data) {
                if (in_macro)
                    lang_error(L, "recur is not allowed inside a macro");
                if (!in_fn) lang_error(L, "recur used outside of a fn");
                if (!tail) lang_error(L, "recur used in non-tail position");
                int argc = (int)n - 1;
                if (argc != nparams)
                    lang_error(L, "recur expects %d arguments, got %d", nparams,
                               argc);
                for (size_t i = 1; i < n; i++)
                    check_form(L, seq_at(f, i), false, in_fn, in_macro, nparams);
                return;
            }
            if (nm == L->s_quote.as.str.data ||
                nm == L->s_quasiquote.as.str.data ||
                nm == L->s_macroexpand.as.str.data)
                return;
            if (nm == L->s_if.as.str.data) {
                if (n >= 2)
                    check_form(L, seq_at(f, 1), false, in_fn, in_macro, nparams);
                if (n >= 3)
                    check_form(L, seq_at(f, 2), tail, in_fn, in_macro, nparams);
                if (n >= 4)
                    check_form(L, seq_at(f, 3), tail, in_fn, in_macro, nparams);
                return;
            }
            if (nm == L->s_do.as.str.data) {
                for (size_t i = 1; i < n; i++)
                    check_form(L, seq_at(f, i), tail && i + 1 == n, in_fn,
                               in_macro, nparams);
                return;
            }
            if (nm == L->s_let.as.str.data) {
                if (n >= 2) check_form(L, seq_at(f, 1), false, in_fn, in_macro, nparams);
                for (size_t i = 2; i < n; i++)
                    check_form(L, seq_at(f, i), tail && i + 1 == n, in_fn,
                               in_macro, nparams);
                return;
            }
            if (nm == L->s_and.as.str.data || nm == L->s_or.as.str.data) {
                for (size_t i = 1; i < n; i++)
                    check_form(L, seq_at(f, i), tail && i + 1 == n, in_fn,
                               in_macro, nparams);
                return;
            }
            if (nm == L->s_def.as.str.data || nm == L->s_var.as.str.data ||
                nm == L->s_set.as.str.data) {
                if (n >= 3)
                    check_form(L, seq_at(f, 2), false, in_fn, in_macro, nparams);
                return;
            }
            if (nm == L->s_fn.as.str.data || nm == L->s_defn.as.str.data ||
                nm == L->s_defmacro.as.str.data) {
                size_t pidx = (nm == L->s_fn.as.str.data) ? 1 : 2;
                if (n > pidx)
                    check_fn_forms(L, f, pidx,
                                   nm == L->s_defmacro.as.str.data);
                return;
            }
        }
        for (size_t i = 0; i < n; i++)
            check_form(L, seq_at(f, i), false, in_fn, in_macro, nparams);
        return;
    }
    if (f.tag == V_TUPLE || f.tag == V_ARRAY || f.tag == V_STRUCT ||
        f.tag == V_TABLE) {
        if (f.tag == V_STRUCT || f.tag == V_TABLE) {
            for (size_t i = 0; i < f.as.map->n; i++) {
                check_form(L, f.as.map->entries[i].key, false, in_fn, in_macro,
                           nparams);
                check_form(L, f.as.map->entries[i].val, false, in_fn, in_macro,
                           nparams);
            }
        } else {
            size_t n = seq_len(f);
            for (size_t i = 0; i < n; i++)
                check_form(L, seq_at(f, i), false, in_fn, in_macro, nparams);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Application                                                        */
/* ------------------------------------------------------------------ */

static Value call_closure(Lang *L, FnObj *f, int argc, Value *argv);

static Value apply_value(Lang *L, Value fnv, int argc, Value *argv) {
    FnObj *f = fnv.as.fn;
    if (f->native) return f->native(L, f, argc, argv);
    return call_closure(L, f, argc, argv);
}

Value lang_apply(Lang *L, Value fn, int argc, Value *argv) {
    if (fn.tag != V_FN) {
        char buf[128];
        value_repr_into(L, fn, buf, sizeof buf);
        lang_error(L, "attempt to call a non-function: %s", buf);
    }
    if (fn.as.fn->is_macro) lang_error(L, "macro used as a value");
    return apply_value(L, fn, argc, argv);
}

Value lang_apply_macro(Lang *L, Value fn, int argc, Value *argv) {
    return apply_value(L, fn, argc, argv);
}

static Value call_closure(Lang *L, FnObj *f, int argc, Value *argv) {
    bool from_recur = false;
    for (;;) {
        if (argc < f->nfixed || (!f->has_rest && argc > f->nfixed + f->nopt)) {
            if (f->has_rest)
                lang_error(L, "arity mismatch in %s: expected at least %d "
                              "argument(s), got %d",
                           f->name ? f->name : "fn", f->nfixed, argc);
            lang_error(L, "arity mismatch in %s: expected %d to %d "
                          "argument(s), got %d",
                       f->name ? f->name : "fn", f->nfixed,
                       f->nfixed + f->nopt, argc);
        }
        Env *env = env_new(L, f->env);
        int i = 0;
        for (int k = 0; k < f->nfixed; k++, i++)
            env_define(L, env, f->pnames[i].as.str.data, argv[i], false, false);
        for (int k = 0; k < f->nopt; k++, i++) {
            Value v = i < argc ? argv[i] : v_nil();
            env_define(L, env, f->pnames[i].as.str.data, v, false, false);
        }
        if (f->has_rest) {
            Value rv;
            int restc = argc - i;
            if (from_recur || restc <= 0)
                rv = from_recur ? argv[i] : v_nil();
            else
                rv = v_seq_over(L, argv, (size_t)argc, (size_t)i, (size_t)restc,
                                true, false);
            env_define(L, env, f->pnames[i].as.str.data, rv, false, false);
            i++;
        }

        if (L->call_depth >= MAX_CALL_DEPTH)
            lang_error(L, "call depth limit (%d) exceeded; ordinary calls grow "
                          "the stack, use recur for iteration",
                       MAX_CALL_DEPTH);
        L->call_depth++;
        FnObj *saved_fn = L->active_fn;
        L->active_fn = f;

        Value result = v_nil();
        size_t n = seq_len(f->body);
        for (size_t k = 0; k < n; k++)
            result = lang_eval_form(L, seq_at(f->body, k), &env, k + 1 == n);

        L->active_fn = saved_fn;
        L->call_depth--;

        if (result.tag == V_RECUR) {
            argc = L->recur_argc;
            argv = L->recur_argv;
            from_recur = true;
            continue;
        }
        return result;
    }
}

/* ------------------------------------------------------------------ */
/* Forward declarations for special forms                             */
/* ------------------------------------------------------------------ */

static Value sf_if(Lang *L, Value form, Env **env, bool tail);
static Value sf_do(Lang *L, Value form, Env **env, bool tail);
static Value sf_let(Lang *L, Value form, Env **env, bool tail);
static Value sf_and(Lang *L, Value form, Env **env, bool tail);
static Value sf_or(Lang *L, Value form, Env **env, bool tail);

/* ------------------------------------------------------------------ */
/* Quasiquote ([KW-009] family; semantics not yet fixed by the spec)  */
/* ------------------------------------------------------------------ */

static Value qq(Lang *L, Value form, Env **env, int depth);

static Value qq_rebuild(Lang *L, Value form, Env **env, int depth) {
    if (form.tag == V_TUPLE || form.tag == V_ARRAY) {
        SeqObj *s = form.as.seq;
        Value *elems =
            s->len ? lang_alloc(L, s->len * sizeof(Value)) : NULL;
        for (size_t i = 0; i < s->len; i++)
            elems[i] = qq(L, seq_at(form, i), env, depth);
        return v_seq_from(L, elems, s->len, s->mutable_, s->paren);
    }
    if (form.tag == V_STRUCT || form.tag == V_TABLE) {
        Value m = v_map_empty(L, form.tag == V_TABLE);
        MapObj *src = form.as.map;
        for (size_t i = 0; i < src->n; i++) {
            Value k = qq(L, src->entries[i].key, env, depth);
            Value v = qq(L, src->entries[i].val, env, depth);
            map_put(L, m, k, v);
        }
        return m;
    }
    return form;
}

static Value qq(Lang *L, Value form, Env **env, int depth) {
    if (form.tag == V_TUPLE && form.as.seq->paren && seq_len(form) > 0) {
        Value head = seq_at(form, 0);
        if (sym_is(head, L->s_unquote)) {
            if (seq_len(form) != 2)
                lang_error(L, "unquote expects exactly one argument");
            if (depth == 1)
                return lang_eval_form(L, seq_at(form, 1), env, false);
            Value *elems = lang_alloc(L, 2 * sizeof(Value));
            elems[0] = head;
            elems[1] = qq(L, seq_at(form, 1), env, depth - 1);
            return v_seq_from(L, elems, 2, false, true);
        }
        if (sym_is(head, L->s_quasiquote)) {
            if (seq_len(form) != 2)
                lang_error(L, "quasiquote expects exactly one argument");
            Value *elems = lang_alloc(L, 2 * sizeof(Value));
            elems[0] = head;
            elems[1] = qq(L, seq_at(form, 1), env, depth + 1);
            return v_seq_from(L, elems, 2, false, true);
        }
    }
    return qq_rebuild(L, form, env, depth);
}

/* ------------------------------------------------------------------ */
/* Function construction                                              */
/* ------------------------------------------------------------------ */

static Value make_fn(Lang *L, Value form, size_t params_index,
                     const char *name, Env *env, bool is_macro) {
    Params p;
    parse_params(L, seq_at(form, params_index), &p);
    Value body = forms_slice(L, form, params_index + 1);
    check_body(L, body, true, is_macro, p.total);

    FnObj *f = lang_alloc(L, sizeof *f);
    f->env = env;
    f->body = body;
    f->nfixed = p.nfixed;
    f->nopt = p.nopt;
    f->has_rest = p.has_rest;
    f->nparams_total = p.total;
    f->pnames = p.names;
    f->is_macro = is_macro;
    f->name = name ? lang_strndup(L, name, strlen(name)) : NULL;

    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_FN;
    v.as.fn = f;
    return v;
}

/* ------------------------------------------------------------------ */
/* Special forms                                                      */
/* ------------------------------------------------------------------ */

static Value sf_def(Lang *L, Value form, Env **env, bool mutable_) {
    if (seq_len(form) != 3)
        lang_error(L, "%s expects exactly 2 arguments, got %zu",
                   mutable_ ? "var" : "def", seq_len(form) - 1);
    Value name = seq_at(form, 1);
    if (name.tag != V_SYMBOL) lang_error(L, "%s name must be a symbol",
                                         mutable_ ? "var" : "def");
    if (is_reserved(L, name))
        lang_error(L, "reserved symbol '%s' cannot be redefined",
                   name.as.str.data);
    Env *newenv = env_new(L, *env);
    Binding *b = env_define(L, newenv, name.as.str.data, v_nil(), mutable_,
                            false);
    b->defined = false;
    Value val = lang_eval_form(L, seq_at(form, 2), &newenv, false);
    b->value = val;
    b->defined = true;
    if (is_global_frame(L, newenv->parent)) L->global = newenv;
    *env = newenv;
    return val;
}

static Value sf_set(Lang *L, Value form, Env **env) {
    if (seq_len(form) != 3)
        lang_error(L, "set! expects exactly 2 arguments, got %zu",
                   seq_len(form) - 1);
    Value name = seq_at(form, 1);
    if (name.tag != V_SYMBOL) lang_error(L, "set! target must be a symbol");
    Binding *b = env_find(L, *env, name.as.str.data);
    if (!b) lang_error(L, "set! of unbound symbol '%s'", name.as.str.data);
    if (b->is_macro) lang_error(L, "set! of macro '%s'", name.as.str.data);
    if (!b->mutable_)
        lang_error(L, "cannot set! immutable binding '%s'", name.as.str.data);
    Value val = lang_eval_form(L, seq_at(form, 2), env, false);
    b->value = val;
    return val;
}

static Value sf_fn(Lang *L, Value form, Env **env) {
    if (seq_len(form) < 2) lang_error(L, "fn expects a parameter list");
    return make_fn(L, form, 1, "fn", *env, false);
}

static Value sf_defn(Lang *L, Value form, Env **env, bool is_macro) {
    const char *what = is_macro ? "defmacro" : "defn";
    if (seq_len(form) < 3)
        lang_error(L, "%s expects a name and a parameter list", what);
    Value name = seq_at(form, 1);
    if (name.tag != V_SYMBOL) lang_error(L, "%s name must be a symbol", what);
    if (is_reserved(L, name))
        lang_error(L, "reserved symbol '%s' cannot be redefined",
                   name.as.str.data);
    Env *newenv = env_new(L, *env);
    Binding *b =
        env_define(L, newenv, name.as.str.data, v_nil(), false, is_macro);
    b->defined = false;
    Value fn = make_fn(L, form, 2, name.as.str.data, newenv, is_macro);
    b->value = fn;
    b->defined = true;
    if (is_global_frame(L, newenv->parent)) L->global = newenv;
    *env = newenv;
    return fn;
}

static Value sf_if(Lang *L, Value form, Env **env, bool tail) {
    size_t n = seq_len(form);
    if (n != 3 && n != 4)
        lang_error(L, "if expects 2 or 3 arguments, got %zu", n - 1);
    Value cond = lang_eval_form(L, seq_at(form, 1), env, false);
    if (truthy(cond)) return lang_eval_form(L, seq_at(form, 2), env, tail);
    if (n == 4) return lang_eval_form(L, seq_at(form, 3), env, tail);
    return v_nil();
}

static Value sf_do(Lang *L, Value form, Env **env, bool tail) {
    size_t n = seq_len(form);
    Value result = v_nil();
    for (size_t i = 1; i < n; i++)
        result = lang_eval_form(L, seq_at(form, i), env, tail && i + 1 == n);
    return result;
}

static Value sf_let(Lang *L, Value form, Env **env, bool tail) {
    size_t n = seq_len(form);
    if (n < 2) lang_error(L, "let expects a binding vector");
    Value bindings = seq_at(form, 1);
    if (!(bindings.tag == V_TUPLE && !bindings.as.seq->paren))
        lang_error(L, "let bindings must be a bracketed tuple");
    size_t bn = bindings.as.seq->len;
    if (bn % 2 != 0)
        lang_error(L, "let bindings must contain an even number of forms");

    Env *newenv = env_new(L, *env);
    for (size_t i = 0; i < bn; i += 2) {
        Value name = seq_at(bindings, i);
        if (name.tag != V_SYMBOL)
            lang_error(L, "let binding name must be a symbol");
        if (is_reserved(L, name))
            lang_error(L, "reserved symbol '%s' cannot be bound",
                       name.as.str.data);
        for (Binding *b = newenv->bindings; b; b = b->next)
            if (b->name == name.as.str.data)
                lang_error(L, "duplicate let binding '%s'", name.as.str.data);
        Value val = lang_eval_form(L, seq_at(bindings, i + 1), &newenv, false);
        env_define(L, newenv, name.as.str.data, val, false, false);
    }

    Value result = v_nil();
    for (size_t i = 2; i < n; i++)
        result = lang_eval_form(L, seq_at(form, i), &newenv, tail && i + 1 == n);
    return result;
}

static Value sf_and(Lang *L, Value form, Env **env, bool tail) {
    size_t n = seq_len(form);
    Value result = v_bool(true);
    for (size_t i = 1; i < n; i++) {
        result = lang_eval_form(L, seq_at(form, i), env, tail && i + 1 == n);
        if (!truthy(result)) return result;
    }
    return result;
}

static Value sf_or(Lang *L, Value form, Env **env, bool tail) {
    size_t n = seq_len(form);
    if (n == 1) return v_nil();
    Value result = v_nil();
    for (size_t i = 1; i < n; i++) {
        result = lang_eval_form(L, seq_at(form, i), env, tail && i + 1 == n);
        if (truthy(result)) return result;
    }
    return result;
}

static Value sf_recur(Lang *L, Value form, Env **env, bool tail) {
    FnObj *f = L->active_fn;
    if (!f) lang_error(L, "recur used outside of a fn");
    if (!tail) lang_error(L, "recur used in non-tail position");
    int argc = (int)seq_len(form) - 1;
    if (argc != f->nparams_total)
        lang_error(L, "recur expects %d arguments, got %d", f->nparams_total,
                   argc);
    Value *args = argc > 0 ? lang_alloc(L, (size_t)argc * sizeof(Value)) : NULL;
    for (int i = 0; i < argc; i++)
        args[i] = lang_eval_form(L, seq_at(form, (size_t)i + 1), env, false);
    L->recur_argc = argc;
    L->recur_argv = args;
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_RECUR;
    return v;
}

static Value expand_macro(Lang *L, Value macro, Value callform) {
    int argc = (int)seq_len(callform) - 1;
    Value *argv = argc > 0 ? lang_alloc(L, (size_t)argc * sizeof(Value)) : NULL;
    for (int i = 0; i < argc; i++) argv[i] = seq_at(callform, (size_t)i + 1);
    if (L->macro_depth >= MAX_MACRO_DEPTH)
        lang_error(L, "macro expansion depth limit (%d) exceeded",
                   MAX_MACRO_DEPTH);
    L->macro_depth++;
    Value out = lang_apply_macro(L, macro, argc, argv);
    L->macro_depth--;
    return out;
}

static Value sf_macroexpand(Lang *L, Value form, Env *env) {
    if (seq_len(form) != 2)
        lang_error(L, "macroexpand expects exactly one argument");
    Value target = seq_at(form, 1);
    if (target.tag == V_TUPLE && target.as.seq->paren && seq_len(target) > 0) {
        Value head = seq_at(target, 0);
        if (head.tag == V_SYMBOL) {
            Binding *b = env_find(L, env, head.as.str.data);
            if (b && b->is_macro) return expand_macro(L, b->value, target);
        }
    }
    return target;
}

/* ------------------------------------------------------------------ */
/* Evaluation                                                         */
/* ------------------------------------------------------------------ */

static Value eval_seq_literal(Lang *L, Value form, Env **env) {
    SeqObj *s = form.as.seq;
    Value *elems = s->len ? lang_alloc(L, s->len * sizeof(Value)) : NULL;
    for (size_t i = 0; i < s->len; i++)
        elems[i] = lang_eval_form(L, seq_at(form, i), env, false);
    return v_seq_from(L, elems, s->len, s->mutable_, s->paren);
}

static Value eval_map_literal(Lang *L, Value form, Env **env) {
    MapObj *m = form.as.map;
    Value out = v_map_empty(L, form.tag == V_TABLE);
    for (size_t i = 0; i < m->n; i++) {
        Value k = lang_eval_form(L, m->entries[i].key, env, false);
        Value v = lang_eval_form(L, m->entries[i].val, env, false);
        map_put(L, out, k, v);
    }
    return out;
}

static Value eval_paren(Lang *L, Value form, Env **env, bool tail) {
    size_t n = seq_len(form);
    if (n == 0) return form; /* () evaluates to itself ([KW-014]) */

    Value head = seq_at(form, 0);
    if (head.tag == V_SYMBOL) {
        const char *nm = head.as.str.data;
        if (nm == L->s_if.as.str.data) return sf_if(L, form, env, tail);
        if (nm == L->s_do.as.str.data) return sf_do(L, form, env, tail);
        if (nm == L->s_let.as.str.data) return sf_let(L, form, env, tail);
        if (nm == L->s_and.as.str.data) return sf_and(L, form, env, tail);
        if (nm == L->s_or.as.str.data) return sf_or(L, form, env, tail);
        if (nm == L->s_def.as.str.data) return sf_def(L, form, env, false);
        if (nm == L->s_var.as.str.data) return sf_def(L, form, env, true);
        if (nm == L->s_set.as.str.data) return sf_set(L, form, env);
        if (nm == L->s_fn.as.str.data) return sf_fn(L, form, env);
        if (nm == L->s_defn.as.str.data) return sf_defn(L, form, env, false);
        if (nm == L->s_defmacro.as.str.data)
            return sf_defn(L, form, env, true);
        if (nm == L->s_recur.as.str.data) return sf_recur(L, form, env, tail);
        if (nm == L->s_macroexpand.as.str.data)
            return sf_macroexpand(L, form, *env);
        if (nm == L->s_quote.as.str.data) {
            if (n != 2)
                lang_error(L, "quote expects exactly one argument, got %zu",
                           n - 1);
            return seq_at(form, 1);
        }
        if (nm == L->s_quasiquote.as.str.data) {
            if (n != 2)
                lang_error(L, "quasiquote expects exactly one argument, got %zu",
                           n - 1);
            return qq(L, seq_at(form, 1), env, 1);
        }
        if (nm == L->s_unquote.as.str.data)
            lang_error(L, "unquote used outside of quasiquote");

        Binding *b = env_find(L, *env, nm);
        if (b && b->is_macro) {
            Value expanded = expand_macro(L, b->value, form);
            return lang_eval_form(L, expanded, env, tail);
        }
    }

    Value fnv = lang_eval_form(L, head, env, false);
    int argc = (int)n - 1;
    Value *argv = argc > 0 ? lang_alloc(L, (size_t)argc * sizeof(Value)) : NULL;
    for (int i = 0; i < argc; i++)
        argv[i] = lang_eval_form(L, seq_at(form, (size_t)i + 1), env, false);
    return lang_apply(L, fnv, argc, argv);
}

Value lang_eval_form(Lang *L, Value form, Env **env, bool tail) {
    switch (form.tag) {
    case V_NIL:
    case V_BOOL:
    case V_INT:
    case V_FLOAT:
    case V_STRING:
    case V_KEYWORD:
    case V_FN:
        return form;
    case V_SYMBOL:
        return eval_symbol(L, form, *env);
    case V_TUPLE:
        if (form.as.seq->paren) return eval_paren(L, form, env, tail);
        return eval_seq_literal(L, form, env);
    case V_ARRAY:
        return eval_seq_literal(L, form, env);
    case V_STRUCT:
    case V_TABLE:
        return eval_map_literal(L, form, env);
    case V_RECUR:
        return form;
    }
    return form;
}

Value lang_eval_top(Lang *L, Value *forms, size_t n) {
    Env *env = L->global;
    Value result = v_nil();
    for (size_t i = 0; i < n; i++) {
        result = lang_eval_form(L, forms[i], &env, false);
        if (result.tag == V_RECUR)
            lang_error(L, "recur used outside of a fn");
    }
    L->global = env;
    return result;
}
