/*
 * prims.c - the core primitives of spec §10.
 *
 * MIT License. See LICENSE.
 */
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void check_arity(Lang *L, const char *who, int argc, int min, int max) {
    if (argc < min || (max >= 0 && argc > max)) {
        if (max == min)
            lang_error(L, "%s expects %d argument(s), got %d", who, min, argc);
        else if (max < 0)
            lang_error(L, "%s expects at least %d argument(s), got %d", who,
                       min, argc);
        else
            lang_error(L, "%s expects %d to %d argument(s), got %d", who, min,
                       max, argc);
    }
}

static bool is_num(Value v) { return v.tag == V_INT || v.tag == V_FLOAT; }

static double num_d(Value v) {
    return v.tag == V_INT ? (double)v.as.i : v.as.f;
}

static void need_num(Lang *L, const char *who, Value v) {
    if (!is_num(v)) {
        char buf[128];
        value_repr_into(L, v, buf, sizeof buf);
        lang_error(L, "%s expects numbers, got %s", who, buf);
    }
}

static void need_seq(Lang *L, const char *who, Value v) {
    if (!is_seq(v)) {
        char buf[128];
        value_repr_into(L, v, buf, sizeof buf);
        lang_error(L, "%s expects a sequence, got %s", who, buf);
    }
}

static void need_array(Lang *L, const char *who, Value v) {
    if (v.tag != V_ARRAY) {
        char buf[128];
        value_repr_into(L, v, buf, sizeof buf);
        lang_error(L, "%s expects a mutable array, got %s", who, buf);
    }
}

static void need_map(Lang *L, const char *who, Value v) {
    if (!is_map(v)) {
        char buf[128];
        value_repr_into(L, v, buf, sizeof buf);
        lang_error(L, "%s expects a struct or table, got %s", who, buf);
    }
}

static void need_table(Lang *L, const char *who, Value v) {
    if (v.tag != V_TABLE) {
        char buf[128];
        value_repr_into(L, v, buf, sizeof buf);
        lang_error(L, "%s expects a mutable table, got %s", who, buf);
    }
}

static int64_t want_index(Lang *L, const char *who, Value v) {
    if (v.tag == V_INT) return v.as.i;
    if (v.tag == V_FLOAT && v.as.f == floor(v.as.f) && fabs(v.as.f) < 9.2e18)
        return (int64_t)v.as.f;
    lang_error(L, "%s expects an integer index", who);
    return 0;
}

static size_t clamped_index(int64_t i, size_t len) {
    if (i <= 0) return 0;
    if ((uint64_t)i >= len) return len;
    return (size_t)i;
}

static Value array_from(Lang *L, Value *elems, size_t n) {
    return v_seq_from(L, elems, n, true, false);
}

static void reg(Lang *L, Env *env, const char *name, NativeFn fn) {
    Value sym = v_symbol(L, name, strlen(name));
    env_define(L, env, sym.as.str.data, v_fn_native(L, name, fn), false, false);
}

/* ------------------------------------------------------------------ */
/* Arithmetic (§10.1)                                                 */
/* ------------------------------------------------------------------ */

static Value prim_add(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    bool all_int = true;
    for (int i = 0; i < argc; i++) {
        need_num(L, "+", argv[i]);
        if (argv[i].tag != V_INT) all_int = false;
    }
    if (all_int) {
        int64_t acc = 0;
        for (int i = 0; i < argc; i++) {
            int64_t r;
            if (__builtin_add_overflow(acc, argv[i].as.i, &r))
                lang_error(L, "+: integer overflow");
            acc = r;
        }
        return v_int(acc);
    }
    double acc = 0;
    for (int i = 0; i < argc; i++) acc += num_d(argv[i]);
    return v_float(acc);
}

static Value prim_sub(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "-", argc, 1, -1);
    bool all_int = true;
    for (int i = 0; i < argc; i++) {
        need_num(L, "-", argv[i]);
        if (argv[i].tag != V_INT) all_int = false;
    }
    if (all_int) {
        int64_t acc = argv[0].as.i;
        if (argc == 1) {
            int64_t r;
            if (__builtin_sub_overflow((int64_t)0, acc, &r))
                lang_error(L, "-: integer overflow");
            return v_int(r);
        }
        for (int i = 1; i < argc; i++) {
            int64_t r;
            if (__builtin_sub_overflow(acc, argv[i].as.i, &r))
                lang_error(L, "-: integer overflow");
            acc = r;
        }
        return v_int(acc);
    }
    double acc = num_d(argv[0]);
    if (argc == 1) return v_float(-acc);
    for (int i = 1; i < argc; i++) acc -= num_d(argv[i]);
    return v_float(acc);
}

