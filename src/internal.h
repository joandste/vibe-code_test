/*
 * internal.h - internal data model and interfaces.
 *
 * MIT License. See LICENSE.
 *
 * Value representation follows spec §5:
 *
 *   - Tuples and arrays share one layout: a buffer reference plus a start
 *     offset and a length ([T-003], [T-031]). Advancing through a sequence
 *     allocates only a small header that points into the same buffer, which is
 *     what makes `next`, `drop` and `slice` O(1) ([V-003], [V-004], [V-012]).
 *   - Tuples carry the shape flag (paren/bracket), arrays carry none
 *     ([T-006a], [T-006b]).
 *   - Structs and tables are associative collections with the same layout as
 *     each other ([T-019]) and are not sequences ([T-021]).
 *
 * Memory management uses an arena owned by the Lang handle: every allocation
 * is released by lang_free(). The spec leaves the strategy open ([T-035]).
 */
#ifndef LANG_INTERNAL_H
#define LANG_INTERNAL_H

#include "lang.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    V_NIL = 0,
    V_BOOL,
    V_INT,
    V_FLOAT,
    V_STRING,
    V_SYMBOL,
    V_KEYWORD,
    V_TUPLE,
    V_ARRAY,
    V_STRUCT,
    V_TABLE,
    V_FN,
    V_RECUR /* internal signal used by `recur` while unwinding to the loop */
} ValueTag;

typedef struct Value Value;
typedef struct SeqObj SeqObj;
typedef struct MapObj MapObj;
typedef struct FnObj FnObj;
typedef struct Buf Buf;
typedef struct Env Env;
typedef struct Binding Binding;

struct Value {
    uint8_t tag;
    union {
        bool b;
        int64_t i;
        double f;
        struct {
            const char *data; /* interned for symbols/keywords, NUL terminated */
            size_t len;
        } str;
        SeqObj *seq;
        MapObj *map;
        FnObj *fn;
    } as;
};

/* Shared storage for tuples and arrays ([T-003]).
 *
 * `cap` is the logical capacity: the length of the base sequence over this
 * buffer ([T-031]). `alloc` is the allocated size and may exceed `cap`; the
 * extra slots are not yet part of any live region. `push!` may only append at
 * `start + len == cap` ([C-040a]). */
struct Buf {
    Value *slots;
    size_t cap;
    size_t alloc;
};

struct SeqObj {
    Buf *buf;
    size_t start;
    size_t len;
    bool mutable_;
    bool paren; /* shape flag: paren (call form) vs bracket (literal data) */
    int line, col; /* source location attached by the reader ([SYN-006]) */
};

typedef struct Entry {
    Value key;
    Value val;
} Entry;

struct MapObj {
    Entry *entries;
    size_t n, cap;
    bool mutable_;
    int line, col;
};

typedef Value (*NativeFn)(Lang *L, FnObj *self, int argc, Value *argv);

struct FnObj {
    /* Native primitives use `native`; closures have native == NULL. */
    NativeFn native;
    bool is_macro;
    const char *name;
    /* Captured values for native closures (complement/comp/const). */
    Value *captured;
    int ncaptured;
    /* Closures. */
    Env *env;
    Value body; /* array of forms */
    int nfixed, nopt;
    bool has_rest;
    int nparams_total;
    Value *pnames; /* nparams_total interned parameter symbols */
};

struct Binding {
    const char *name; /* interned */
    Value value;
    bool mutable_;
    bool is_macro;
    bool defined; /* false while a `def` value is being evaluated */
    Binding *next;
};

struct Env {
    Env *parent;
    Binding *bindings;
};

/* ------------------------------------------------------------------ */
/* Interpreter                                                         */
/* ------------------------------------------------------------------ */

typedef union MemBlock {
    struct {
        union MemBlock *next;
        size_t size;
    } h;
    max_align_t align;
} MemBlock;

typedef struct InternEntry {
    const char *name;
    size_t len;
    bool keyword;
    uint32_t hash;
    Value value;
    struct InternEntry *next;
} InternEntry;

struct Lang {
    jmp_buf *err_jmp;
    char errmsg[768];
    MemBlock *blocks;
    Env *global; /* newest top-level frame */
    Env *core;   /* frame holding the primitives */

    InternEntry **intern;
    size_t intern_cap, intern_n;

