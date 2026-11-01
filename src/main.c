/*
 * main.c - command line driver: REPL and script runner.
 *
 * MIT License. See LICENSE.
 *
 * Usage:
 *   langrun                 read-eval-print loop
 *   langrun FILE            evaluate FILE
 *   langrun -p FILE         evaluate FILE, printing every top-level value
 *   langrun -e EXPR         evaluate EXPR and print its value
 *   langrun --version
 *
 * The language itself has no printing primitive: writing values out is the
 * host's job, exactly as §10's primitive set implies.
 */
#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_error(Lang *L) {
    fflush(stdout);
    fprintf(stderr, "error: %s\n", L->errmsg);
}

/* Read every form, converting reader diagnostics into printed errors. */
static bool protected_read(Lang *L, const char *src, size_t len,
                           const char *chunk, Value **forms, size_t *n,
                           bool *incomplete) {
    jmp_buf jb;
    jmp_buf *saved = L->err_jmp;
    if (setjmp(jb)) {
        L->err_jmp = saved;
        return false;
    }
    L->err_jmp = &jb;
    bool ok = lang_read_forms(L, src, len, chunk, forms, n, incomplete);
    L->err_jmp = saved;
    return ok;
}

/* Evaluate forms one by one, optionally printing each result. */
static bool protected_eval(Lang *L, Value *forms, size_t n, bool print_results) {
    jmp_buf jb;
    jmp_buf *saved_jmp = L->err_jmp;
    Env *saved_global = L->global;
    FnObj *saved_fn = L->active_fn;
    int saved_depth = L->call_depth;
    int saved_macro = L->macro_depth;

    if (setjmp(jb)) {
        L->err_jmp = saved_jmp;
        L->global = saved_global;
        L->active_fn = saved_fn;
        L->call_depth = saved_depth;
        L->macro_depth = saved_macro;
        return false;
    }
    L->err_jmp = &jb;

    Env *env = L->global;
    for (size_t i = 0; i < n; i++) {
        Value v = lang_eval_form(L, forms[i], &env, false);
        if (print_results) {
            char *repr = value_repr(L, v);
            printf("%s\n", repr);
            free(repr);
        }
    }
    L->global = env;
    L->err_jmp = saved_jmp;
    return true;
}

static int run_source(Lang *L, const char *src, const char *chunk,
                      bool print_results) {
    Value *forms = NULL;
    size_t n = 0;
    bool incomplete = false;
    if (!protected_read(L, src, strlen(src), chunk, &forms, &n, &incomplete)) {
        if (incomplete) {
            fflush(stdout);
            fprintf(stderr, "error: %s: unexpected end of input\n", chunk);
        } else {
            print_error(L);
        }
        return 1;
    }
    bool ok = protected_eval(L, forms, n, print_results);
    if (!ok) print_error(L);
    free(forms);
    return ok ? 0 : 1;
}

static char *read_stream(FILE *f, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                return NULL;
            }
            buf = p;
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) break;
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

static void repl(Lang *L) {
    bool tty = isatty(fileno(stdin));
    size_t cap = 0, len = 0;
    char *buf = NULL;

    if (tty) printf("core lang %s - Ctrl-D to exit\n", "0.8.0");

    for (;;) {
        if (tty) {
            fputs(len ? ".. " : "> ", stdout);
            fflush(stdout);
        }
        size_t linecap = 0;
        char *line = NULL;
        ssize_t got = getline(&line, &linecap, stdin);
        if (got < 0) {
            free(line);
            break;
        }
        while (len + (size_t)got + 1 > cap) {
            cap = cap ? cap * 2 : 256;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                free(line);
                fprintf(stderr, "error: out of memory\n");
                return;
            }
            buf = p;
        }
        memcpy(buf + len, line, (size_t)got);
        len += (size_t)got;
        buf[len] = '\0';
        free(line);

        Value *forms = NULL;
        size_t n = 0;
        bool incomplete = false;
        if (!protected_read(L, buf, len, "<repl>", &forms, &n, &incomplete)) {
            if (incomplete) continue; /* keep collecting lines */
            print_error(L);
            len = 0;
            continue;
        }
        len = 0;
        if (n > 0) {
            if (!protected_eval(L, forms, n, true)) print_error(L);
        }
        free(forms);
    }
    free(buf);
}

int main(int argc, char **argv) {
    bool print_results = false;
    const char *file = NULL;
    const char *expr = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--print") == 0) {
            print_results = true;
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            expr = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [-p] [FILE]\n"
                   "       %s -e EXPR\n"
                   "  -p, --print   print the value of every top-level form\n"
                   "  -e EXPR       evaluate EXPR and print its value\n",
                   argv[0], argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("langrun 0.8.0\n");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return 2;
        } else {
            file = argv[i];
        }
    }

    Lang *L = lang_new();
    if (!L) {
        fprintf(stderr, "error: cannot create interpreter\n");
        return 1;
    }

    int rc = 0;
    if (expr) {
        rc = run_source(L, expr, "<expr>", true);
    } else if (file) {
        size_t len = 0;
        FILE *f = fopen(file, "rb");
        if (!f) {
            fprintf(stderr, "error: cannot open file: %s\n", file);
            lang_free(L);
            return 1;
        }
        char *src = read_stream(f, &len);
        fclose(f);
        if (!src) {
            fprintf(stderr, "error: out of memory\n");
            lang_free(L);
            return 1;
        }
        (void)len;
        rc = run_source(L, src, file, print_results);
        free(src);
    } else {
        repl(L);
    }

    lang_free(L);
    return rc;
}