static Value prim_mul(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    bool all_int = true;
    for (int i = 0; i < argc; i++) {
        need_num(L, "*", argv[i]);
        if (argv[i].tag != V_INT) all_int = false;
    }
    if (all_int) {
        int64_t acc = 1;
        for (int i = 0; i < argc; i++) {
            int64_t r;
            if (__builtin_mul_overflow(acc, argv[i].as.i, &r))
                lang_error(L, "*: integer overflow");
            acc = r;
        }
        return v_int(acc);
    }
    double acc = 1;
    for (int i = 0; i < argc; i++) acc *= num_d(argv[i]);
    return v_float(acc);
}

static Value prim_div(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "/", argc, 1, -1);
    bool all_int = true;
    for (int i = 0; i < argc; i++) {
        need_num(L, "/", argv[i]);
        if (argv[i].tag != V_INT) all_int = false;
    }
    if (argc == 1) {
        double x = num_d(argv[0]);
        if (x == 0) lang_error(L, "/: division by zero");
        return v_float(1.0 / x);
    }
    if (all_int) {
        int64_t acc = argv[0].as.i;
        for (int i = 1; i < argc; i++) {
            int64_t d = argv[i].as.i;
            if (d == 0) lang_error(L, "/: division by zero");
            if (acc == INT64_MIN && d == -1)
                lang_error(L, "/: integer overflow");
            acc /= d;
        }
        return v_int(acc);
    }
    double acc = num_d(argv[0]);
    for (int i = 1; i < argc; i++) acc /= num_d(argv[i]);
    return v_float(acc);
}

static Value prim_mod(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "mod", argc, 2, 2);
    need_num(L, "mod", argv[0]);
    need_num(L, "mod", argv[1]);
    if (argv[0].tag == V_INT && argv[1].tag == V_INT) {
        int64_t a = argv[0].as.i, b = argv[1].as.i;
        if (b == 0) lang_error(L, "mod: division by zero");
        int64_t r = a % b;
        if (r != 0 && ((r < 0) != (b < 0))) r += b;
        return v_int(r);
    }
    double a = num_d(argv[0]), b = num_d(argv[1]);
    if (b == 0) lang_error(L, "mod: division by zero");
    double r = fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return v_float(r);
}

static Value prim_rem(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "rem", argc, 2, 2);
    need_num(L, "rem", argv[0]);
    need_num(L, "rem", argv[1]);
    if (argv[0].tag == V_INT && argv[1].tag == V_INT) {
        if (argv[1].as.i == 0) lang_error(L, "rem: division by zero");
        return v_int(argv[0].as.i % argv[1].as.i);
    }
    double b = num_d(argv[1]);
    if (b == 0) lang_error(L, "rem: division by zero");
    return v_float(fmod(num_d(argv[0]), b));
}

static Value prim_inc(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "inc", argc, 1, 1);
    need_num(L, "inc", argv[0]);
    if (argv[0].tag == V_INT) {
        int64_t r;
        if (__builtin_add_overflow(argv[0].as.i, (int64_t)1, &r))
            lang_error(L, "inc: integer overflow");
        return v_int(r);
    }
    return v_float(argv[0].as.f + 1.0);
}

static Value prim_dec(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "dec", argc, 1, 1);
    need_num(L, "dec", argv[0]);
    if (argv[0].tag == V_INT) {
        int64_t r;
        if (__builtin_sub_overflow(argv[0].as.i, (int64_t)1, &r))
            lang_error(L, "dec: integer overflow");
        return v_int(r);
    }
    return v_float(argv[0].as.f - 1.0);
}

static Value prim_abs(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "abs", argc, 1, 1);
    need_num(L, "abs", argv[0]);
    if (argv[0].tag == V_INT) {
        if (argv[0].as.i == INT64_MIN) lang_error(L, "abs: integer overflow");
        return v_int(argv[0].as.i < 0 ? -argv[0].as.i : argv[0].as.i);
    }
    return v_float(fabs(argv[0].as.f));
}

static Value prim_min(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "min", argc, 1, -1);
    int best = 0;
    need_num(L, "min", argv[0]);
    for (int i = 1; i < argc; i++) {
        need_num(L, "min", argv[i]);
        if (num_d(argv[i]) < num_d(argv[best])) best = i;
    }
    return argv[best];
}

