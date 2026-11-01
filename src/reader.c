/*
 * reader.c - lexical analysis and reading of source text into values.
 *
 * MIT License. See LICENSE.
 *
 * Implements spec §3 and §4. The reader builds runtime values directly
 * ([P-007]): a paren form becomes a paren tuple, `[..]` a bracket tuple,
 * `@(..)`/`@[..]` an array, `{..}` a struct and `@{..}` a table.
 */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Reader {
    Lang *L;
    const char *src;
    size_t len, pos;
    const char *chunk;
    int line, col;

    bool failed;
    bool incomplete;
    int errline, errcol;
    char err[256];
} Reader;

static const char SYMBOL_SPECIAL[] = "!$%&*+-./<=>?^_";

static bool is_sym_initial(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c && strchr(SYMBOL_SPECIAL, c) != NULL);
}

static bool is_sym_sub(int c) {
    return is_sym_initial(c) || (c >= '0' && c <= '9');
}

static bool is_digit(int c) { return c >= '0' && c <= '9'; }

static bool is_hex(int c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int r_peek(Reader *r) {
    return r->pos < r->len ? (unsigned char)r->src[r->pos] : -1;
}

static int r_peek2(Reader *r) {
    return r->pos + 1 < r->len ? (unsigned char)r->src[r->pos + 1] : -1;
}

static int r_next(Reader *r) {
    if (r->pos >= r->len) return -1;
    int c = (unsigned char)r->src[r->pos++];
    if (c == '\n') {
        r->line++;
        r->col = 1;
    } else {
        r->col++;
    }
    return c;
}

static void r_fail(Reader *r, bool incomplete, const char *fmt, ...) {
    if (r->failed) return;
    r->failed = true;
    r->incomplete = incomplete;
    r->errline = r->line;
    r->errcol = r->col;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, sizeof r->err, fmt, ap);
    va_end(ap);
}

static void r_skip_space(Reader *r) {
    for (;;) {
        int c = r_peek(r);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            r_next(r);
            continue;
        }
        if (c == ';') {
            while (r_peek(r) != -1 && r_peek(r) != '\n') r_next(r);
            continue;
        }
        break;
    }
}

static Value r_form(Reader *r);

/* Collect forms until `closer`, returning them through a malloc'd array. */
static Value *r_collect(Reader *r, int closer, size_t *count) {
    Value *elems = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        r_skip_space(r);
        int c = r_peek(r);
        if (c == -1) {
            r_fail(r, true, "unexpected end of input, expected '%c'", closer);
            free(elems);
            return NULL;
        }
        if (c == closer) {
            r_next(r);
            break;
        }
        if (c == ')' || c == ']' || c == '}') {
            r_fail(r, false, "unexpected '%c'", c);
            free(elems);
            return NULL;
        }
        Value v = r_form(r);
        if (r->failed) {
            free(elems);
            return NULL;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            Value *p = realloc(elems, cap * sizeof(Value));
            if (!p) {
                free(elems);
                r_fail(r, false, "out of memory");
                return NULL;
            }
            elems = p;
        }
        elems[n++] = v;
    }
    *count = n;
    return elems;
}

static Value r_string(Reader *r) {
    r_next(r); /* opening quote */
    size_t cap = 32, n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        r_fail(r, false, "out of memory");
        return v_nil();
    }
    for (;;) {
        int c = r_next(r);
        if (c == -1) {
            free(buf);
            r_fail(r, true, "unexpected end of input in string");
            return v_nil();
        }
        if (c == '"') break;
        if (c == '\\') {
            int e = r_next(r);
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            case '0': c = '\0'; break;
            case 'x': {
                int h1 = r_next(r);
                int h2 = r_next(r);
                if (!is_hex(h1) || !is_hex(h2)) {
                    free(buf);
                    r_fail(r, false, "invalid \\x escape in string");
                    return v_nil();
                }
                char hexbuf[3] = {(char)h1, (char)h2, 0};
                c = (int)strtol(hexbuf, NULL, 16);
                break;
            }
            case -1:
                free(buf);
                r_fail(r, true, "unexpected end of input in string");
                return v_nil();
            default:
                free(buf);
                r_fail(r, false, "invalid escape '\\%c' in string", e);
                return v_nil();
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                r_fail(r, false, "out of memory");
                return v_nil();
            }
            buf = p;
        }
        buf[n++] = (char)c;
    }
    Value v = v_string(r->L, buf, n);
    free(buf);
    return v;
}

