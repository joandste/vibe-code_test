#include "lisp.h"

#include <stdio.h>
#include <string.h>

static LispValue host_increment(LispState *state, int argc,
                                const LispValue *argv, void *userdata) {
    (void)userdata;
    if (argc != 1 || lisp_type(&argv[0]) != LISP_NUMBER) {
        lisp_raise(state, "host_increment expects one number");
        return lisp_nil();
    }
    return lisp_number(lisp_as_number(&argv[0]) + 1);
}

static int check_number(LispState *state, const char *source, double expected) {
    LispValue result;
    if (lisp_eval(state, source, &result) != 0) {
        fprintf(stderr, "%s: %s\n", source, lisp_last_error(state));
        return 1;
    }
    if (lisp_type(&result) != LISP_NUMBER || lisp_as_number(&result) != expected) {
        fprintf(stderr, "%s: expected %g, got %s\n", source, expected,
                lisp_value_to_string(state, result));
        return 1;
    }
    return 0;
}

static int check_string(LispState *state, const char *source, const char *expected) {
    LispValue result;
    size_t length;
    const char *text;
    if (lisp_eval(state, source, &result) != 0) {
        fprintf(stderr, "%s: %s\n", source, lisp_last_error(state));
        return 1;
    }
    text = lisp_as_string(&result, &length);
    if (!text || strlen(expected) != length || memcmp(text, expected, length) != 0) {
        fprintf(stderr, "%s: expected string %s, got %s\n", source, expected,
                lisp_value_to_string(state, result));
        return 1;
    }
    return 0;
}

static int check_repr(LispState *state, const char *source, const char *expected) {
    LispValue result;
    const char *text;
    if (lisp_eval(state, source, &result) != 0) {
        fprintf(stderr, "%s: %s\n", source, lisp_last_error(state));
        return 1;
    }
    text = lisp_value_to_string(state, result);
    if (!text || strcmp(text, expected) != 0) {
        fprintf(stderr, "%s: expected %s, got %s\n", source, expected,
                text ? text : "<null>");
        return 1;
    }
    return 0;
}

int main(void) {
    LispState *state = lisp_state_new();
    LispValue result;
    int failures = 0;
    if (!state) return 1;
    lisp_define_native(state, "host-increment", host_increment, NULL);
    failures += check_number(state, "(+ 1 2 3)", 6);
    failures += check_number(state, "(- 10 2 3)", 5);
    failures += check_number(state, "(/ 8 2 2)", 2);
    failures += check_number(state, "(/ 0 2)", 0);
    failures += check_number(state, "(host-increment 40)", 41);
    failures += check_number(state, "((fn [x] (* x x)) 7)", 49);
    failures += check_number(state, "(let [x 6 f (fn [y] (+ x y))] (f 4))", 10);
    failures += check_number(state, "(get @[1 (+ 1 1) 3] 1)", 2);
    failures += check_number(state, "(get [1 (+ 1 1) 3] 1)", 2);
    failures += check_repr(state, "'(1 2)", "[1 2]");
    failures += check_repr(state, "[1 2]", "[1 2]");
    failures += check_repr(state, "@[1 2]", "@[1 2]");
    failures += check_repr(state, "'(foo (+ 1 2))", "[foo [+ 1 2]]");
    failures += check_number(state,
        "(do (def fib (fn [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))) (fib 8))",
        21);
    failures += check_number(state, "(do (def x 10) (let [x 3 y 4] (+ x y)))", 7);
    failures += check_string(state, "(get {:name \"Ada\"} :name)", "Ada");
    failures += check_number(state, "(do (def xs @[1 2 3]) (put xs 1 9) (get xs 1))", 9);
    failures += check_number(state, "(get (tuple 4 5 6) 1)", 5);
    failures += check_number(state, "(do (def n 0) (while (< n 5) (set n (+ n 1))) n)", 5);
    {
        LispValue array = lisp_array(state);
        LispValue table = lisp_table(state);
        LispValue key = lisp_keyword(state, "answer");
        LispValue stored;
        lisp_array_push(state, array, lisp_number(42));
        lisp_table_put(state, table, key, lisp_array_get(array, 0));
        stored = lisp_table_get(table, key);
        if (lisp_array_length(array) != 1 ||
            lisp_type(&stored) != LISP_NUMBER || lisp_as_number(&stored) != 42) {
            fputs("host collection API failed\n", stderr);
            failures++;
        }
        lisp_define(state, "host-data", table);
        failures += check_number(state, "(get host-data :answer)", 42);
    }
    if (lisp_eval(state, "(/ 1 0)", &result) == 0 ||
        strstr(lisp_last_error(state), "division by zero") == NULL) {
        fputs("expected division-by-zero error\n", stderr);
        failures++;
    }
    if (lisp_eval(state, "(put [1 2] 0 9)", &result) == 0 ||
        strstr(lisp_last_error(state), "immutable tuple") == NULL) {
        fputs("expected immutable tuple error\n", stderr);
        failures++;
    }
    if (lisp_eval(state, "'(1 2)", &result) != 0 ||
        lisp_type(&result) != LISP_TUPLE) {
        fputs("quoted call form should be a tuple\n", stderr);
        failures++;
    }
    lisp_state_free(state);
    if (failures) return 1;
    puts("all tests passed");
    return 0;
}