static Value prim_max(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "max", argc, 1, -1);
    int best = 0;
    need_num(L, "max", argv[0]);
    for (int i = 1; i < argc; i++) {
        need_num(L, "max", argv[i]);
        if (num_d(argv[i]) > num_d(argv[best])) best = i;
    }
    return argv[best];
}

/* ------------------------------------------------------------------ */
/* Comparison (§10.2) and boolean logic (§10.3)                       */
/* ------------------------------------------------------------------ */

static Value prim_eq(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    for (int i = 1; i < argc; i++)
        if (!value_equal(L, argv[i - 1], argv[i])) return v_bool(false);
    return v_bool(true);
}

static Value prim_neq(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    for (int i = 1; i < argc; i++)
        if (!value_equal(L, argv[i - 1], argv[i])) return v_bool(true);
    return v_bool(false);
}

typedef enum { CMP_LT, CMP_LE, CMP_GT, CMP_GE } CmpOp;

static Value compare_chain(Lang *L, const char *who, int argc, Value *argv,
                           CmpOp op) {
    for (int i = 0; i < argc; i++) need_num(L, who, argv[i]);
    for (int i = 1; i < argc; i++) {
        double a = num_d(argv[i - 1]), b = num_d(argv[i]);
        bool ok = false;
        switch (op) {
        case CMP_LT: ok = a < b; break;
        case CMP_LE: ok = a <= b; break;
        case CMP_GT: ok = a > b; break;
        case CMP_GE: ok = a >= b; break;
        }
        if (!ok) return v_bool(false);
    }
    return v_bool(true);
}

static Value prim_lt(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    return compare_chain(L, "<", argc, argv, CMP_LT);
}
static Value prim_le(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    return compare_chain(L, "<=", argc, argv, CMP_LE);
}
static Value prim_gt(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    return compare_chain(L, ">", argc, argv, CMP_GT);
}
static Value prim_ge(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    return compare_chain(L, ">=", argc, argv, CMP_GE);
}

static Value prim_not(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "not", argc, 1, 1);
    return v_bool(!truthy(argv[0]));
}

/* ------------------------------------------------------------------ */
/* Sequence access (§10.4)                                            */
/* ------------------------------------------------------------------ */

static Value prim_length(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "length", argc, 1, 1);
    if (argv[0].tag == V_NIL) return v_int(0);
    need_seq(L, "length", argv[0]);
    return v_int((int64_t)seq_len(argv[0]));
}

static Value prim_empty(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "empty?", argc, 1, 1);
    if (argv[0].tag == V_NIL) return v_bool(true);
    if (is_seq(argv[0])) return v_bool(seq_len(argv[0]) == 0);
    return v_bool(false);
}

static Value prim_first(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "first", argc, 1, 1);
    if (argv[0].tag == V_NIL) return v_nil();
    need_seq(L, "first", argv[0]);
    if (seq_len(argv[0]) == 0) lang_error(L, "first: empty sequence");
    return seq_at(argv[0], 0);
}

static Value prim_next(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "next", argc, 1, 1);
    if (argv[0].tag == V_NIL) return v_nil();
    need_seq(L, "next", argv[0]);
    SeqObj *s = argv[0].as.seq;
    if (s->len == 0) lang_error(L, "next: empty sequence");
    return v_seq_view(L, argv[0], s->start + 1, s->len - 1);
}

static Value prim_get(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "get", argc, 2, 2);
    Value c = argv[0], k = argv[1];
    if (c.tag == V_NIL) return v_nil();
    if (is_map(c)) {
        Value out;
        if (map_lookup(c, k, &out)) return out;
        return v_nil();
    }
    need_seq(L, "get", c);
    int64_t i = want_index(L, "get", k);
    if (i < 0 || (uint64_t)i >= seq_len(c)) return v_nil();
    return seq_at(c, (size_t)i);
}

static Value prim_drop(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "drop", argc, 2, 2);
    Value coll = argv[1];
    if (coll.tag == V_NIL) return v_nil();
    need_seq(L, "drop", coll);
    int64_t n = want_index(L, "drop", argv[0]);
    SeqObj *s = coll.as.seq;
    if (n <= 0) return coll;
    if ((uint64_t)n >= s->len) return v_seq_view(L, coll, s->start + s->len, 0);
    return v_seq_view(L, coll, s->start + (size_t)n, s->len - (size_t)n);
}

