#include "lisp.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Sequence Sequence;
typedef struct Table Table;
typedef struct Binding Binding;
typedef struct Environment Environment;
typedef struct Closure Closure;
typedef struct Native Native;

struct Sequence {
    LispValue *items;
    size_t count;
    size_t capacity;
};

typedef struct {
    LispValue key;
    LispValue value;
} Entry;

struct Table {
    Entry *entries;
    size_t count;
    size_t capacity;
};

struct Binding {
    const char *name;
    LispValue value;
};

struct Environment {
    size_t parent;
    Binding *bindings;
    size_t binding_count;
    size_t binding_capacity;
};

struct Closure {
    Sequence *parameters;
    Sequence *body;
    size_t environment;
};

struct Native {
    LispNativeFn function;
    void *userdata;
};

struct LispState {
    void **allocations;
    size_t allocation_count;
    size_t allocation_capacity;
    Environment *environments;
    size_t environment_count;
    size_t environment_capacity;
    size_t global;
    char error[512];
    int failed;
    int eval_depth;
    int steps;
};

typedef struct {
    LispValue *items;
    size_t length;
} ValueView;

typedef struct {
    Binding *items;
    size_t length;
} BindingView;

typedef struct {
    Entry *items;
    size_t length;
} EntryView;

typedef struct {
    LispState *state;
    const char *source;
    size_t length;
    size_t offset;
    int line;
    int column;
} Parser;

static LispValue eval_value(LispState *, LispValue, size_t);
static LispValue quote_value(LispState *, LispValue, int);
static LispValue make_symbol(LispState *, const char *, size_t);
static LispValue make_keyword(LispState *, const char *, size_t);
static LispValue make_sequence(LispState *, LispType);
static int value_equal(LispValue, LispValue);
static size_t table_find_index(EntryView, LispValue, size_t);

static int grow_array(LispState *state, void **items, size_t *capacity,
                      size_t needed, size_t element_size) {
    size_t next = *capacity ? *capacity : 8;
    void *grown;
    while (next < needed) {
        if (next > (size_t)-1 / 2) {
            lisp_raise(state, "array capacity overflow");
            return -1;
        }
        next *= 2;
    }
    if (next > (size_t)-1 / element_size) {
        lisp_raise(state, "array size overflow");
        return -1;
    }
    grown = realloc(*items, next * element_size);
    if (!grown) {
        lisp_raise(state, "out of memory");
        return -1;
    }
    *items = grown;
    *capacity = next;
    return 0;
}

static size_t table_find_index(EntryView entries, LispValue key, size_t index) {
    if (!entries.length) return (size_t)-1;
    if (value_equal(entries.items[0].key, key)) return index;
    entries.items++;
    entries.length--;
    return table_find_index(entries, key, index + 1);
}

static void *state_alloc(LispState *state, size_t size) {
    void *memory = calloc(1, size ? size : 1);
    if (!memory) {
        if (state) lisp_raise(state, "out of memory");
        return NULL;
    }
    if (state) {
        if (state->allocation_count == state->allocation_capacity &&
            grow_array(state, (void **)&state->allocations,
                       &state->allocation_capacity, state->allocation_count + 1,
                       sizeof(*state->allocations)) != 0) {
            free(memory);
            return NULL;
        }
        state->allocations[state->allocation_count++] = memory;
    }
    return memory;
}