static bool is_number_start(Reader *r) {
    int c = r_peek(r);
    if (is_digit(c)) return true;
    if (c == '-' && is_digit(r_peek2(r))) return true;
    return false;
}

static bool is_token_delim(int c) {
    if (c == -1) return true;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return true;
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
        c == '@' || c == '"' || c == ';' || c == ':')
        return true;
    return false;
}

static Value r_number(Reader *r) {
    size_t start = r->pos;
    int c = r_peek(r);
    bool neg = false;
    if (c == '-') {
        neg = true;
        r_next(r);
        c = r_peek(r);
    }
    if (c == '0' && (r_peek2(r) == 'x' || r_peek2(r) == 'X')) {
        if (neg) {
            r_fail(r, false, "negative hexadecimal literals are not allowed");
            return v_nil();
        }
        r_next(r);
        r_next(r);
        if (!is_hex(r_peek(r))) {
            r_fail(r, false, "hexadecimal literal requires at least one digit");
            return v_nil();
        }
        while (is_hex(r_peek(r))) r_next(r);
        if (!is_token_delim(r_peek(r))) {
            r_fail(r, false, "invalid character in number");
            return v_nil();
        }
        size_t len = r->pos - start;
        char *tmp = malloc(len + 1);
        if (!tmp) {
            r_fail(r, false, "out of memory");
            return v_nil();
        }
        memcpy(tmp, r->src + start, len);
        tmp[len] = '\0';
        errno = 0;
        unsigned long long uv = strtoull(tmp + 2, NULL, 16);
        free(tmp);
        if (errno == ERANGE || uv > (unsigned long long)INT64_MAX) {
            r_fail(r, false, "integer literal out of range");
            return v_nil();
        }
        return v_int((int64_t)uv);
    }
    while (is_digit(r_peek(r))) r_next(r);
    bool isfloat = false;
    if (r_peek(r) == '.' && is_digit(r_peek2(r))) {
        isfloat = true;
        r_next(r);
        while (is_digit(r_peek(r))) r_next(r);
    }
    if (r_peek(r) == 'e' || r_peek(r) == 'E') {
        int n1 = r_peek2(r);
        size_t save = r->pos;
        int savecol = r->col;
        r_next(r);
        if (n1 == '+' || n1 == '-') {
            r_next(r);
            n1 = r_peek(r);
        }
        if (is_digit(n1)) {
            isfloat = true;
            while (is_digit(r_peek(r))) r_next(r);
        } else {
            r->pos = save;
            r->col = savecol;
        }
    }
    if (!is_token_delim(r_peek(r))) {
        r_fail(r, false, "invalid character in number");
        return v_nil();
    }
    size_t len = r->pos - start;
    char *tmp = malloc(len + 1);
    if (!tmp) {
        r_fail(r, false, "out of memory");
        return v_nil();
    }
    memcpy(tmp, r->src + start, len);
    tmp[len] = '\0';
    errno = 0;
    if (isfloat) {
        double d = strtod(tmp, NULL);
        free(tmp);
        if (errno == ERANGE && (d == 0.0 || d > 1e308)) {
            r_fail(r, false, "float literal out of range");
            return v_nil();
        }
        return v_float(d);
    }
    long long iv = strtoll(tmp, NULL, 10);
    free(tmp);
    if (errno == ERANGE) {
        r_fail(r, false, "integer literal out of range");
        return v_nil();
    }
    return v_int((int64_t)iv);
}

static Value r_symbol(Reader *r) {
    size_t start = r->pos;
    while (is_sym_sub(r_peek(r))) r_next(r);
    size_t len = r->pos - start;
    const char *s = r->src + start;
    if (len == 3 && memcmp(s, "nil", 3) == 0) return v_nil();
    if (len == 4 && memcmp(s, "true", 4) == 0) return v_bool(true);
    if (len == 5 && memcmp(s, "false", 5) == 0) return v_bool(false);
    return v_symbol(r->L, s, len);
}

static Value r_keyword(Reader *r) {
    r_next(r); /* ':' */
    int c = r_peek(r);
    if (!is_sym_initial(c)) {
        r_fail(r, false, "keyword must be followed by a symbol");
        return v_nil();
    }
    size_t start = r->pos;
    while (is_sym_sub(r_peek(r))) r_next(r);
    return v_keyword(r->L, r->src + start, r->pos - start);
}