static Value prim_slice(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "slice", argc, 3, 3);
    Value coll = argv[2];
    if (coll.tag == V_NIL) return v_nil();
    need_seq(L, "slice", coll);
    int64_t n = want_index(L, "slice", argv[0]);
    int64_t m = want_index(L, "slice", argv[1]);
    SeqObj *s = coll.as.seq;
    size_t len = s->len;
    size_t lo = clamped_index(n, len);
    size_t hi = clamped_index(m, len);
    if (lo >= hi) return v_seq_view(L, coll, s->start + lo, 0);
    return v_seq_view(L, coll, s->start + lo, hi - lo);
}

/* ------------------------------------------------------------------ */
/* Type predicates (§10.5)                                            */
/* ------------------------------------------------------------------ */

static Value prim_nilp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "nil?", argc, 1, 1);
    return v_bool(argv[0].tag == V_NIL);
}
static Value prim_booleanp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "boolean?", argc, 1, 1);
    return v_bool(argv[0].tag == V_BOOL);
}
static Value prim_numberp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "number?", argc, 1, 1);
    return v_bool(is_num(argv[0]));
}
static Value prim_stringp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "string?", argc, 1, 1);
    return v_bool(argv[0].tag == V_STRING);
}
static Value prim_symbolp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "symbol?", argc, 1, 1);
    return v_bool(argv[0].tag == V_SYMBOL);
}
static Value prim_keywordp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "keyword?", argc, 1, 1);
    return v_bool(argv[0].tag == V_KEYWORD);
}
static Value prim_tuplep(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "tuple?", argc, 1, 1);
    return v_bool(argv[0].tag == V_TUPLE);
}
static Value prim_arrayp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "array?", argc, 1, 1);
    return v_bool(argv[0].tag == V_ARRAY);
}
static Value prim_parenp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "paren?", argc, 1, 1);
    return v_bool(argv[0].tag == V_TUPLE && argv[0].as.seq->paren);
}
static Value prim_bracketp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "bracket?", argc, 1, 1);
    return v_bool(argv[0].tag == V_TUPLE && !argv[0].as.seq->paren);
}
static Value prim_structp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "struct?", argc, 1, 1);
    return v_bool(argv[0].tag == V_STRUCT);
}
static Value prim_tablep(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "table?", argc, 1, 1);
    return v_bool(argv[0].tag == V_TABLE);
}
static Value prim_fnp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "fn?", argc, 1, 1);
    return v_bool(argv[0].tag == V_FN);
}

static Value prim_type(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "type", argc, 1, 1);
    switch (argv[0].tag) {
    case V_NIL: return L->k_nil;
    case V_BOOL: return L->k_boolean;
    case V_INT:
    case V_FLOAT: return L->k_number;
    case V_STRING: return L->k_string;
    case V_SYMBOL: return L->k_symbol;
    case V_KEYWORD: return L->k_keyword;
    case V_TUPLE: return L->k_tuple;
    case V_ARRAY: return L->k_array;
    case V_STRUCT: return L->k_struct;
    case V_TABLE: return L->k_table;
    case V_FN: return L->k_function;
    default: return L->k_nil;
    }
}

/* ------------------------------------------------------------------ */
/* Mutators (§10.6)                                                   */
/* ------------------------------------------------------------------ */

static void buf_grow(Lang *L, Buf *b, size_t need) {
    if (b->alloc >= need) return;
    size_t alloc = b->alloc ? b->alloc : 4;
    while (alloc < need) alloc *= 2;
    Value *slots = lang_alloc(L, alloc * sizeof(Value));
    if (b->cap) memcpy(slots, b->slots, b->cap * sizeof(Value));
    b->slots = slots;
    b->alloc = alloc;
}

static Value prim_push(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "push!", argc, 2, 2);
    need_array(L, "push!", argv[0]);
    SeqObj *s = argv[0].as.seq;
    if (s->start + s->len != s->buf->cap)
        lang_error(L, "push!: array does not extend to the end of its buffer");
    buf_grow(L, s->buf, s->buf->cap + 1);
    s->buf->slots[s->start + s->len] = argv[1];
    s->buf->cap++;
    s->len++;
    return argv[0];
}

static Value prim_pop(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "pop!", argc, 1, 1);
    need_array(L, "pop!", argv[0]);
    SeqObj *s = argv[0].as.seq;
    if (s->len == 0) return v_nil();
    Value v = seq_at(argv[0], s->len - 1);
    s->len--;
    return v;
}

static Value prim_peek(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "peek", argc, 1, 1);
    need_array(L, "peek", argv[0]);
    if (seq_len(argv[0]) == 0) return v_nil();
    return seq_at(argv[0], seq_len(argv[0]) - 1);
}

/* ---- sorting ---- */

