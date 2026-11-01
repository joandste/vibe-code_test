/*
 * embed.c - minimal example of embedding the interpreter.
 *
 * MIT License. See LICENSE.
 *
 * Build with `make example`, then run ./embed "..." to evaluate an expression.
 */
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    Lang *L = lang_new();
    if (!L) {
        fprintf(stderr, "cannot create interpreter\n");
        return 1;
    }

    char *result = NULL;
    char *error = NULL;
    const char *source = argc > 1 ? argv[1] : "(map inc [1 2 3])";

    if (lang_eval_string(L, source, "<embed>", &result, &error) != 0) {
        fprintf(stderr, "error: %s\n", error);
        free(error);
        lang_free(L);
        return 1;
    }

    printf("%s\n", result);
    free(result);
    lang_free(L);
    return 0;
}
