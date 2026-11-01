/*
 * lang.h - public embedding API for the Core language reference implementation.
 *
 * MIT License. See LICENSE.
 *
 * The API is intentionally tiny: create an interpreter, evaluate source,
 * inspect the printed result or the error message, destroy the interpreter.
 *
 * Memory: all values produced by the interpreter live in an arena owned by the
 * Lang handle and are released by lang_free(). The strings returned through
 * `result`/`error` are heap-allocated with malloc() and must be released with
 * free() by the caller.
 */
#ifndef LANG_H
#define LANG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Lang Lang;

/* Create a new interpreter with the core environment installed. */
Lang *lang_new(void);

/* Destroy an interpreter and every value it allocated. */
void lang_free(Lang *L);

/*
 * Evaluate `source` as one or more top-level forms.
 *
 * `chunkname` is only used in diagnostics and may be NULL.
 *
 * Returns 0 on success. On success *result receives the printed representation
 * of the value of the last form (malloc'd, never NULL on success) and *error is
 * set to NULL. On failure the function returns 1, *result is set to NULL and
 * *error receives the diagnostic message (malloc'd). Either out-parameter may
 * be NULL if the caller is not interested in it.
 */
int lang_eval_string(Lang *L, const char *source, const char *chunkname,
                     char **result, char **error);

/* Same as lang_eval_string, reading the file at `path`. */
int lang_eval_file(Lang *L, const char *path, char **result, char **error);

#ifdef __cplusplus
}
#endif

#endif /* LANG_H */
