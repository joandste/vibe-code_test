#ifndef EMBER_LISP_H
#define EMBER_LISP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LispState LispState;
typedef struct LispValue LispValue;

typedef enum {
    LISP_NIL,
    LISP_BOOL,
    LISP_NUMBER,
    LISP_STRING,
    LISP_KEYWORD,
    LISP_SYMBOL,
    LISP_CALL,
    LISP_TUPLE,
    LISP_ARRAY,
    LISP_TABLE,
    LISP_FUNCTION,
    LISP_NATIVE
} LispType;

struct LispValue {
    LispType type;
    union {
        int boolean;
        double number;
        struct {
            const char *data;
            size_t length;
        } string;
        void *object;
    } as;
};

typedef LispValue (*LispNativeFn)(LispState *state, int argc,
                                  const LispValue *argv, void *userdata);

LispState *lisp_state_new(void);
void lisp_state_free(LispState *state);

/* Returns 0 on success and -1 on parse or runtime error. */
int lisp_eval(LispState *state, const char *source, LispValue *result);
const char *lisp_last_error(const LispState *state);

/* Native functions and values remain owned by state until it is freed. */
void lisp_define_native(LispState *state, const char *name,
                        LispNativeFn function, void *userdata);
void lisp_define(LispState *state, const char *name, LispValue value);

LispValue lisp_nil(void);
LispValue lisp_bool(int value);
LispValue lisp_number(double value);
LispValue lisp_string(LispState *state, const char *value);
LispValue lisp_string_n(LispState *state, const char *value, size_t length);
LispValue lisp_keyword(LispState *state, const char *value);
LispValue lisp_array(LispState *state);
int lisp_array_push(LispState *state, LispValue array, LispValue value);
size_t lisp_array_length(LispValue array);
LispValue lisp_array_get(LispValue array, size_t index);
LispValue lisp_table(LispState *state);
int lisp_table_put(LispState *state, LispValue table, LispValue key,
                   LispValue value);
LispValue lisp_table_get(LispValue table, LispValue key);

LispType lisp_type(const LispValue *value);
double lisp_as_number(const LispValue *value);
int lisp_as_bool(const LispValue *value);
const char *lisp_as_string(const LispValue *value, size_t *length);
int lisp_raise(LispState *state, const char *message);

/* Returns a state-owned, human-readable representation of value. */
const char *lisp_value_to_string(LispState *state, LispValue value);

#ifdef __cplusplus
}
#endif

#endif