typedef struct LessCtx {
    Lang *L;
    Value fn;
    bool has_fn;
} LessCtx;

static bool default_less(Lang *L, Value a, Value b) {
    if (a.tag == V_INT && b.tag == V_INT) return a.as.i < b.as.i;
    if (is_num(a) && is_num(b)) return num_d(a) < num_d(b);
    char buf[128];
    value_repr_into(L, a, buf, sizeof buf);
    lang_error(L, "sort: default comparator requires numbers, got %s", buf);
    return false;
}

static bool less_than(LessCtx *ctx, Value a, Value b) {
    if (ctx->has_fn) {
        Value args[2] = {a, b};
        return truthy(lang_apply(ctx->L, ctx->fn, 2, args));
    }
    return default_less(ctx->L, a, b);
}

static void merge_sort(LessCtx *ctx, Value *a, Value *tmp, size_t n) {
    if (n < 2) return;
    size_t mid = n / 2;
    merge_sort(ctx, a, tmp, mid);
    merge_sort(ctx, a + mid, tmp, n - mid);
    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        if (less_than(ctx, a[j], a[i]))
            tmp[k++] = a[j++];
        else
            tmp[k++] = a[i++];
    }
    while (i < mid) tmp[k++] = a[i++];
    while (j < n) tmp[k++] = a[j++];
    memcpy(a, tmp, n * sizeof(Value));
}

static LessCtx make_less(Lang *L, int argc, Value *argv, int cmp_index,
                         const char *who) {
    LessCtx ctx;
    ctx.L = L;
    ctx.has_fn = argc > cmp_index;
    ctx.fn = ctx.has_fn ? argv[cmp_index] : v_nil();
    (void)who;
    return ctx;
}

static Value prim_sort_bang(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "sort!", argc, 1, 2);
    need_array(L, "sort!", argv[0]);
    SeqObj *s = argv[0].as.seq;
    if (s->len < 2) return argv[0];
    LessCtx ctx = make_less(L, argc, argv, 1, "sort!");
    Value *work = lang_alloc(L, s->len * sizeof(Value));
    Value *tmp = lang_alloc(L, s->len * sizeof(Value));
    for (size_t i = 0; i < s->len; i++) work[i] = seq_at(argv[0], i);
    merge_sort(&ctx, work, tmp, s->len);
    for (size_t i = 0; i < s->len; i++) seq_set_at(argv[0], i, work[i]);
    return argv[0];
}

static Value prim_reverse_bang(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "reverse!", argc, 1, 1);
    need_array(L, "reverse!", argv[0]);
    size_t n = seq_len(argv[0]);
    for (size_t i = 0; i < n / 2; i++) {
        Value a = seq_at(argv[0], i);
        Value b = seq_at(argv[0], n - 1 - i);
        seq_set_at(argv[0], i, b);
        seq_set_at(argv[0], n - 1 - i, a);
    }
    return argv[0];
}

static uint64_t rng_next(Lang *L) {
    uint64_t x = L->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    L->rng = x;
    return x * 2685821657736338717ull;
}

static Value prim_shuffle_bang(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "shuffle!", argc, 1, 1);
    need_array(L, "shuffle!", argv[0]);
    size_t n = seq_len(argv[0]);
    for (size_t i = n; i > 1; i--) {
        size_t j = (size_t)(rng_next(L) % i);
        Value a = seq_at(argv[0], i - 1);
        Value b = seq_at(argv[0], j);
        seq_set_at(argv[0], i - 1, b);
        seq_set_at(argv[0], j, a);
    }
    return argv[0];
}

/* ---- tables ---- */

static Value prim_put(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "put!", argc, 3, 3);
    need_table(L, "put!", argv[0]);
    map_put(L, argv[0], argv[1], argv[2]);
    return argv[0];
}

static Value prim_del(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "del!", argc, 2, 2);
    need_table(L, "del!", argv[0]);
    map_del(L, argv[0], argv[1]);
    return argv[0];
}

static Value prim_has(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "has?", argc, 2, 2);
    need_map(L, "has?", argv[0]);
    Value out;
    return v_bool(map_lookup(argv[0], argv[1], &out));
}

static Value prim_keys(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "keys", argc, 1, 1);
    need_map(L, "keys", argv[0]);
    MapObj *m = argv[0].as.map;
    Value *elems = m->n ? lang_alloc(L, m->n * sizeof(Value)) : NULL;
    for (size_t i = 0; i < m->n; i++) elems[i] = m->entries[i].key;
    return array_from(L, elems, m->n);
}

