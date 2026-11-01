/*
 * value.c - values, buffers/views, associative collections, interning,
 *           equality and printing.
 *
 * MIT License. See LICENSE.
 */
#include "internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena memory                                                       */
/* ------------------------------------------------------------------ */

void *lang_alloc(Lang *L, size_t n) {
    if (n == 0) n = 1;
    MemBlock *m = malloc(sizeof(MemBlock) + n);
    if (!m) {
        fprintf(stderr, "lang: out of memory\n");
        exit(70);
    }
    m->h.next = L->blocks;
    m->h.size = n;
    L->blocks = m;
    void *p = (void *)(m + 1);
    memset(p, 0, n);
    return p;
}

char *lang_strndup(Lang *L, const char *s, size_t n) {
    char *p = lang_alloc(L, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void lang_free_arena(Lang *L) {
    MemBlock *m = L->blocks;
    while (m) {
        MemBlock *next = m->h.next;
        free(m);
        m = next;
    }
    L->blocks = NULL;
}

void lang_error(Lang *L, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(L->errmsg, sizeof L->errmsg, fmt, ap);
    va_end(ap);
    if (L->err_jmp) longjmp(*L->err_jmp, 1);
    fprintf(stderr, "lang: error: %s\n", L->errmsg);
    exit(70);
}

/* ------------------------------------------------------------------ */
/* Interning                                                          */
/* ------------------------------------------------------------------ */

static uint32_t hash_bytes(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static void intern_grow(Lang *L) {
    size_t cap = L->intern_cap * 2;
    InternEntry **buckets = calloc(cap, sizeof(InternEntry *));
    if (!buckets) {
        fprintf(stderr, "lang: out of memory\n");
        exit(70);
    }
    for (size_t i = 0; i < L->intern_cap; i++) {
        InternEntry *e = L->intern[i];
        while (e) {
            InternEntry *next = e->next;
            size_t idx = e->hash & (cap - 1);
            e->next = buckets[idx];
            buckets[idx] = e;
            e = next;
        }
    }
    free(L->intern);
    L->intern = buckets;
    L->intern_cap = cap;
}

static Value intern_name(Lang *L, const char *s, size_t n, bool keyword) {
    uint32_t h = hash_bytes(s, n);
    size_t idx = h & (L->intern_cap - 1);
    for (InternEntry *e = L->intern[idx]; e; e = e->next) {
        if (e->hash == h && e->len == n && e->keyword == keyword &&
            memcmp(e->name, s, n) == 0)
            return e->value;
    }
    if (L->intern_n + 1 > L->intern_cap - (L->intern_cap >> 2)) intern_grow(L);
    idx = h & (L->intern_cap - 1);
    InternEntry *e = lang_alloc(L, sizeof *e);
    e->name = lang_strndup(L, s, n);
    e->len = n;
    e->keyword = keyword;
    e->hash = h;
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = keyword ? V_KEYWORD : V_SYMBOL;
    v.as.str.data = e->name;
    v.as.str.len = n;
    e->value = v;
    e->next = L->intern[idx];
    L->intern[idx] = e;
    L->intern_n++;
    return v;
}

Value v_symbol(Lang *L, const char *s, size_t n) {
    return intern_name(L, s, n, false);
}

Value v_keyword(Lang *L, const char *s, size_t n) {
    return intern_name(L, s, n, true);
}

/* ------------------------------------------------------------------ */
/* Constructors                                                       */
/* ------------------------------------------------------------------ */

Value v_nil(void) {
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_NIL;
    return v;
}

Value v_bool(bool b) {
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_BOOL;
    v.as.b = b;
    return v;
}

Value v_int(int64_t i) {
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_INT;
    v.as.i = i;
    return v;
}

Value v_float(double f) {
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_FLOAT;
    v.as.f = f;
    return v;
}

Value v_string(Lang *L, const char *s, size_t n) {
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_STRING;
    v.as.str.data = lang_strndup(L, s, n);
    v.as.str.len = n;
    return v;
}

static FnObj *fn_new(Lang *L, const char *name, NativeFn fn) {
    FnObj *f = lang_alloc(L, sizeof *f);
    f->native = fn;
    f->name = name ? lang_strndup(L, name, strlen(name)) : NULL;
    return f;
}

Value v_fn_native(Lang *L, const char *name, NativeFn fn) {
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_FN;
    v.as.fn = fn_new(L, name, fn);
    return v;
}

Value v_fn_native_closure(Lang *L, const char *name, NativeFn fn, int ncaptured,
                          const Value *captured) {
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = V_FN;
    FnObj *f = fn_new(L, name, fn);
    f->ncaptured = ncaptured;
    if (ncaptured > 0) {
        f->captured = lang_alloc(L, (size_t)ncaptured * sizeof(Value));
        memcpy(f->captured, captured, (size_t)ncaptured * sizeof(Value));
    }
    v.as.fn = f;
    return v;
}

/* ------------------------------------------------------------------ */
/* Sequences                                                          */
/* ------------------------------------------------------------------ */

Buf *buf_new(Lang *L, size_t cap) {
    Buf *b = lang_alloc(L, sizeof *b);
    b->cap = cap;
    b->alloc = cap;
    b->slots = cap ? lang_alloc(L, cap * sizeof(Value)) : NULL;
    return b;
}

Value v_seq(Lang *L, Buf *buf, size_t start, size_t len, bool mut, bool paren) {
    SeqObj *s = lang_alloc(L, sizeof *s);
    s->buf = buf;
    s->start = start;
    s->len = len;
    s->mutable_ = mut;
    s->paren = paren;
    s->line = 0;
    s->col = 0;
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = mut ? V_ARRAY : V_TUPLE;
    v.as.seq = s;
    return v;
}

Value v_seq_from(Lang *L, const Value *elems, size_t n, bool mut, bool paren) {
    Buf *b = buf_new(L, n);
    if (n) memcpy(b->slots, elems, n * sizeof(Value));
    return v_seq(L, b, 0, n, mut, paren);
}

/* Build a sequence that references caller-provided slots (rest parameters). */
Value v_seq_over(Lang *L, Value *slots, size_t cap, size_t start, size_t len,
                 bool mut, bool paren) {
    Buf *b = lang_alloc(L, sizeof *b);
    b->slots = slots;
    b->cap = cap;
    b->alloc = cap;
    return v_seq(L, b, start, len, mut, paren);
}

Value v_seq_view(Lang *L, Value src, size_t start, size_t len) {
    SeqObj *s = src.as.seq;
    return v_seq(L, s->buf, start, len, s->mutable_, s->paren);
}

size_t seq_len(Value v) { return v.as.seq->len; }

Value seq_at(Value v, size_t i) {
    SeqObj *s = v.as.seq;
    return s->buf->slots[s->start + i];
}

void seq_set_at(Value v, size_t i, Value x) {
    SeqObj *s = v.as.seq;
    s->buf->slots[s->start + i] = x;
}

/* ------------------------------------------------------------------ */
/* Associative collections                                            */
/* ------------------------------------------------------------------ */

Value v_map_empty(Lang *L, bool mut) {
    MapObj *m = lang_alloc(L, sizeof *m);
    m->mutable_ = mut;
    Value v;
    memset(&v, 0, sizeof v);
    v.tag = mut ? V_TABLE : V_STRUCT;
    v.as.map = m;
    return v;
}

bool map_lookup(Value map, Value key, Value *out) {
    MapObj *m = map.as.map;
    for (size_t i = 0; i < m->n; i++) {
        /* Keys are compared structurally; see [T-017]/[T-018]. */
        if (value_equal_inline(m->entries[i].key, key)) {
            if (out) *out = m->entries[i].val;
            return true;
        }
    }
    return false;
}

void map_put(Lang *L, Value map, Value key, Value val) {
    MapObj *m = map.as.map;
    for (size_t i = 0; i < m->n; i++) {
        if (value_equal(L, m->entries[i].key, key)) {
            m->entries[i].val = val;
            return;
        }
    }
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        Entry *entries = lang_alloc(L, cap * sizeof(Entry));
        if (m->n) memcpy(entries, m->entries, m->n * sizeof(Entry));
        m->entries = entries;
        m->cap = cap;
    }
    m->entries[m->n].key = key;
    m->entries[m->n].val = val;
    m->n++;
}

bool map_del(Lang *L, Value map, Value key) {
    MapObj *m = map.as.map;
    for (size_t i = 0; i < m->n; i++) {
        if (value_equal(L, m->entries[i].key, key)) {
            memmove(&m->entries[i], &m->entries[i + 1],
                    (m->n - i - 1) * sizeof(Entry));
            m->n--;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Equality                                                           */
/* ------------------------------------------------------------------ */

static bool eq_depth(Lang *L, Value a, Value b, int depth);

static bool num_equal(Value a, Value b) {
    if (a.tag == V_INT && b.tag == V_INT) return a.as.i == b.as.i;
    double x = a.tag == V_INT ? (double)a.as.i : a.as.f;
    double y = b.tag == V_INT ? (double)b.as.i : b.as.f;
    return x == y;
}

static bool eq_inline(Value a, Value b, int depth) {
    if (depth > 256) return false;
    if (a.tag != b.tag) {
        if ((a.tag == V_INT || a.tag == V_FLOAT) &&
            (b.tag == V_INT || b.tag == V_FLOAT))
            return num_equal(a, b);
        return false;
    }
    switch (a.tag) {
    case V_NIL:
        return true;
    case V_BOOL:
        return a.as.b == b.as.b;
    case V_INT:
        return a.as.i == b.as.i;
    case V_FLOAT:
        return a.as.f == b.as.f;
    case V_STRING:
    case V_SYMBOL:
    case V_KEYWORD:
        return a.as.str.len == b.as.str.len &&
               memcmp(a.as.str.data, b.as.str.data, a.as.str.len) == 0;
    case V_TUPLE:
    case V_ARRAY: {
        SeqObj *x = a.as.seq, *y = b.as.seq;
        if (x->len != y->len) return false;
        if (a.tag == V_TUPLE && x->paren != y->paren) return false; /* [T-014] */
        for (size_t i = 0; i < x->len; i++)
            if (!eq_inline(seq_at(a, i), seq_at(b, i), depth + 1)) return false;
        return true;
    }
    case V_STRUCT:
    case V_TABLE: {
        MapObj *x = a.as.map, *y = b.as.map;
        if (x->n != y->n) return false;
        for (size_t i = 0; i < x->n; i++) {
            bool found = false;
            for (size_t j = 0; j < y->n; j++) {
                if (eq_inline(x->entries[i].key, y->entries[j].key, depth + 1)) {
                    found = eq_inline(x->entries[i].val, y->entries[j].val,
                                      depth + 1);
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }
    case V_FN:
        return a.as.fn == b.as.fn;
    case V_RECUR:
        return false;
    }
    return false;
}

bool value_equal_inline(Value a, Value b) { return eq_inline(a, b, 0); }

static bool eq_depth(Lang *L, Value a, Value b, int depth) {
    if (depth > 256) lang_error(L, "=: structures nested too deeply");
    if (a.tag != b.tag) {
        if ((a.tag == V_INT || a.tag == V_FLOAT) &&
            (b.tag == V_INT || b.tag == V_FLOAT))
            return num_equal(a, b);
        return false;
    }
    switch (a.tag) {
    case V_STRING:
    case V_SYMBOL:
    case V_KEYWORD:
        return a.as.str.len == b.as.str.len &&
               memcmp(a.as.str.data, b.as.str.data, a.as.str.len) == 0;
    case V_TUPLE:
    case V_ARRAY: {
        SeqObj *x = a.as.seq, *y = b.as.seq;
        if (x->len != y->len) return false;
        if (a.tag == V_TUPLE && x->paren != y->paren) return false; /* [T-014] */
        for (size_t i = 0; i < x->len; i++)
            if (!eq_depth(L, seq_at(a, i), seq_at(b, i), depth + 1)) return false;
        return true;
    }
    case V_STRUCT:
    case V_TABLE: {
        MapObj *x = a.as.map, *y = b.as.map;
        if (x->n != y->n) return false;
        for (size_t i = 0; i < x->n; i++) {
            bool found = false;
            for (size_t j = 0; j < y->n; j++) {
                if (eq_depth(L, x->entries[i].key, y->entries[j].key, depth + 1)) {
                    found = eq_depth(L, x->entries[i].val, y->entries[j].val,
                                     depth + 1);
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }
    case V_FN:
        return a.as.fn == b.as.fn;
    default:
        return eq_inline(a, b, depth);
    }
}

bool value_equal(Lang *L, Value a, Value b) { return eq_depth(L, a, b, 0); }

/* ------------------------------------------------------------------ */
/* Printing                                                           */
/* ------------------------------------------------------------------ */

typedef struct StrBuf {
    char *data;
    size_t len, cap;
} StrBuf;

static void sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < sb->len + extra + 1) cap *= 2;
    char *p = realloc(sb->data, cap);
    if (!p) {
        fprintf(stderr, "lang: out of memory\n");
        exit(70);
    }
    sb->data = p;
    sb->cap = cap;
}

static void sb_add(StrBuf *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    if (n) memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_adds(StrBuf *sb, const char *s) { sb_add(sb, s, strlen(s)); }

static void sb_addc(StrBuf *sb, char c) { sb_add(sb, &c, 1); }

static void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[128];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        sb_add(sb, tmp, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big) {
        fprintf(stderr, "lang: out of memory\n");
        exit(70);
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_add(sb, big, (size_t)n);
    free(big);
}

static void format_float(double d, char *out, size_t outsz) {
    if (isnan(d)) {
        snprintf(out, outsz, "nan");
        return;
    }
    if (isinf(d)) {
        snprintf(out, outsz, d < 0 ? "-inf" : "inf");
        return;
    }
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    if (!strpbrk(buf, ".eE")) {
        size_t n = strlen(buf);
        if (n + 3 < sizeof buf) snprintf(buf + n, sizeof buf - n, ".0");
    }
    snprintf(out, outsz, "%s", buf);
}

static void print_string(StrBuf *sb, Value v) {
    static const char hex[] = "0123456789abcdef";
    sb_addc(sb, '"');
    for (size_t i = 0; i < v.as.str.len; i++) {
        unsigned char c = (unsigned char)v.as.str.data[i];
        switch (c) {
        case '"': sb_adds(sb, "\\\""); break;
        case '\\': sb_adds(sb, "\\\\"); break;
        case '\n': sb_adds(sb, "\\n"); break;
        case '\t': sb_adds(sb, "\\t"); break;
        case '\r': sb_adds(sb, "\\r"); break;
        default:
            if (c < 0x20) {
                sb_adds(sb, "\\x");
                sb_addc(sb, hex[c >> 4]);
                sb_addc(sb, hex[c & 0xf]);
            } else {
                sb_addc(sb, (char)c);
            }
        }
    }
    sb_addc(sb, '"');
}

static void print_value(Lang *L, StrBuf *sb, Value v, int depth) {
    (void)L;
    if (depth > 64) {
        sb_adds(sb, "...");
        return;
    }
    switch (v.tag) {
    case V_NIL:
        sb_adds(sb, "nil");
        break;
    case V_BOOL:
        sb_adds(sb, v.as.b ? "true" : "false");
        break;
    case V_INT:
        sb_printf(sb, "%" PRId64, v.as.i);
        break;
    case V_FLOAT: {
        char buf[80];
        format_float(v.as.f, buf, sizeof buf);
        sb_adds(sb, buf);
        break;
    }
    case V_STRING:
        print_string(sb, v);
        break;
    case V_SYMBOL:
        sb_add(sb, v.as.str.data, v.as.str.len);
        break;
    case V_KEYWORD:
        sb_addc(sb, ':');
        sb_add(sb, v.as.str.data, v.as.str.len);
        break;
    case V_TUPLE:
    case V_ARRAY: {
        SeqObj *s = v.as.seq;
        if (v.tag == V_ARRAY) sb_adds(sb, "@[");
        else sb_addc(sb, s->paren ? '(' : '[');
        for (size_t i = 0; i < s->len; i++) {
            if (i) sb_addc(sb, ' ');
            print_value(L, sb, seq_at(v, i), depth + 1);
        }
        sb_addc(sb, v.tag == V_ARRAY ? ']' : (s->paren ? ')' : ']'));
        break;
    }
    case V_STRUCT:
    case V_TABLE: {
        MapObj *m = v.as.map;
        if (v.tag == V_TABLE) sb_adds(sb, "@{");
        else sb_addc(sb, '{');
        for (size_t i = 0; i < m->n; i++) {
            if (i) sb_addc(sb, ' ');
            print_value(L, sb, m->entries[i].key, depth + 1);
            sb_addc(sb, ' ');
            print_value(L, sb, m->entries[i].val, depth + 1);
        }
        sb_addc(sb, '}');
        break;
    }
    case V_FN: {
        FnObj *f = v.as.fn;
        if (f->is_macro) sb_adds(sb, "<macro");
        else sb_adds(sb, "<function");
        if (f->name && f->name[0]) {
            sb_addc(sb, ' ');
            sb_adds(sb, f->name);
        }
        sb_addc(sb, '>');
        break;
    }
    case V_RECUR:
        sb_adds(sb, "<recur>");
        break;
    }
}

char *value_repr(Lang *L, Value v) {
    StrBuf sb;
    memset(&sb, 0, sizeof sb);
    print_value(L, &sb, v, 0);
    if (!sb.data) sb_adds(&sb, "");
    return sb.data;
}

void value_repr_into(Lang *L, Value v, char *buf, size_t n) {
    char *repr = value_repr(L, v);
    snprintf(buf, n, "%s", repr);
    free(repr);
}
