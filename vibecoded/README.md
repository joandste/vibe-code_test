# Tiny embeddable Lisp

A small C99 reference implementation of a Lisp with Janet-inspired syntax and a
recursive tree-walking evaluator. It is intentionally a compact starting point,
not a drop-in implementation of Janet.

## Build and run

```sh
make
./lisp                    # line-oriented REPL
./lisp examples/hello.lisp
make test
```

Embed it by compiling `lisp.c` into your program and including `lisp.h`:

```c
LispState *state = lisp_state_new();
LispValue result;
if (lisp_eval(state, "(+ 20 22)", &result) == 0)
    printf("%s\n", lisp_value_to_string(state, result));
else
    fprintf(stderr, "%s\n", lisp_last_error(state));
lisp_state_free(state);
```

The state owns values, environments, closures, and returned string
representations. They stay valid until `lisp_state_free`; there is no garbage
collector in this reference implementation. Native functions are registered
with `lisp_define_native`. Their arguments are already evaluated, and they
return a `LispValue`; use the `lisp_*` constructors and `lisp_raise` for errors.
Host code can also create keywords, arrays, and tables with `lisp_keyword`,
`lisp_array`, and `lisp_table`, then use the corresponding collection helpers.

## Syntax

```lisp
; comments start with ; or #
(def square (fn [x] (* x x)))
(square 9)                         ; => 81
(let [name "Ada" scores @[10 20]]
  {:name name :best (get scores 1)})
'(a b c)                           ; => [a b c]
```

Supported values are `nil`, booleans, numbers, strings, keywords (`:name`),
symbols, immutable tuples, mutable arrays, and tables (`{key value ...}`).
Parentheses are call forms. Brackets (`[...]`) construct immutable tuple
literals, while `@[...]` constructs mutable array literals. Quoting a call form
with `'(...)` produces a tuple. Function parameters and `let` bindings use
tuple literals, as in `(fn [x] ...)` and `(let [x 1] ...)`.
Core forms are `quote`, `if`, `do`, `def`, `set`, `let`, `fn`, `while`, `and`,
and `or`. Built-ins include arithmetic/comparison, `not`, `print`, `println`,
`tuple`, `get`, `put`, and `length`. Tuples can be indexed but not mutated.

All memory is retained for the lifetime of a `LispState`; create and destroy
states at suitable boundaries when embedding long-running workloads.

## Data layout and traversal

Tuples, arrays, table entries, scope bindings, and allocation bookkeeping are
stored in contiguous, growable arrays; there are no lists, cons cells, or
linked-list nodes. Lexical scopes live in an indexed array, with parent scope indices.
Tree-walking sequence traversal is recursive: each step consumes the first
element and shifts a lightweight view to the remaining array (`items + 1`,
`length - 1`). This keeps traversal allocation-free while making recursion and
array slicing explicit.