static Value prim_values(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "values", argc, 1, 1);
    need_map(L, "values", argv[0]);
    MapObj *m = argv[0].as.map;
    Value *elems = m->n ? lang_alloc(L, m->n * sizeof(Value)) : NULL;
    for (size_t i = 0; i < m->n; i++) elems[i] = m->entries[i].val;
    return array_from(L, elems, m->n);
}

/* ------------------------------------------------------------------ */
/* Builders (§10.7)                                                   */
/* ------------------------------------------------------------------ */

/* Length of a builder input; nil is treated as an empty sequence. */
static size_t coll_len(Lang *L, const char *who, Value v) {
    if (v.tag == V_NIL) return 0;
    need_seq(L, who, v);
    return seq_len(v);
}

static Value prim_map(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "map", argc, 2, 2);
    size_t n = coll_len(L, "map", argv[1]);
    Value *elems = n ? lang_alloc(L, n * sizeof(Value)) : NULL;
    for (size_t i = 0; i < n; i++) {
        Value x = seq_at(argv[1], i);
        elems[i] = lang_apply(L, argv[0], 1, &x);
    }
    return array_from(L, elems, n);
}

static Value prim_filter(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "filter", argc, 2, 2);
    size_t n = coll_len(L, "filter", argv[1]);
    Value *elems = n ? lang_alloc(L, n * sizeof(Value)) : NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        Value x = seq_at(argv[1], i);
        Value r = lang_apply(L, argv[0], 1, &x);
        if (truthy(r)) elems[k++] = x;
    }
    return array_from(L, elems, k);
}

static Value prim_reduce(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "reduce", argc, 3, 3);
    size_t n = coll_len(L, "reduce", argv[2]);
    Value acc = argv[1];
    for (size_t i = 0; i < n; i++) {
        Value args[2] = {acc, seq_at(argv[2], i)};
        acc = lang_apply(L, argv[0], 2, args);
    }
    return acc;
}

static Value prim_each(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "each", argc, 2, 2);
    size_t n = coll_len(L, "each", argv[1]);
    for (size_t i = 0; i < n; i++) {
        Value x = seq_at(argv[1], i);
        (void)lang_apply(L, argv[0], 1, &x);
    }
    return v_nil();
}

static Value prim_some(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "some", argc, 2, 2);
    size_t n = coll_len(L, "some", argv[1]);
    for (size_t i = 0; i < n; i++) {
        Value x = seq_at(argv[1], i);
        Value r = lang_apply(L, argv[0], 1, &x);
        if (truthy(r)) return r;
    }
    return v_nil();
}

static Value prim_every(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "every?", argc, 2, 2);
    size_t n = coll_len(L, "every?", argv[1]);
    for (size_t i = 0; i < n; i++) {
        Value x = seq_at(argv[1], i);
        if (!truthy(lang_apply(L, argv[0], 1, &x))) return v_bool(false);
    }
    return v_bool(true);
}

static Value prim_find(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "find", argc, 2, 2);
    size_t n = coll_len(L, "find", argv[1]);
    for (size_t i = 0; i < n; i++) {
        Value x = seq_at(argv[1], i);
        if (truthy(lang_apply(L, argv[0], 1, &x))) return x;
    }
    return v_nil();
}

static Value prim_take(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "take", argc, 2, 2);
    int64_t n = want_index(L, "take", argv[0]);
    size_t len = coll_len(L, "take", argv[1]);
    size_t k = n <= 0 ? 0 : clamped_index(n, len);
    Value *elems = k ? lang_alloc(L, k * sizeof(Value)) : NULL;
    for (size_t i = 0; i < k; i++) elems[i] = seq_at(argv[1], i);
    return array_from(L, elems, k);
}

static Value prim_concat(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    size_t total = 0;
    for (int i = 0; i < argc; i++) total += coll_len(L, "concat", argv[i]);
    Value *elems = total ? lang_alloc(L, total * sizeof(Value)) : NULL;
    size_t k = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = coll_len(L, "concat", argv[i]);
        for (size_t j = 0; j < n; j++) elems[k++] = seq_at(argv[i], j);
    }
    return array_from(L, elems, k);
}

static Value prim_reverse(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "reverse", argc, 1, 1);
    size_t n = coll_len(L, "reverse", argv[0]);
    Value *elems = n ? lang_alloc(L, n * sizeof(Value)) : NULL;
    for (size_t i = 0; i < n; i++) elems[i] = seq_at(argv[0], n - 1 - i);
    return array_from(L, elems, n);
}