    Value s_def, s_var, s_set, s_fn, s_defn, s_defmacro, s_macroexpand, s_if,
        s_do, s_let, s_recur, s_quote, s_quasiquote, s_unquote, s_and, s_or,
        s_amp, s_ampopt;

    Value k_nil, k_boolean, k_number, k_string, k_symbol, k_keyword, k_tuple,
        k_array, k_struct, k_table, k_function;

    FnObj *active_fn;
    int recur_argc;
    Value *recur_argv;
    int call_depth;
    int macro_depth;
    uint64_t rng;
};

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

void *lang_alloc(Lang *L, size_t n); /* zero-filled, arena owned */
char *lang_strndup(Lang *L, const char *s, size_t n);
void lang_free_arena(Lang *L);
void lang_error(Lang *L, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

Value v_nil(void);
Value v_bool(bool b);
Value v_int(int64_t i);
Value v_float(double f);
Value v_string(Lang *L, const char *s, size_t n);
Value v_symbol(Lang *L, const char *s, size_t n);
Value v_keyword(Lang *L, const char *s, size_t n);
Value v_fn_native(Lang *L, const char *name, NativeFn fn);
Value v_fn_native_closure(Lang *L, const char *name, NativeFn fn, int ncaptured,
                          const Value *captured);

Buf *buf_new(Lang *L, size_t cap);
Value v_seq(Lang *L, Buf *buf, size_t start, size_t len, bool mut, bool paren);
Value v_seq_from(Lang *L, const Value *elems, size_t n, bool mut, bool paren);
Value v_seq_over(Lang *L, Value *slots, size_t cap, size_t start, size_t len,
                 bool mut, bool paren);
Value v_seq_view(Lang *L, Value src, size_t start, size_t len);
Value v_map_empty(Lang *L, bool mut);

static inline bool is_nil(Value v) { return v.tag == V_NIL; }
static inline bool is_bool(Value v) { return v.tag == V_BOOL; }
static inline bool is_seq(Value v) { return v.tag == V_TUPLE || v.tag == V_ARRAY; }
static inline bool is_tuple(Value v) { return v.tag == V_TUPLE; }
static inline bool is_array(Value v) { return v.tag == V_ARRAY; }
static inline bool is_map(Value v) { return v.tag == V_STRUCT || v.tag == V_TABLE; }

/* Only nil and false are falsey ([P-006], §6). */
static inline bool truthy(Value v) {
    return !(v.tag == V_NIL || (v.tag == V_BOOL && !v.as.b));
}

size_t seq_len(Value v);
Value seq_at(Value v, size_t i);
void seq_set_at(Value v, size_t i, Value x);

bool map_lookup(Value map, Value key, Value *out);
void map_put(Lang *L, Value map, Value key, Value val);
bool map_del(Lang *L, Value map, Value key);

bool value_equal(Lang *L, Value a, Value b);
/* Depth-limited structural equality without diagnostics (table key lookup). */
bool value_equal_inline(Value a, Value b);
char *value_repr(Lang *L, Value v); /* malloc'd; caller frees */
/* Copy the printed form into `buf` (used in diagnostics). */
void value_repr_into(Lang *L, Value v, char *buf, size_t n);

/* ------------------------------------------------------------------ */
/* Environments                                                        */
/* ------------------------------------------------------------------ */

Env *env_new(Lang *L, Env *parent);
Binding *env_define(Lang *L, Env *env, const char *interned_name, Value v,
                    bool mutable_, bool is_macro);
Binding *env_find(Lang *L, Env *env, const char *interned_name);

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

/*
 * Read every form in `src`. `*out` receives a malloc'd array that the caller
 * frees. Returns true on success. If the input ends in the middle of a form,
 * returns false with *incomplete = true and no diagnostic (used by the REPL).
 */
bool lang_read_forms(Lang *L, const char *src, size_t len, const char *chunk,
                     Value **out, size_t *count, bool *incomplete);

/* ------------------------------------------------------------------ */
/* Evaluator                                                           */
/* ------------------------------------------------------------------ */

Value lang_eval_form(Lang *L, Value form, Env **env, bool tail);
Value lang_eval_top(Lang *L, Value *forms, size_t n);
Value lang_apply(Lang *L, Value fn, int argc, Value *argv);
Value lang_apply_macro(Lang *L, Value fn, int argc, Value *argv);

/* ------------------------------------------------------------------ */
/* Primitives                                                          */
/* ------------------------------------------------------------------ */

void prims_install(Lang *L, Env *env);

#endif /* LANG_INTERNAL_H */
