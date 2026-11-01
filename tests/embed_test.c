/*
 * embed_test.c - checks the public embedding API (include/lang.h).
 *
 * MIT License. See LICENSE.
 */
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    char *result = NULL;
    char *error = NULL;

    Lang *L = lang_new();
    check(L != NULL, "lang_new returns a handle");

    check(lang_eval_string(L, "(+ 1 2)", "test", &result, &error) == 0,
          "evaluating (+ 1 2) succeeds");
    check(result != NULL && strcmp(result, "3") == 0, "result prints as 3");
    check(error == NULL, "no error on success");
    free(result);
    result = NULL;

    check(lang_eval_string(L, "(def a @[1 2]) (push! a 3) a", "test", &result,
                           &error) == 0,
          "state persists across evaluations");
    check(result != NULL && strcmp(result, "@[1 2 3]") == 0,
          "mutation is visible to the host");
    free(result);
    result = NULL;

    check(lang_eval_string(L, "(first [])", "test", &result, &error) == 1,
          "runtime errors are reported");
    check(result == NULL, "no result on failure");
    check(error != NULL && strstr(error, "empty") != NULL,
          "error message describes the failure");
    free(error);
    error = NULL;

    check(lang_eval_string(L, "(+ 1", "test", &result, &error) == 1,
          "incomplete input is an error");
    check(error != NULL && strstr(error, "unexpected end of input") != NULL,
          "incomplete input is diagnosed");
    free(error);
    error = NULL;

    check(lang_eval_file(L, "tests/does-not-exist.lang", &result, &error) == 1,
          "missing files are reported");
    free(error);
    error = NULL;

    lang_free(L);

    if (failures) {
        printf("%d embedding checks failed\n", failures);
        return 1;
    }
    printf("ok   embed\n");
    return 0;
}