static Value prim_sort(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "sort", argc, 1, 2);
    size_t n = coll_len(L, "sort", argv[0]);
    Value *elems = n ? lang_alloc(L, n * sizeof(Value)) : NULL;
    for (size_t i = 0; i < n; i++) elems[i] = seq_at(argv[0], i);
    if (n >= 2) {
        LessCtx ctx = make_less(L, argc, argv, 1, "sort");
        Value *tmp = lang_alloc(L, n * sizeof(Value));
        merge_sort(&ctx, elems, tmp, n);
    }
    return array_from(L, elems, n);
}

static Value prim_zip(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    size_t shortest = 0;
    if (argc > 0) {
        shortest = (size_t)-1;
        for (int i = 0; i < argc; i++) {
            size_t n = coll_len(L, "zip", argv[i]);
            if (n < shortest) shortest = n;
        }
    }
    size_t rowsz = (size_t)argc;
    Value *elems = shortest ? lang_alloc(L, shortest * sizeof(Value)) : NULL;
    for (size_t r = 0; r < shortest; r++) {
        Value *row = rowsz ? lang_alloc(L, rowsz * sizeof(Value)) : NULL;
        for (int c = 0; c < argc; c++) row[c] = seq_at(argv[c], r);
        elems[r] = v_seq_from(L, row, rowsz, false, false);
    }
    return array_from(L, elems, shortest);
}

static Value prim_range(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "range", argc, 1, 3);
    need_num(L, "range", argv[0]);
    Value start = argc == 1 ? v_int(0) : argv[0];
    Value end = argc == 1 ? argv[0] : argv[1];
    Value step = argc >= 3 ? argv[2] : v_int(1);
    need_num(L, "range", end);
    need_num(L, "range", step);

    if (num_d(step) == 0) lang_error(L, "range: step must not be zero");
    bool all_int = start.tag == V_INT && end.tag == V_INT && step.tag == V_INT;
    double ds = num_d(step);
    double span = num_d(end) - num_d(start);
    double countd = span / ds;
    int64_t count = (int64_t)ceil(countd);
    if (count < 0) count = 0;
    if (count > 100000000)
        lang_error(L, "range: refusing to build %lld elements",
                   (long long)count);
    size_t n = (size_t)count;
    Value *elems = n ? lang_alloc(L, n * sizeof(Value)) : NULL;
    if (all_int) {
        int64_t s = start.as.i, st = step.as.i;
        for (size_t i = 0; i < n; i++) elems[i] = v_int(s + (int64_t)i * st);
    } else {
        double s = num_d(start);
        for (size_t i = 0; i < n; i++) elems[i] = v_float(s + (double)i * ds);
    }
    return array_from(L, elems, n);
}

static Value prim_array(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "array", argc, 0, 1);
    if (argc == 0) return array_from(L, NULL, 0);
    if (argv[0].tag == V_ARRAY) return argv[0];
    size_t n = coll_len(L, "array", argv[0]);
    Value *elems = n ? lang_alloc(L, n * sizeof(Value)) : NULL;
    for (size_t i = 0; i < n; i++) elems[i] = seq_at(argv[0], i);
    return array_from(L, elems, n);
}

static Value prim_tuple(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "tuple", argc, 0, 1);
    if (argc == 0) return v_seq_from(L, NULL, 0, false, false);
    if (argv[0].tag == V_TUPLE) return argv[0];
    size_t n = coll_len(L, "tuple", argv[0]);
    Value *elems = n ? lang_alloc(L, n * sizeof(Value)) : NULL;
    for (size_t i = 0; i < n; i++) elems[i] = seq_at(argv[0], i);
    return v_seq_from(L, elems, n, false, false);
}

/* ------------------------------------------------------------------ */
/* Standard library (§10.8)                                           */
/* ------------------------------------------------------------------ */

static Value prim_apply(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "apply", argc, 2, 2);
    size_t n = coll_len(L, "apply", argv[1]);
    Value *args = n ? lang_alloc(L, n * sizeof(Value)) : NULL;
    for (size_t i = 0; i < n; i++) args[i] = seq_at(argv[1], i);
    return lang_apply(L, argv[0], (int)n, args);
}

static Value prim_identity(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "identity", argc, 1, 1);
    (void)L;
    return argv[0];
}

static Value prim_complement_call(Lang *L, FnObj *self, int argc,
                                  Value *argv) {
    Value r = lang_apply(L, self->captured[0], argc, argv);
    return v_bool(!truthy(r));
}

static Value prim_complement(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "complement", argc, 1, 1);
    return v_fn_native_closure(L, "complement", prim_complement_call, 1, argv);
}

