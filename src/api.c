/*
 * api.c - interpreter lifecycle and the small embedding API.
 *
 * MIT License. See LICENSE.
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *dup_cstr(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static Value intern_cached(Lang *L, const char *name) {
    return v_symbol(L, name, strlen(name));
}

Lang *lang_new(void) {
    Lang *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->intern_cap = 256;
    L->intern = calloc(L->intern_cap, sizeof(InternEntry *));
    if (!L->intern) {
        free(L);
        return NULL;
    }
    L->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)time(NULL);

    /* Reserved symbols ([LEX-005], §9). */
    L->s_def = intern_cached(L, "def");
    L->s_var = intern_cached(L, "var");
    L->s_set = intern_cached(L, "set!");
    L->s_fn = intern_cached(L, "fn");
    L->s_defn = intern_cached(L, "defn");
    L->s_defmacro = intern_cached(L, "defmacro");
    L->s_macroexpand = intern_cached(L, "macroexpand");
    L->s_if = intern_cached(L, "if");
    L->s_do = intern_cached(L, "do");
    L->s_let = intern_cached(L, "let");
    L->s_recur = intern_cached(L, "recur");
    L->s_quote = intern_cached(L, "quote");
    L->s_quasiquote = intern_cached(L, "quasiquote");
    L->s_unquote = intern_cached(L, "unquote");
    L->s_and = intern_cached(L, "and");
    L->s_or = intern_cached(L, "or");
    L->s_amp = intern_cached(L, "&");
    L->s_ampopt = intern_cached(L, "&opt");

    /* Type keywords ([C-039]). */
    L->k_nil = v_keyword(L, "nil", 3);
    L->k_boolean = v_keyword(L, "boolean", 7);
    L->k_number = v_keyword(L, "number", 6);
    L->k_string = v_keyword(L, "string", 6);
    L->k_symbol = v_keyword(L, "symbol", 6);
    L->k_keyword = v_keyword(L, "keyword", 7);
    L->k_tuple = v_keyword(L, "tuple", 5);
    L->k_array = v_keyword(L, "array", 5);
    L->k_struct = v_keyword(L, "struct", 6);
    L->k_table = v_keyword(L, "table", 5);
    L->k_function = v_keyword(L, "function", 8);

    L->core = env_new(L, NULL);
    L->global = L->core;
    prims_install(L, L->core);
    return L;
}

void lang_free(Lang *L) {
    if (!L) return;
    free(L->intern);
    lang_free_arena(L);
    free(L);
}

int lang_eval_string(Lang *L, const char *source, const char *chunkname,
                     char **result, char **error) {
    if (result) *result = NULL;
    if (error) *error = NULL;

    Value *forms = NULL;
    jmp_buf jb;
    jmp_buf *saved_jmp = L->err_jmp;
    Env *saved_global = L->global;
    FnObj *saved_fn = L->active_fn;
    int saved_depth = L->call_depth;
    int saved_macro = L->macro_depth;

    if (setjmp(jb)) {
        free(forms);
        L->err_jmp = saved_jmp;
        L->global = saved_global;
        L->active_fn = saved_fn;
        L->call_depth = saved_depth;
        L->macro_depth = saved_macro;
        if (error) *error = dup_cstr(L->errmsg);
        return 1;
    }
    L->err_jmp = &jb;

    size_t n = 0;
    bool incomplete = false;
    lang_read_forms(L, source, strlen(source), chunkname, &forms, &n,
                    &incomplete);
    if (incomplete)
        lang_error(L, "%s: unexpected end of input",
                   chunkname ? chunkname : "<input>");

    Value last = lang_eval_top(L, forms, n);
    char *repr = value_repr(L, last);
    free(forms);

    L->err_jmp = saved_jmp;
    if (result) *result = repr;
    else free(repr);
    return 0;
}

int lang_eval_file(Lang *L, const char *path, char **result, char **error) {
    if (result) *result = NULL;
    if (error) *error = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error) {
            const char *prefix = "cannot open file: ";
            size_t n = strlen(prefix) + strlen(path) + 1;
            char *msg = malloc(n);
            if (msg) snprintf(msg, n, "%s%s", prefix, path);
            *error = msg;
        }
        return 1;
    }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        if (error) *error = dup_cstr("out of memory");
        return 1;
    }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                fclose(f);
                if (error) *error = dup_cstr("out of memory");
                return 1;
            }
            buf = p;
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) break;
    }
    int read_err = ferror(f);
    fclose(f);
    if (read_err) {
        free(buf);
        if (error) *error = dup_cstr("error while reading file");
        return 1;
    }
    buf[len] = '\0';
    int rc = lang_eval_string(L, buf, path, result, error);
    free(buf);
    return rc;
}