static Value r_form(Reader *r) {
    int c = r_peek(r);
    int line = r->line, col = r->col;

    if (c == '(') {
        r_next(r);
        size_t n = 0;
        Value *elems = r_collect(r, ')', &n);
        if (r->failed) return v_nil();
        Value v = v_seq_from(r->L, elems, n, false, true);
        free(elems);
        v.as.seq->line = line;
        v.as.seq->col = col;
        return v;
    }
    if (c == '[') {
        r_next(r);
        size_t n = 0;
        Value *elems = r_collect(r, ']', &n);
        if (r->failed) return v_nil();
        Value v = v_seq_from(r->L, elems, n, false, false);
        free(elems);
        v.as.seq->line = line;
        v.as.seq->col = col;
        return v;
    }
    if (c == '{') {
        r_next(r);
        size_t n = 0;
        Value *elems = r_collect(r, '}', &n);
        if (r->failed) return v_nil();
        if (n % 2 != 0) {
            free(elems);
            r_fail(r, false, "struct literal requires an even number of forms");
            return v_nil();
        }
        Value v = v_map_empty(r->L, false);
        for (size_t i = 0; i < n; i += 2) {
            map_put(r->L, v, elems[i], elems[i + 1]);
        }
        free(elems);
        v.as.map->line = line;
        v.as.map->col = col;
        return v;
    }
    if (c == '@') {
        int n2 = r_peek2(r);
        if (n2 == '(' || n2 == '[') {
            r_next(r);
            r_next(r);
            size_t n = 0;
            Value *elems = r_collect(r, n2 == '(' ? ')' : ']', &n);
            if (r->failed) return v_nil();
            Value v = v_seq_from(r->L, elems, n, true, false);
            free(elems);
            v.as.seq->line = line;
            v.as.seq->col = col;
            return v;
        }
        if (n2 == '{') {
            r_next(r);
            r_next(r);
            size_t n = 0;
            Value *elems = r_collect(r, '}', &n);
            if (r->failed) return v_nil();
            if (n % 2 != 0) {
                free(elems);
                r_fail(r, false,
                       "table literal requires an even number of forms");
                return v_nil();
            }
            Value v = v_map_empty(r->L, true);
            for (size_t i = 0; i < n; i += 2) map_put(r->L, v, elems[i], elems[i + 1]);
            free(elems);
            v.as.map->line = line;
            v.as.map->col = col;
            return v;
        }
        r_next(r);
        r_fail(r, false, "'@' must be immediately followed by '(', '[' or '{'");
        return v_nil();
    }
    if (c == ')' || c == ']' || c == '}') {
        r_next(r);
        r_fail(r, false, "unexpected '%c'", c);
        return v_nil();
    }
    if (c == '"') return r_string(r);
    if (c == ':') return r_keyword(r);
    if (is_number_start(r)) return r_number(r);
    if (is_sym_initial(c)) return r_symbol(r);
    if (c == -1) {
        r_fail(r, true, "unexpected end of input");
        return v_nil();
    }
    r_next(r);
    r_fail(r, false, "unexpected character '%c'", c);
    return v_nil();
}

bool lang_read_forms(Lang *L, const char *src, size_t len, const char *chunk,
                     Value **out, size_t *count, bool *incomplete) {
    Reader r;
    memset(&r, 0, sizeof r);
    r.L = L;
    r.src = src;
    r.len = len;
    r.chunk = chunk;
    r.line = 1;
    r.col = 1;

    Value *arr = NULL;
    size_t n = 0, cap = 0;
    *out = NULL;
    *count = 0;
    if (incomplete) *incomplete = false;

    for (;;) {
        r_skip_space(&r);
        if (r.pos >= r.len || r.failed) break;
        Value v = r_form(&r);
        if (r.failed) break;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            Value *p = realloc(arr, cap * sizeof(Value));
            if (!p) {
                free(arr);
                lang_error(L, "out of memory");
            }
            arr = p;
        }
        arr[n++] = v;
    }

    if (r.failed) {
        free(arr);
        if (r.incomplete) {
            if (incomplete) *incomplete = true;
            return false;
        }
        if (r.chunk)
            lang_error(L, "%s:%d:%d: %s", r.chunk, r.errline, r.errcol, r.err);
        lang_error(L, "%d:%d: %s", r.errline, r.errcol, r.err);
    }

    *out = arr;
    *count = n;
    return true;
}