static Value prim_comp_call(Lang *L, FnObj *self, int argc, Value *argv) {
    int n = self->ncaptured;
    if (n == 0) {
        check_arity(L, "comp", argc, 1, 1);
        return argv[0];
    }
    Value acc = lang_apply(L, self->captured[n - 1], argc, argv);
    for (int i = n - 2; i >= 0; i--) acc = lang_apply(L, self->captured[i], 1, &acc);
    return acc;
}

static Value prim_comp(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    return v_fn_native_closure(L, "comp", prim_comp_call, argc, argv);
}

static Value prim_const_call(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)L;
    (void)argc;
    (void)argv;
    return self->captured[0];
}

static Value prim_const(Lang *L, FnObj *self, int argc, Value *argv) {
    (void)self;
    check_arity(L, "const", argc, 1, 1);
    return v_fn_native_closure(L, "const", prim_const_call, 1, argv);
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

void prims_install(Lang *L, Env *env) {
    /* §10.1 */
    reg(L, env, "+", prim_add);
    reg(L, env, "-", prim_sub);
    reg(L, env, "*", prim_mul);
    reg(L, env, "/", prim_div);
    reg(L, env, "mod", prim_mod);
    reg(L, env, "rem", prim_rem);
    reg(L, env, "inc", prim_inc);
    reg(L, env, "dec", prim_dec);
    reg(L, env, "abs", prim_abs);
    reg(L, env, "min", prim_min);
    reg(L, env, "max", prim_max);
    /* §10.2 */
    reg(L, env, "=", prim_eq);
    reg(L, env, "not=", prim_neq);
    reg(L, env, "<", prim_lt);
    reg(L, env, "<=", prim_le);
    reg(L, env, ">", prim_gt);
    reg(L, env, ">=", prim_ge);
    /* §10.3 */
    reg(L, env, "not", prim_not);
    /* §10.4 */
    reg(L, env, "length", prim_length);
    reg(L, env, "empty?", prim_empty);
    reg(L, env, "first", prim_first);
    reg(L, env, "next", prim_next);
    reg(L, env, "get", prim_get);
    reg(L, env, "drop", prim_drop);
    reg(L, env, "slice", prim_slice);
    /* §10.5 */
    reg(L, env, "nil?", prim_nilp);
    reg(L, env, "boolean?", prim_booleanp);
    reg(L, env, "number?", prim_numberp);
    reg(L, env, "string?", prim_stringp);
    reg(L, env, "symbol?", prim_symbolp);
    reg(L, env, "keyword?", prim_keywordp);
    reg(L, env, "tuple?", prim_tuplep);
    reg(L, env, "array?", prim_arrayp);
    reg(L, env, "paren?", prim_parenp);
    reg(L, env, "bracket?", prim_bracketp);
    reg(L, env, "struct?", prim_structp);
    reg(L, env, "table?", prim_tablep);
    reg(L, env, "fn?", prim_fnp);
    reg(L, env, "type", prim_type);
    /* §10.6 */
    reg(L, env, "push!", prim_push);
    reg(L, env, "pop!", prim_pop);
    reg(L, env, "peek", prim_peek);
    reg(L, env, "sort!", prim_sort_bang);
    reg(L, env, "reverse!", prim_reverse_bang);
    reg(L, env, "shuffle!", prim_shuffle_bang);
    reg(L, env, "put!", prim_put);
    reg(L, env, "del!", prim_del);
    reg(L, env, "has?", prim_has);
    reg(L, env, "keys", prim_keys);
    reg(L, env, "values", prim_values);
    /* §10.7 */
    reg(L, env, "map", prim_map);
    reg(L, env, "filter", prim_filter);
    reg(L, env, "reduce", prim_reduce);
    reg(L, env, "each", prim_each);
    reg(L, env, "some", prim_some);
    reg(L, env, "every?", prim_every);
    reg(L, env, "find", prim_find);
    reg(L, env, "take", prim_take);
    reg(L, env, "concat", prim_concat);
    reg(L, env, "reverse", prim_reverse);
    reg(L, env, "sort", prim_sort);
    reg(L, env, "zip", prim_zip);
    reg(L, env, "range", prim_range);
    reg(L, env, "array", prim_array);
    reg(L, env, "tuple", prim_tuple);
    /* §10.8 */
    reg(L, env, "apply", prim_apply);
    reg(L, env, "identity", prim_identity);
    reg(L, env, "complement", prim_complement);
    reg(L, env, "comp", prim_comp);
    reg(L, env, "const", prim_const);
}