static char *state_strndup(LispState *state, const char *text, size_t length) {
    char *copy = (char *)state_alloc(state, length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static int failf(LispState *state, const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return lisp_raise(state, buffer);
}

int lisp_raise(LispState *state, const char *message) {
    if (!state) return -1;
    snprintf(state->error, sizeof(state->error), "%s", message ? message : "error");
    state->failed = 1;
    return -1;
}

LispValue lisp_nil(void) {
    LispValue value;
    memset(&value, 0, sizeof(value));
    value.type = LISP_NIL;
    return value;
}

LispValue lisp_bool(int boolean) {
    LispValue value = lisp_nil();
    value.type = LISP_BOOL;
    value.as.boolean = boolean != 0;
    return value;
}

LispValue lisp_number(double number) {
    LispValue value = lisp_nil();
    value.type = LISP_NUMBER;
    value.as.number = number;
    return value;
}

LispValue lisp_string_n(LispState *state, const char *text, size_t length) {
    LispValue value = lisp_nil();
    value.type = LISP_STRING;
    value.as.string.data = state_strndup(state, text, length);
    value.as.string.length = length;
    return value;
}

LispValue lisp_string(LispState *state, const char *text) {
    return lisp_string_n(state, text ? text : "", text ? strlen(text) : 0);
}

LispType lisp_type(const LispValue *value) {
    return value ? value->type : LISP_NIL;
}

double lisp_as_number(const LispValue *value) {
    return value && value->type == LISP_NUMBER ? value->as.number : 0.0;
}

int lisp_as_bool(const LispValue *value) {
    return value && value->type == LISP_BOOL && value->as.boolean;
}

const char *lisp_as_string(const LispValue *value, size_t *length) {
    if (!value || (value->type != LISP_STRING && value->type != LISP_KEYWORD &&
                   value->type != LISP_SYMBOL)) {
        if (length) *length = 0;
        return NULL;
    }
    if (length) *length = value->as.string.length;
    return value->as.string.data;
}

static LispValue make_symbol(LispState *state, const char *text, size_t length) {
    LispValue value = lisp_nil();
    value.type = LISP_SYMBOL;
    value.as.string.data = state_strndup(state, text, length);
    value.as.string.length = length;
    return value;
}

static LispValue make_keyword(LispState *state, const char *text, size_t length) {
    LispValue value = lisp_nil();
    value.type = LISP_KEYWORD;
    value.as.string.data = state_strndup(state, text, length);
    value.as.string.length = length;
    return value;
}

LispValue lisp_keyword(LispState *state, const char *text) {
    if (!text) return lisp_nil();
    return make_keyword(state, text, strlen(text));
}

static LispValue make_sequence(LispState *state, LispType type) {
    LispValue value = lisp_nil();
    Sequence *sequence = (Sequence *)state_alloc(state, sizeof(*sequence));
    value.type = type;
    value.as.object = sequence;
    return value;
}

static Sequence *sequence_of(LispValue value) {
    return (Sequence *)value.as.object;
}

static int sequence_push(LispState *state, Sequence *sequence, LispValue value) {
    if (sequence->count == sequence->capacity) {
        size_t capacity = sequence->capacity ? sequence->capacity * 2 : 8;
        LispValue *items = (LispValue *)state_alloc(state, capacity * sizeof(*items));
        if (!items) return -1;
        if (sequence->items)
            memcpy(items, sequence->items, sequence->count * sizeof(*items));
        sequence->items = items;
        sequence->capacity = capacity;
    }
    sequence->items[sequence->count++] = value;
    return 0;
}

LispValue lisp_array(LispState *state) {
    return state ? make_sequence(state, LISP_ARRAY) : lisp_nil();
}

int lisp_array_push(LispState *state, LispValue array, LispValue value) {
    if (array.type != LISP_ARRAY)
        return lisp_raise(state, "lisp_array_push expects an array");
    return sequence_push(state, sequence_of(array), value);
}

size_t lisp_array_length(LispValue array) {
    return array.type == LISP_ARRAY ? sequence_of(array)->count : 0;
}

LispValue lisp_array_get(LispValue array, size_t index) {
    Sequence *sequence;
    if (array.type != LISP_ARRAY) return lisp_nil();
    sequence = sequence_of(array);
    return index < sequence->count ? sequence->items[index] : lisp_nil();
}

LispValue lisp_table(LispState *state) {
    LispValue value = lisp_nil();
    if (!state) return value;
    value.type = LISP_TABLE;
    value.as.object = state_alloc(state, sizeof(Table));
    return value;
}

int lisp_table_put(LispState *state, LispValue value, LispValue key,
                   LispValue item) {
    Table *table;
    size_t i;
    if (value.type != LISP_TABLE)
        return lisp_raise(state, "lisp_table_put expects a table");
    table = (Table *)value.as.object;
    i = table_find_index((EntryView){table->entries, table->count}, key, 0);
    if (i == (size_t)-1) {
        i = table->count;
        if (table->count == table->capacity) {
            size_t capacity = table->capacity ? table->capacity * 2 : 4;
            Entry *entries = (Entry *)state_alloc(state, capacity * sizeof(*entries));
            if (!entries) return -1;
            if (table->entries)
                memcpy(entries, table->entries, table->count * sizeof(*entries));
            table->entries = entries;
            table->capacity = capacity;
        }
        table->count++;
    }
    table->entries[i].key = key;
    table->entries[i].value = item;
    return 0;
}

LispValue lisp_table_get(LispValue value, LispValue key) {
    Table *table;
    size_t i;
    if (value.type != LISP_TABLE) return lisp_nil();
    table = (Table *)value.as.object;
    i = table_find_index((EntryView){table->entries, table->count}, key, 0);
    return i == (size_t)-1 ? lisp_nil() : table->entries[i].value;
}

static size_t environment_new(LispState *state, size_t parent) {
    size_t index = state->environment_count;
    Environment empty;
    if (index == state->environment_capacity &&
        grow_array(state, (void **)&state->environments,
                   &state->environment_capacity, index + 1,
                   sizeof(*state->environments)) != 0)
        return (size_t)-1;
    memset(&empty, 0, sizeof(empty));
    empty.parent = parent;
    state->environments[state->environment_count++] = empty;
    return index;
}

static Binding *binding_find_view(BindingView view, const char *name,
                                  size_t length) {
    if (!view.length) return NULL;
    if (strlen(view.items[0].name) == length &&
        memcmp(view.items[0].name, name, length) == 0)
        return &view.items[0];
    view.items++;
    view.length--;
    return binding_find_view(view, name, length);
}

static Binding *environment_find(LispState *state, size_t environment,
                                 const char *name, size_t length) {
    Environment *scope;
    Binding *binding;
    if (environment == (size_t)-1 || environment >= state->environment_count)
        return NULL;
    scope = &state->environments[environment];
    binding = binding_find_view((BindingView){scope->bindings,
                                               scope->binding_count},
                                name, length);
    if (binding || scope->parent == (size_t)-1) return binding;
    return environment_find(state, scope->parent, name, length);
}

static int environment_set(LispState *state, size_t environment,
                           const char *name, size_t length, LispValue value,
                           int update_only) {
    Binding *binding = update_only ?
        environment_find(state, environment, name, length) : NULL;
    if (!binding) {
        if (update_only) return failf(state, "cannot set unbound symbol '%.*s'",
                                      (int)length, name);
        {
            Environment *scope;
            const char *copy = state_strndup(state, name, length);
            if (!copy) return -1;
            scope = &state->environments[environment];
            if (scope->binding_count == scope->binding_capacity) {
                if (grow_array(state, (void **)&scope->bindings,
                               &scope->binding_capacity,
                               scope->binding_count + 1,
                               sizeof(*scope->bindings)) != 0)
                    return -1;
            }
            binding = &scope->bindings[scope->binding_count++];
            binding->name = copy;
        }
    }
    binding->value = value;
    return 0;
}

static int truthy(LispValue value) {
    return value.type != LISP_NIL && !(value.type == LISP_BOOL && !value.as.boolean);
}

static int value_equal(LispValue a, LispValue b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case LISP_NIL: return 1;
        case LISP_BOOL: return a.as.boolean == b.as.boolean;
        case LISP_NUMBER: return a.as.number == b.as.number;
        case LISP_STRING:
        case LISP_KEYWORD:
        case LISP_SYMBOL:
            return a.as.string.length == b.as.string.length &&
                   memcmp(a.as.string.data, b.as.string.data,
                          a.as.string.length) == 0;
        default: return a.as.object == b.as.object;
    }
}

static int parser_error(Parser *parser, const char *message) {
    return failf(parser->state, "%s at %d:%d", message, parser->line,
                 parser->column);
}

static char parser_peek(Parser *parser) {
    return parser->offset < parser->length ? parser->source[parser->offset] : '\0';
}

static char parser_advance(Parser *parser) {
    char ch = parser_peek(parser);
    if (!ch) return ch;
    parser->offset++;
    if (ch == '\n') {
        parser->line++;
        parser->column = 1;
    } else {
        parser->column++;
    }
    return ch;
}

static void parser_skip_space(Parser *parser) {
    for (;;) {
        char ch = parser_peek(parser);
        if (isspace((unsigned char)ch)) {
            parser_advance(parser);
        } else if (ch == '#' || ch == ';') {
            while (parser_peek(parser) && parser_peek(parser) != '\n')
                parser_advance(parser);
        } else {
            return;
        }
    }
}

static int is_delimiter(char ch) {
    return !ch || isspace((unsigned char)ch) || ch == '(' || ch == ')' ||
           ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '"' ||
           ch == ';' || ch == '#';
}

static int parse_form(Parser *parser, LispValue *out);

static int parse_sequence(Parser *parser, LispType type, char closing,
                          LispValue *out) {
    LispValue value = make_sequence(parser->state, type);
    Sequence *sequence = sequence_of(value);
    while (!parser->state->failed) {
        LispValue item;
        parser_skip_space(parser);
        if (parser_peek(parser) == closing) {
            parser_advance(parser);
            *out = value;
            return 0;
        }
        if (!parser_peek(parser))
            return parser_error(parser, "unterminated collection");
        if (parse_form(parser, &item) != 0) return -1;
        if (sequence_push(parser->state, sequence, item) != 0) return -1;
    }
    return -1;
}

static int parse_string(Parser *parser, LispValue *out) {
    char *buffer;
    size_t count = 0, capacity = 32;
    if (parser_advance(parser) != '"') return -1;
    buffer = (char *)malloc(capacity);
    if (!buffer) return lisp_raise(parser->state, "out of memory");
    while (parser_peek(parser) && parser_peek(parser) != '"') {
        char ch = parser_advance(parser);
        if (ch == '\\') {
            ch = parser_advance(parser);
            if (!ch) break;
            switch (ch) {
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case '\\': case '"': break;
                default:
                    free(buffer);
                    return parser_error(parser, "invalid string escape");
            }
        }
        if (count + 1 >= capacity) {
            char *grown;
            capacity *= 2;
            grown = (char *)realloc(buffer, capacity);
            if (!grown) {
                free(buffer);
                return lisp_raise(parser->state, "out of memory");
            }
            buffer = grown;
        }
        buffer[count++] = ch;
    }
    if (parser_peek(parser) != '"') {
        free(buffer);
        return parser_error(parser, "unterminated string");
    }
    parser_advance(parser);
    *out = lisp_string_n(parser->state, buffer, count);
    free(buffer);
    return parser->state->failed ? -1 : 0;
}

static int parse_atom(Parser *parser, LispValue *out) {
    size_t start = parser->offset, length;
    char *end;
    double number;
    while (!is_delimiter(parser_peek(parser))) parser_advance(parser);
    length = parser->offset - start;
    if (!length) return parser_error(parser, "unexpected character");
    if (length == 3 && memcmp(parser->source + start, "nil", 3) == 0) {
        *out = lisp_nil();
    } else if ((length == 4 && memcmp(parser->source + start, "true", 4) == 0) ||
               (length == 5 && memcmp(parser->source + start, "false", 5) == 0)) {
        *out = lisp_bool(parser->source[start] == 't');
    } else if (parser->source[start] == ':') {
        if (length == 1) return parser_error(parser, "empty keyword");
        *out = make_keyword(parser->state, parser->source + start + 1, length - 1);
    } else {
        char *token = state_strndup(parser->state, parser->source + start, length);
        if (!token) return -1;
        number = strtod(token, &end);
        if (end != token && *end == '\0') {
            *out = lisp_number(number);
        } else {
            *out = make_symbol(parser->state, token, length);
        }
    }
    return parser->state->failed ? -1 : 0;
}

static int parse_form(Parser *parser, LispValue *out) {
    char ch;
    parser_skip_space(parser);
    ch = parser_peek(parser);
    if (!ch) return parser_error(parser, "expected form");
    if (ch == '(') {
        parser_advance(parser);
        return parse_sequence(parser, LISP_CALL, ')', out);
    }
    if (ch == '[') {
        parser_advance(parser);
        return parse_sequence(parser, LISP_TUPLE, ']', out);
    }
    if (ch == '{') {
        LispValue value = lisp_nil();
        Table *table;
        parser_advance(parser);
        value.type = LISP_TABLE;
        table = (Table *)state_alloc(parser->state, sizeof(*table));
        value.as.object = table;
        for (;;) {
            LispValue key, item;
            parser_skip_space(parser);
            if (parser_peek(parser) == '}') {
                parser_advance(parser);
                *out = value;
                return 0;
            }
            if (!parser_peek(parser)) return parser_error(parser, "unterminated table");
            if (parse_form(parser, &key) != 0) return -1;
            parser_skip_space(parser);
            if (parser_peek(parser) == '}')
                return parser_error(parser, "table requires key/value pairs");
            if (parse_form(parser, &item) != 0) return -1;
            /* Table literals retain their forms until evaluation. */
            {
                Sequence *forms = (Sequence *)state_alloc(parser->state, sizeof(*forms));
                forms->items = (LispValue *)state_alloc(parser->state, 2 * sizeof(LispValue));
                forms->items[0] = key;
                forms->items[1] = item;
                forms->count = forms->capacity = 2;
                /* Store pairs as a sequence of two-item sequences. */
                {
                    LispValue pair = lisp_nil();
                    pair.type = LISP_TUPLE;
                    pair.as.object = forms;
                    if (table->count == table->capacity) {
                        size_t cap = table->capacity ? table->capacity * 2 : 4;
                        Entry *entries = (Entry *)state_alloc(parser->state, cap * sizeof(*entries));
                        if (table->entries)
                            memcpy(entries, table->entries, table->count * sizeof(*entries));
                        table->entries = entries;
                        table->capacity = cap;
                    }
                    table->entries[table->count].key = pair;
                    table->entries[table->count].value = lisp_nil();
                    table->count++;
                }
            }
        }
    }
    if (ch == '@') {
        parser_advance(parser);
        ch = parser_peek(parser);
        if (ch == '[') {
            parser_advance(parser);
            return parse_sequence(parser, LISP_ARRAY, ']', out);
        }
        if (ch == '{') {
            LispValue inner;
            Table *table;
            parser_advance(parser);
            *out = lisp_nil();
            out->type = LISP_TABLE;
            table = (Table *)state_alloc(parser->state, sizeof(*table));
            out->as.object = table;
            for (;;) {
                LispValue key, item;
                parser_skip_space(parser);
                if (parser_peek(parser) == '}') {
                    parser_advance(parser);
                    return 0;
                }
                if (!parser_peek(parser)) return parser_error(parser, "unterminated table");
                if (parse_form(parser, &key) != 0) return -1;
                parser_skip_space(parser);
                if (parser_peek(parser) == '}')
                    return parser_error(parser, "table requires key/value pairs");
                if (parse_form(parser, &item) != 0) return -1;
                if (table->count == table->capacity) {
                    size_t cap = table->capacity ? table->capacity * 2 : 4;
                    Entry *entries = (Entry *)state_alloc(parser->state, cap * sizeof(*entries));
                    if (table->entries)
                        memcpy(entries, table->entries, table->count * sizeof(*entries));
                    table->entries = entries;
                    table->capacity = cap;
                }
                inner = lisp_nil();
                inner.type = LISP_TUPLE;
                {
                    Sequence *pair = (Sequence *)state_alloc(parser->state, sizeof(*pair));
                    pair->items = (LispValue *)state_alloc(parser->state, 2 * sizeof(LispValue));
                    pair->items[0] = key;
                    pair->items[1] = item;
                    pair->count = pair->capacity = 2;
                    inner.as.object = pair;
                }
                table->entries[table->count].key = inner;
                table->entries[table->count].value = lisp_nil();
                table->count++;
            }
        }
        return parser_error(parser, "expected '[' or '{' after '@'");
    }
    if (ch == '"') return parse_string(parser, out);
    if (ch == '\'') {
        LispValue form, quoted;
        Sequence *sequence;
        parser_advance(parser);
        if (parse_form(parser, &form) != 0) return -1;
        quoted = make_sequence(parser->state, LISP_CALL);
        sequence = sequence_of(quoted);
        if (sequence_push(parser->state, sequence, make_symbol(parser->state, "quote", 5)) != 0 ||
            sequence_push(parser->state, sequence, form) != 0) return -1;
        *out = quoted;
        return 0;
    }
    if (ch == ')' || ch == ']' || ch == '}')
        return parser_error(parser, "unexpected closing delimiter");
    return parse_atom(parser, out);
}

static ValueView value_view_tail(ValueView view) {
    if (view.length) {
        view.items++;
        view.length--;
    }
    return view;
}

static LispValue eval_body(LispState *state, ValueView body, size_t environment) {
    LispValue result;
    if (!body.length || state->failed) return lisp_nil();
    result = eval_value(state, body.items[0], environment);
    if (state->failed) return result;
    if (body.length == 1) return result;
    return eval_body(state, value_view_tail(body), environment);
}

static int eval_array_view(LispState *state, ValueView source,
                           Sequence *target, size_t environment) {
    if (!source.length || state->failed) return state->failed ? -1 : 0;
    if (sequence_push(state, target,
                      eval_value(state, source.items[0], environment)) != 0 ||
        state->failed)
        return -1;
    return eval_array_view(state, value_view_tail(source), target, environment);
}

static int bind_let_view(LispState *state, ValueView bindings,
                         size_t environment) {
    LispValue key, value;
    if (!bindings.length || state->failed) return state->failed ? -1 : 0;
    key = bindings.items[0];
    if (key.type != LISP_SYMBOL)
        return lisp_raise(state, "let binding name must be a symbol");
    value = eval_value(state, bindings.items[1], environment);
    if (state->failed ||
        environment_set(state, environment, key.as.string.data,
                        key.as.string.length, value, 0) != 0)
        return -1;
    bindings.items += 2;
    bindings.length -= 2;
    return bind_let_view(state, bindings, environment);
}

static int bind_parameters(LispState *state, ValueView parameters,
                           ValueView arguments, size_t caller_environment,
                           size_t local_environment) {
    LispValue parameter, argument;
    if (!parameters.length || state->failed) return state->failed ? -1 : 0;
    parameter = parameters.items[0];
    argument = eval_value(state, arguments.items[0], caller_environment);
    if (state->failed) return -1;
    if (parameter.type != LISP_SYMBOL)
        return lisp_raise(state, "function parameter must be a symbol");
    if (environment_set(state, local_environment, parameter.as.string.data,
                        parameter.as.string.length, argument, 0) != 0)
        return -1;
    return bind_parameters(state, value_view_tail(parameters),
                           value_view_tail(arguments), caller_environment,
                           local_environment);
}

static int eval_arguments(LispState *state, ValueView arguments,
                          size_t environment, LispValue *evaluated) {
    if (!arguments.length || state->failed) return state->failed ? -1 : 0;
    evaluated[0] = eval_value(state, arguments.items[0], environment);
    if (state->failed) return -1;
    return eval_arguments(state, value_view_tail(arguments), environment,
                          evaluated + 1);
}

static LispValue eval_logic(LispState *state, ValueView forms, size_t environment,
                            int is_and) {
    LispValue result;
    if (!forms.length) return lisp_bool(is_and);
    result = eval_value(state, forms.items[0], environment);
    if (state->failed ||
        (is_and && !truthy(result)) || (!is_and && truthy(result)))
        return result;
    return eval_logic(state, value_view_tail(forms), environment, is_and);
}

static int require_count(LispState *state, const char *name, int argc,
                         int minimum, int maximum) {
    if (argc < minimum || (maximum >= 0 && argc > maximum))
        return failf(state, "%s expects %s%d argument%s, got %d", name,
                     maximum == minimum ? "" : "at least ",
                     maximum == minimum ? minimum : minimum,
                     minimum == 1 ? "" : "s", argc);
    return 0;
}

static LispValue eval_special(LispState *state, Sequence *form,
                              size_t environment, const char *name,
                              size_t name_length, int *recognized) {
    Sequence *args = form;
    LispValue result = lisp_nil();
    *recognized = 1;
#define SPECIAL_IS(s) (name_length == sizeof(s) - 1 && memcmp(name, s, sizeof(s) - 1) == 0)
    if (SPECIAL_IS("quote")) {
        if (args->count != 2) lisp_raise(state, "quote expects one argument");
        else result = quote_value(state, args->items[1], 0);
    } else if (SPECIAL_IS("if")) {
        if (args->count < 3 || args->count > 4) lisp_raise(state, "if expects 2 or 3 arguments");
        else if (truthy(eval_value(state, args->items[1], environment)))
            result = eval_value(state, args->items[2], environment);
        else if (args->count == 4)
            result = eval_value(state, args->items[3], environment);
    } else if (SPECIAL_IS("do")) {
        result = eval_body(state, (ValueView){args->items + 1, args->count - 1},
                           environment);
    } else if (SPECIAL_IS("def") || SPECIAL_IS("set")) {
        if (args->count != 3 || args->items[1].type != LISP_SYMBOL) {
            lisp_raise(state, "def/set expects a symbol and a value");
        } else {
            LispValue v = eval_value(state, args->items[2], environment);
            environment_set(state, SPECIAL_IS("def") ? state->global : environment,
                            args->items[1].as.string.data,
                            args->items[1].as.string.length, v,
                            SPECIAL_IS("set"));
            result = v;
        }
    } else if (SPECIAL_IS("let")) {
        size_t local;
        Sequence *bindings;
        if (args->count < 3 || args->items[1].type != LISP_TUPLE) {
            lisp_raise(state, "let expects a binding tuple and a body");
            return result;
        }
        bindings = sequence_of(args->items[1]);
        if (bindings->count % 2 != 0) {
            lisp_raise(state, "let bindings must be symbol/value pairs");
            return result;
        }
        local = environment_new(state, environment);
        if (local == (size_t)-1) return result;
        if (bind_let_view(state, (ValueView){bindings->items, bindings->count},
                          local) == 0)
            result = eval_body(state,
                (ValueView){args->items + 2, args->count - 2}, local);
    } else if (SPECIAL_IS("fn")) {
        Closure *closure;
        if (args->count < 3 || args->items[1].type != LISP_TUPLE) {
            lisp_raise(state, "fn expects a parameter tuple and a body");
        } else {
            closure = (Closure *)state_alloc(state, sizeof(*closure));
            closure->parameters = sequence_of(args->items[1]);
            closure->body = (Sequence *)state_alloc(state, sizeof(*closure->body));
            closure->body->items = args->items + 2;
            closure->body->count = closure->body->capacity = args->count - 2;
            closure->environment = environment;
            result = lisp_nil();
            result.type = LISP_FUNCTION;
            result.as.object = closure;
        }
    } else if (SPECIAL_IS("while")) {
        if (args->count < 2) {
            lisp_raise(state, "while expects a condition");
        } else {
            ValueView body = {args->items + 2, args->count - 2};
            while (!state->failed && truthy(eval_value(state, args->items[1], environment))) {
                result = eval_body(state, body, environment);
                if (++state->steps > 1000000) {
                    lisp_raise(state, "evaluation step limit exceeded");
                    break;
                }
            }
        }
    } else if (SPECIAL_IS("and") || SPECIAL_IS("or")) {
        result = eval_logic(state, (ValueView){args->items + 1, args->count - 1},
                            environment, SPECIAL_IS("and"));
    } else {
        *recognized = 0;
    }
#undef SPECIAL_IS
    return result;
}

static int eval_table_entries(LispState *state, EntryView source,
                              Table *target, size_t environment) {
    Sequence *pair;
    LispValue key, item;
    if (!source.length || state->failed) return state->failed ? -1 : 0;
    pair = sequence_of(source.items[0].key);
    key = eval_value(state, pair->items[0], environment);
    item = eval_value(state, pair->items[1], environment);
    if (state->failed || lisp_table_put(state,
        (LispValue){.type = LISP_TABLE, .as.object = target}, key, item) != 0)
        return -1;
    source.items++;
    source.length--;
    return eval_table_entries(state, source, target, environment);
}

static LispValue eval_table_literal(LispState *state, Table *source,
                                    size_t environment) {
    Table *table = (Table *)state_alloc(state, sizeof(*table));
    LispValue value = lisp_nil();
    if (!table) return value;
    value.type = LISP_TABLE;
    value.as.object = table;
    eval_table_entries(state, (EntryView){source->entries, source->count},
                       table, environment);
    return value;
}

static int quote_sequence(LispState *state, ValueView source,
                          Sequence *target, int depth) {
    LispValue item;
    if (!source.length || state->failed) return state->failed ? -1 : 0;
    item = quote_value(state, source.items[0], depth + 1);
    if (state->failed || sequence_push(state, target, item) != 0) return -1;
    return quote_sequence(state, value_view_tail(source), target, depth);
}

static int quote_table_entries(LispState *state, EntryView source,
                               Table *target, int depth) {
    Sequence *pair;
    LispValue key, value, target_value = lisp_nil();
    if (!source.length || state->failed) return state->failed ? -1 : 0;
    pair = sequence_of(source.items[0].key);
    key = quote_value(state, pair->items[0], depth + 1);
    value = quote_value(state, pair->items[1], depth + 1);
    target_value.type = LISP_TABLE;
    target_value.as.object = target;
    if (state->failed || lisp_table_put(state, target_value, key, value) != 0)
        return -1;
    source.items++;
    source.length--;
    return quote_table_entries(state, source, target, depth);
}

static LispValue quote_value(LispState *state, LispValue value, int depth) {
    LispType type = value.type == LISP_CALL ? LISP_TUPLE : value.type;
    if (depth > 1024) {
        lisp_raise(state, "maximum quote depth exceeded");
        return lisp_nil();
    }
    if (value.type == LISP_CALL || value.type == LISP_TUPLE ||
        value.type == LISP_ARRAY) {
        Sequence *source = sequence_of(value);
        LispValue result = make_sequence(state, type);
        if (!result.as.object) return lisp_nil();
        quote_sequence(state, (ValueView){source->items, source->count},
                       sequence_of(result), depth);
        return result;
    }
    if (value.type == LISP_TABLE) {
        Table *source = (Table *)value.as.object;
        Table *target = (Table *)state_alloc(state, sizeof(*target));
        LispValue result = lisp_nil();
        if (!target) return result;
        result.type = LISP_TABLE;
        result.as.object = target;
        quote_table_entries(state, (EntryView){source->entries, source->count},
                            target, depth);
        return result;
    }
    return value;
}

static LispValue eval_value(LispState *state, LispValue expression,
                            size_t environment) {
    LispValue result = lisp_nil();
    if (state->failed) return result;
    if (++state->eval_depth > 1024) {
        state->eval_depth--;
        lisp_raise(state, "maximum evaluation depth exceeded");
        return result;
    }
    switch (expression.type) {
        case LISP_NIL: case LISP_BOOL: case LISP_NUMBER: case LISP_STRING:
        case LISP_KEYWORD: case LISP_FUNCTION: case LISP_NATIVE:
            result = expression;
            break;
        case LISP_SYMBOL: {
            Binding *binding = environment_find(state, environment,
                expression.as.string.data, expression.as.string.length);
            if (!binding)
                failf(state, "unbound symbol '%.*s'",
                      (int)expression.as.string.length, expression.as.string.data);
            else result = binding->value;
            break;
        }
        case LISP_TUPLE:
        case LISP_ARRAY: {
            Sequence *source = sequence_of(expression);
            result = make_sequence(state, expression.type);
            if (result.as.object) {
                Sequence *target = sequence_of(result);
                eval_array_view(state, (ValueView){source->items, source->count},
                                target, environment);
            }
            break;
        }
        case LISP_TABLE:
            result = eval_table_literal(state, (Table *)expression.as.object, environment);
            break;
        case LISP_CALL: {
            Sequence *form = sequence_of(expression);
            LispValue callable;
            if (!form->count) break;
            if (form->items[0].type == LISP_SYMBOL) {
                int recognized;
                callable = eval_special(state, form, environment,
                    form->items[0].as.string.data, form->items[0].as.string.length,
                    &recognized);
                if (recognized) {
                    result = callable;
                    break;
                }
            }
            callable = eval_value(state, form->items[0], environment);
            if (callable.type == LISP_FUNCTION) {
                Closure *closure = (Closure *)callable.as.object;
                Sequence *parameters = closure->parameters;
                size_t local;
                if ((size_t)(form->count - 1) != parameters->count) {
                    failf(state, "function expects %lu argument%s, got %lu",
                          (unsigned long)parameters->count,
                          parameters->count == 1 ? "" : "s",
                          (unsigned long)(form->count - 1));
                    break;
                }
                local = environment_new(state, closure->environment);
                if (local == (size_t)-1) break;
                if (bind_parameters(state,
                    (ValueView){parameters->items, parameters->count},
                    (ValueView){form->items + 1, form->count - 1},
                    environment, local) == 0)
                    result = eval_body(state,
                        (ValueView){closure->body->items, closure->body->count},
                        local);
            } else if (callable.type == LISP_NATIVE) {
                Native *native = (Native *)callable.as.object;
                int argc = (int)(form->count - 1);
                LispValue *argv = argc ? (LispValue *)state_alloc(state, argc * sizeof(*argv)) : NULL;
                if (argc && !argv) break;
                if (eval_arguments(state,
                    (ValueView){form->items + 1, (size_t)argc},
                    environment, argv) == 0)
                    result = native->function(state, argc, argv, native->userdata);
            } else if (!state->failed) {
                lisp_raise(state, "first value in a form is not callable");
            }
            break;
        }
    }
    state->eval_depth--;
    return result;
}

static int expect_number(LispState *state, const char *name, LispValue value,
                         double *number) {
    if (value.type != LISP_NUMBER) return failf(state, "%s expects numeric arguments", name);
    *number = value.as.number;
    return 0;
}

typedef enum { FOLD_ADD, FOLD_MULTIPLY, FOLD_SUBTRACT, FOLD_DIVIDE } FoldOp;

static int fold_numbers(LispState *state, const char *name, ValueView values,
                        FoldOp operation, double total, double *result) {
    double number;
    if (!values.length) {
        *result = total;
        return 0;
    }
    if (expect_number(state, name, values.items[0], &number) != 0) return -1;
    if (operation == FOLD_ADD) total += number;
    else if (operation == FOLD_MULTIPLY) total *= number;
    else if (operation == FOLD_SUBTRACT) total -= number;
    else {
        if (number == 0) return lisp_raise(state, "division by zero");
        total /= number;
    }
    return fold_numbers(state, name, value_view_tail(values), operation,
                        total, result);
}

static LispValue native_add(LispState *s, int n, const LispValue *a, void *u) {
    double result; (void)u;
    if (fold_numbers(s, "+", (ValueView){(LispValue *)a, (size_t)n},
                     FOLD_ADD, 0, &result) != 0) return lisp_nil();
    return lisp_number(result);
}
static LispValue native_mul(LispState *s, int n, const LispValue *a, void *u) {
    double result; (void)u;
    if (fold_numbers(s, "*", (ValueView){(LispValue *)a, (size_t)n},
                     FOLD_MULTIPLY, 1, &result) != 0) return lisp_nil();
    return lisp_number(result);
}
static LispValue native_sub(LispState *s, int n, const LispValue *a, void *u) {
    double first, result; (void)u;
    if (require_count(s, "-", n, 1, -1) ||
        expect_number(s, "-", a[0], &first)) return lisp_nil();
    if (n == 1) return lisp_number(-first);
    if (fold_numbers(s, "-", (ValueView){(LispValue *)a + 1, (size_t)n - 1},
                     FOLD_SUBTRACT, first, &result) != 0) return lisp_nil();
    return lisp_number(result);
}
static LispValue native_div(LispState *s, int n, const LispValue *a, void *u) {
    double first, result; (void)u;
    if (require_count(s, "/", n, 1, -1) ||
        expect_number(s, "/", a[0], &first)) return lisp_nil();
    if (n == 1) {
        if (first == 0) {
            lisp_raise(s, "division by zero");
            return lisp_nil();
        }
        return lisp_number(1 / first);
    }
    if (fold_numbers(s, "/", (ValueView){(LispValue *)a + 1, (size_t)n - 1},
                     FOLD_DIVIDE, first, &result) != 0) return lisp_nil();
    return lisp_number(result);
}

typedef enum { CMP_EQ, CMP_LT, CMP_LE, CMP_GT, CMP_GE } Compare;
static int compare_values(LispState *state, ValueView values, Compare operation) {
    double left, right;
    int matches;
    if (values.length < 2) return 1;
    if (operation == CMP_EQ) {
        matches = value_equal(values.items[0], values.items[1]);
    } else {
        if (expect_number(state, "comparison", values.items[0], &left) ||
            expect_number(state, "comparison", values.items[1], &right))
            return 0;
        matches = operation == CMP_LT ? left < right :
            operation == CMP_LE ? left <= right :
            operation == CMP_GT ? left > right : left >= right;
    }
    if (!matches || state->failed) return 0;
    values.items++;
    values.length--;
    return compare_values(state, values, operation);
}

static LispValue native_compare(LispState *s, int n, const LispValue *a, void *u) {
    Compare op = (Compare)(size_t)u;
    if (require_count(s, "comparison", n, 2, -1)) return lisp_nil();
    return lisp_bool(compare_values(s, (ValueView){(LispValue *)a, (size_t)n},
                                    op));
}

static LispValue native_not(LispState *s, int n, const LispValue *a, void *u) {
    (void)u;
    if (require_count(s, "not", n, 1, 1)) return lisp_nil();
    return lisp_bool(!truthy(a[0]));
}

static void print_values(LispState *state, ValueView values, int *first) {
    const char *text;
    if (!values.length) return;
    text = lisp_value_to_string(state, values.items[0]);
    if (!*first) putchar(' ');
    fputs(text ? text : "nil", stdout);
    *first = 0;
    print_values(state, value_view_tail(values), first);
}

static LispValue native_print_common(LispState *s, int n, const LispValue *a,
                                     void *userdata) {
    int newline = (int)(size_t)userdata, first = 1;
    print_values(s, (ValueView){(LispValue *)a, (size_t)n}, &first);
    if (newline) putchar('\n');
    return n ? a[n - 1] : lisp_nil();
}

static int tuple_append_view(LispState *state, Sequence *target,
                             ValueView values) {
    if (!values.length || state->failed) return state->failed ? -1 : 0;
    if (sequence_push(state, target, values.items[0]) != 0) return -1;
    return tuple_append_view(state, target, value_view_tail(values));
}

static LispValue native_tuple(LispState *s, int n, const LispValue *a, void *u) {
    LispValue value = make_sequence(s, LISP_TUPLE);
    (void)u;
    tuple_append_view(s, sequence_of(value), (ValueView){(LispValue *)a, (size_t)n});
    return value;
}
static LispValue native_get(LispState *s, int n, const LispValue *a, void *u) {
    (void)u;
    if (require_count(s, "get", n, 2, 3)) return lisp_nil();
    if (a[0].type == LISP_TABLE) {
        Table *table = (Table *)a[0].as.object;
        size_t index = table_find_index(
            (EntryView){table->entries, table->count}, a[1], 0);
        return index == (size_t)-1 ?
            (n == 3 ? a[2] : lisp_nil()) : table->entries[index].value;
    }
    if (a[0].type == LISP_ARRAY || a[0].type == LISP_TUPLE) {
        Sequence *sequence = sequence_of(a[0]);
        if (a[1].type != LISP_NUMBER || a[1].as.number < 0 ||
            floor(a[1].as.number) != a[1].as.number ||
            a[1].as.number >= (double)sequence->count)
            return n == 3 ? a[2] : lisp_nil();
        return sequence->items[(size_t)a[1].as.number];
    }
    lisp_raise(s, "get expects a table, tuple, or array");
    return lisp_nil();
}
static LispValue native_put(LispState *s, int n, const LispValue *a, void *u) {
    (void)u;
    if (require_count(s, "put", n, 3, 3)) return lisp_nil();
    if (a[0].type == LISP_TABLE) {
        if (lisp_table_put(s, a[0], a[1], a[2]) != 0) return lisp_nil();
        return a[0];
    }
    if (a[0].type == LISP_ARRAY) {
        Sequence *sequence = sequence_of(a[0]);
        if (a[1].type != LISP_NUMBER || a[1].as.number < 0 ||
            floor(a[1].as.number) != a[1].as.number ||
            a[1].as.number >= (double)sequence->count) {
            lisp_raise(s, "array index out of range");
            return lisp_nil();
        }
        sequence->items[(size_t)a[1].as.number] = a[2];
        return a[0];
    }
    if (a[0].type == LISP_TUPLE)
        lisp_raise(s, "cannot mutate an immutable tuple");
    else
        lisp_raise(s, "put expects a table or array");
    return lisp_nil();
}
static LispValue native_length(LispState *s, int n, const LispValue *a, void *u) {
    size_t length = 0; (void)u;
    if (require_count(s, "length", n, 1, 1)) return lisp_nil();
    if (a[0].type == LISP_ARRAY || a[0].type == LISP_TUPLE)
        length = sequence_of(a[0])->count;
    else if (a[0].type == LISP_TABLE)
        length = ((Table *)a[0].as.object)->count;
    else if (a[0].type == LISP_STRING)
        length = a[0].as.string.length;
    else {
        lisp_raise(s, "length expects a collection or string");
        return lisp_nil();
    }
    return lisp_number((double)length);
}

static void define_native(LispState *state, const char *name, LispNativeFn fn,
                          void *userdata) {
    Native *native = (Native *)state_alloc(state, sizeof(*native));
    LispValue value = lisp_nil();
    native->function = fn;
    native->userdata = userdata;
    value.type = LISP_NATIVE;
    value.as.object = native;
    environment_set(state, state->global, name, strlen(name), value, 0);
}

void lisp_define_native(LispState *state, const char *name,
                        LispNativeFn function, void *userdata) {
    if (state && name && function)
        define_native(state, name, function, userdata);
}

void lisp_define(LispState *state, const char *name, LispValue value) {
    if (state && name)
        environment_set(state, state->global, name, strlen(name), value, 0);
}

LispState *lisp_state_new(void) {
    LispState *state = (LispState *)calloc(1, sizeof(*state));
    if (!state) return NULL;
    state->global = environment_new(state, (size_t)-1);
    if (state->global == (size_t)-1) {
        lisp_state_free(state);
        return NULL;
    }
    define_native(state, "+", native_add, NULL);
    define_native(state, "-", native_sub, NULL);
    define_native(state, "*", native_mul, NULL);
    define_native(state, "/", native_div, NULL);
    define_native(state, "=", native_compare, (void *)(size_t)CMP_EQ);
    define_native(state, "<", native_compare, (void *)(size_t)CMP_LT);
    define_native(state, "<=", native_compare, (void *)(size_t)CMP_LE);
    define_native(state, ">", native_compare, (void *)(size_t)CMP_GT);
    define_native(state, ">=", native_compare, (void *)(size_t)CMP_GE);
    define_native(state, "not", native_not, NULL);
    define_native(state, "print", native_print_common, NULL);
    define_native(state, "println", native_print_common, (void *)(size_t)1);
    define_native(state, "tuple", native_tuple, NULL);
    define_native(state, "get", native_get, NULL);
    define_native(state, "put", native_put, NULL);
    define_native(state, "length", native_length, NULL);
    return state;
}

void lisp_state_free(LispState *state) {
    size_t i;
    if (!state) return;
    for (i = 0; i < state->allocation_count; i++)
        free(state->allocations[i]);
    free(state->allocations);
    for (i = 0; i < state->environment_count; i++)
        free(state->environments[i].bindings);
    free(state->environments);
    free(state);
}

int lisp_eval(LispState *state, const char *source, LispValue *result) {
    Parser parser;
    LispValue value = lisp_nil();
    if (!state || !source) return -1;
    state->failed = 0;
    state->error[0] = '\0';
    state->eval_depth = 0;
    state->steps = 0;
    parser.state = state;
    parser.source = source;
    parser.length = strlen(source);
    parser.offset = 0;
    parser.line = parser.column = 1;
    while (!state->failed) {
        parser_skip_space(&parser);
        if (!parser_peek(&parser)) break;
        if (parse_form(&parser, &value) != 0) break;
        value = eval_value(state, value, state->global);
    }
    if (result) *result = value;
    return state->failed ? -1 : 0;
}

const char *lisp_last_error(const LispState *state) {
    return state ? state->error : "";
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuffer;

static int buffer_append(StringBuffer *buffer, const char *text, size_t length) {
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 64;
        char *data;
        while (capacity < buffer->length + length + 1) capacity *= 2;
        data = (char *)realloc(buffer->data, capacity);
        if (!data) return -1;
        buffer->data = data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int format_value(StringBuffer *buffer, LispValue value, int depth);

static int format_values(StringBuffer *buffer, ValueView values, int depth) {
    if (!values.length) return 0;
    if (format_value(buffer, values.items[0], depth + 1) != 0) return -1;
    values.items++;
    values.length--;
    if (!values.length) return 0;
    if (buffer_append(buffer, " ", 1) != 0) return -1;
    return format_values(buffer, values, depth);
}

static int format_entries(StringBuffer *buffer, EntryView entries, int depth,
                          int first) {
    if (!entries.length) return 0;
    if (!first && buffer_append(buffer, " ", 1) != 0) return -1;
    if (format_value(buffer, entries.items[0].key, depth + 1) != 0 ||
        buffer_append(buffer, " ", 1) != 0 ||
        format_value(buffer, entries.items[0].value, depth + 1) != 0)
        return -1;
    entries.items++;
    entries.length--;
    return format_entries(buffer, entries, depth, 0);
}

static int format_value(StringBuffer *buffer, LispValue value, int depth) {
    char number[64];
    size_t i;
    if (depth > 32) return buffer_append(buffer, "...", 3);
    switch (value.type) {
        case LISP_NIL: return buffer_append(buffer, "nil", 3);
        case LISP_BOOL:
            return buffer_append(buffer, value.as.boolean ? "true" : "false",
                                 value.as.boolean ? 4 : 5);
        case LISP_NUMBER:
            snprintf(number, sizeof(number), "%.15g", value.as.number);
            return buffer_append(buffer, number, strlen(number));
        case LISP_STRING:
            if (buffer_append(buffer, "\"", 1)) return -1;
            for (i = 0; i < value.as.string.length; i++) {
                char ch = value.as.string.data[i];
                const char *escape = ch == '\n' ? "\\n" : ch == '\r' ? "\\r" :
                                     ch == '\t' ? "\\t" : ch == '"' ? "\\\"" :
                                     ch == '\\' ? "\\\\" : NULL;
                if (escape) {
                    if (buffer_append(buffer, escape, 2)) return -1;
                } else if (buffer_append(buffer, &ch, 1)) return -1;
            }
            return buffer_append(buffer, "\"", 1);
        case LISP_KEYWORD:
            if (buffer_append(buffer, ":", 1)) return -1;
            return buffer_append(buffer, value.as.string.data, value.as.string.length);
        case LISP_SYMBOL:
            return buffer_append(buffer, value.as.string.data, value.as.string.length);
        case LISP_CALL: case LISP_TUPLE: case LISP_ARRAY: {
            Sequence *sequence = sequence_of(value);
            const char *open = value.type == LISP_CALL ? "(" :
                               value.type == LISP_ARRAY ? "@[" : "[";
            size_t open_length = value.type == LISP_ARRAY ? 2 : 1;
            const char *close = value.type == LISP_CALL ? ")" : "]";
            if (buffer_append(buffer, open, open_length) != 0) return -1;
            if (format_values(buffer,
                (ValueView){sequence->items, sequence->count}, depth) != 0)
                return -1;
            return buffer_append(buffer, close, 1);
        }
        case LISP_TABLE: {
            Table *table = (Table *)value.as.object;
            if (buffer_append(buffer, "{", 1)) return -1;
            if (format_entries(buffer,
                (EntryView){table->entries, table->count}, depth, 1) != 0)
                return -1;
            return buffer_append(buffer, "}", 1);
        }
        case LISP_FUNCTION: return buffer_append(buffer, "<function>", 10);
        case LISP_NATIVE: return buffer_append(buffer, "<native>", 8);
    }
    return buffer_append(buffer, "nil", 3);
}

const char *lisp_value_to_string(LispState *state, LispValue value) {
    StringBuffer buffer = {0};
    char *result;
    if (format_value(&buffer, value, 0) != 0) {
        free(buffer.data);
        lisp_raise(state, "out of memory formatting value");
        return NULL;
    }
    result = state_strndup(state, buffer.data ? buffer.data : "", buffer.length);
    free(buffer.data);
    return result;
}
