#include "lisp.h"

#include <stdio.h>
#include <stdlib.h>

static int run(LispState *state, const char *source, int print_result) {
    LispValue result;
    if (lisp_eval(state, source, &result) != 0) {
        fprintf(stderr, "error: %s\n", lisp_last_error(state));
        return 1;
    }
    if (print_result) puts(lisp_value_to_string(state, result));
    return 0;
}

int main(int argc, char **argv) {
    LispState *state = lisp_state_new();
    int status;
    if (!state) {
        fputs("could not create interpreter\n", stderr);
        return 1;
    }
    if (argc > 1) {
        FILE *file = fopen(argv[1], "rb");
        long length;
        char *source;
        if (!file) {
            perror(argv[1]);
            lisp_state_free(state);
            return 1;
        }
        fseek(file, 0, SEEK_END);
        length = ftell(file);
        rewind(file);
        if (length < 0 || !(source = (char *)malloc((size_t)length + 1))) {
            fputs("could not read source file\n", stderr);
            fclose(file);
            lisp_state_free(state);
            return 1;
        }
        if (fread(source, 1, (size_t)length, file) != (size_t)length) {
            fputs("could not read source file\n", stderr);
            free(source);
            fclose(file);
            lisp_state_free(state);
            return 1;
        }
        source[length] = '\0';
        fclose(file);
        status = run(state, source, 0);
        free(source);
    } else {
        char line[4096];
        status = 0;
        while (fputs("lisp> ", stdout), fflush(stdout), fgets(line, sizeof(line), stdin)) {
            if (run(state, line, 1) != 0) status = 1;
        }
    }
    lisp_state_free(state);
    return status;
}
