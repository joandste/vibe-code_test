# Core Language — C reference implementation

A reference implementation of [the Core language specification](lang-spec.md)
(version 0.8.0) in portable C11, following the notes in
[implementation.md](implementation.md):

- MIT licensed ([LICENSE](LICENSE)).
- Multi-file development (`src/`, `include/`).
- Tree-walking evaluator.
- Amalgamated single-file build: deliberately skipped for now.

The spec text is the source of truth; clauses are cited in the source comments
by their stable IDs (`[P-001]`, `[T-014]`, `[V-013]`, …).

## Build

```sh
make            # builds ./langrun
make lib        # builds liblang.a
make example    # builds ./embed (embedding example)
make test       # builds and runs the test suite
make clean
```

`cc` with C11 support is the only requirement (`-lm` is linked).

## Run

```sh
./langrun                 # read-eval-print loop
./langrun file.lang       # evaluate a file, no output except errors
./langrun -p file.lang    # evaluate a file, print every top-level value
./langrun -e '(map inc [1 2 3])'   # evaluate an expression and print it
```

The language has no printing primitive (see §10's "standard library beyond the
primitives listed is out of scope"), so writing values out is the host's job:
the REPL and `-p` print the value of each top-level form, exactly like a
Lisp REPL.

```lisp
> (defn sum [xs acc]
    (if (empty? xs)
      acc
      (recur (next xs) (+ acc (first xs)))))
<function sum>
> (sum (range 10) 0)
45
> (def v (drop 2 @[1 2 3 4 5]))
@[3 4 5]
> (length v)
3
> (= (quote (1 2)) (quote [1 2]))
false
```

## Layout

| Path | Contents |
|---|---|
| [include/lang.h](include/lang.h) | Public embedding API |
| [src/internal.h](src/internal.h) | Value model, interpreter state, internal interfaces |
| [src/value.c](src/value.c) | Values, buffers/views, structs/tables, interning, equality, printing |
| [src/reader.c](src/reader.c) | Lexer and reader (§3, §4) |
| [src/env.c](src/env.c) | Lexical environments |
| [src/eval.c](src/eval.c) | Evaluator, special forms, macros, `recur` (§8, §9) |
| [src/prims.c](src/prims.c) | Core primitives (§10) |
| [src/api.c](src/api.c) | Interpreter lifecycle, `lang_eval_string` / `lang_eval_file` |
| [src/main.c](src/main.c) | REPL and script driver |
| [tests/cases](tests/cases) | Programs with expected output |
| [tests/errors](tests/errors) | Programs that must fail, with expected diagnostics |
| [tests/embed_test.c](tests/embed_test.c) | Public-API test |
| [examples/embed.c](examples/embed.c) | Minimal embedding example |

## Coverage

Everything the specification requires in §3–§10 is implemented:

- **Reader (§3, §4)** — all token classes, `()`, `[]`, `{}`, `@()`, `@[]`,
  `@{}`, escapes (`\n \t \r \\ \" \0 \xHH`), comments, hex/float literals,
  `@` immediately followed by an opening delimiter, source locations on
  sequences, and reader errors for `@foo`, stray punctuation, odd struct/table
  element counts.
- **Data model (§5)** — three distinct non-values; tuples/arrays sharing a
  buffer+offset+length header; shape and mutability flags; views that preserve
  both; structs/tables with a shared layout and no sequence behaviours;
  structural equality that respects flags; conversion with identity
  fast-paths.
- **Truthiness (§6)** — only `nil` and `false` are falsey.
- **Views (§7)** — `next`, `drop` and `slice` are O(1) header allocations over
  the same buffer; `drop`/`slice` clamp, `next` errors on empty.
- **Iteration and tail calls (§8)** — `recur` is a jump implemented by the
  function call loop, so it does not grow the C stack; ordinary calls do grow
  it; `recur` is validated statically when a `fn` is created (outside a fn,
  non-tail position, and arity mismatches are rejected).
- **Special forms (§9)** — `if do let def var set! fn defn defmacro
  macroexpand quote quasiquote unquote and or recur`, plus the evaluation
  rules for every form kind.
- **Primitives (§10)** — C-001 through C-071: arithmetic, comparison, boolean
  logic, sequence access, type predicates, mutators, builders and the partial
  standard library (`apply`, `identity`, `complement`, `comp`, `const`).

## Implementation decisions

These are the choices the spec deliberately leaves open (§5.10, §5.11, §12,
§13), plus the reading taken where the text allows more than one.

**Evaluator.** Tree-walking over the reader's values, as suggested by
implementation.md. Macro expansion happens while a form is evaluated; `recur`
positions are checked when the `fn` value is created.

**Value representation.** `Value` is a 16-byte tagged struct. Tuples and arrays
point to a `SeqObj` holding `{buffer, start, length, mutability, shape}`.
`next`, `drop` and `slice` allocate only a new `SeqObj`, so subranges never copy
their source ([V-003], [V-004], [V-012]).

**Buffer capacity and `push!`.** [T-031] describes a base sequence as one with
`start = 0, length = capacity`, so the buffer tracks a *logical* capacity (the
length of the base) separately from the allocated size. `push!` only accepts a
mutable array with `start + length == capacity` ([C-040a]); appending grows the
logical capacity and the allocation is grown geometrically in the background.
This keeps repeated `push!` on a base amortized O(1) while refusing to write
into slots another view considers its own.

**Memory management ([T-034], [T-035]).** Values are allocated from an arena
owned by the interpreter and released by `lang_free`. This satisfies the
spec's "automatic memory management" without mandating a strategy; the
implementation.md note "GC: TBD" is honoured by not committing to a collector
yet. The trade-off is that a long-running interpreter does not reclaim
intermediate values.

**`def` and closures ([KW-002]).** A top-level `def`/`var`/`defn`/`defmacro`
installs a fresh scope frame instead of overwriting a binding, so closures
created earlier keep seeing the previous value. A lookup that misses in the
chain captured by a closure falls back to the newest top-level frame, which
makes forward references work:

```lisp
(def x 1)
(defn read-x [] x)   ; captures the binding x = 1
(def x 2)            ; new binding for subsequent code
(read-x)             ; => 1
x                    ; => 2
```

The consequence is that a symbol that is not found locally is looked up through
one frame per preceding top-level definition; the implementation favours clear
semantics over a flatter environment here.

**Numbers.** Integers are 64-bit and checked: overflow in `+`, `-`, `*`, `inc`,
`dec`, `abs` raises an error instead of wrapping. Mixing integers and floats
produces a float; when all operands are integers the result is an integer, so
`(/ 7 2)` is `3` while `(/ 7 2.0)` is `3.5`, and `(/ 2)` is the float `0.5`.
Integer division by zero is an error; float division follows IEEE-754. The
spec fixes only "mixing produces a float" [§10.1], so the integer case is a
documented reading.

**Sorting.** [C-043]/[C-062] name `<` as the default comparator, so a
comparator is a *less-than predicate*: `(sort xs cmp)` orders ascending when
`(cmp a b)` is truthy. Without a comparator only numbers are comparable.
Sorting is a stable merge sort.

**Errors.** §12 leaves the representation of runtime errors open, so errors are
host-level: the evaluator raises a diagnostic that unwinds to the embedding API
or the REPL, which reports it and (in the REPL) continues. Nothing about this is
visible to language code.

**Macros.** Unhygienic, expanding once per call (the §13.4/§13.5 direction).
`quasiquote`/`unquote` are implemented even though §12 lists their semantics as
unsettled: a single `unquote` at depth 1 evaluates, nested `quasiquote`
increases depth, and no `unquote-splicing` form exists because the spec does
not name one. `macroexpand` expands exactly one level and never evaluates its
argument ([KW-013]).

**`nil` in sequence positions.** [C-019]–[C-022] explicitly accept `nil`;
builders accept it as an empty sequence ([C-052]–[C-064]). `drop`, `slice` and
`get` also return `nil` for a `nil` collection, as a consistent extension.

**Call depth.** Ordinary calls grow the C stack ([I-009]), so a limit of 2000
nested calls turns runaway recursion into a diagnosed error. `recur` loops are
constant-stack.

**Quote shorthand.** The examples in §5.4 are written with `'x`, but the
lexical grammar (§3) defines no `'` token — the only sigil is `@` and `quote`
is a special form. This implementation follows the grammar, so the §5.4
equivalences are written `(quote (1 2))` etc.; they are exercised in
[tests/cases/values.lang](tests/cases/values.lang).

**Named `fn`.** The illustration in §8.1 writes `(fn sum [xs acc] ...)`, while
[KW-005] defines `(fn [params] body...)` as the anonymous form and [KW-006]
defines `defn` as `def` plus that anonymous `fn`. The implementation follows
§9.5, so the canonical loop is written `(defn sum [xs acc] ...)` in
[tests/cases/recur.lang](tests/cases/recur.lang).

**Structs, tables and `empty?`.** [C-020] states that a non-sequence, non-`nil`
value is not empty, while [L-005] says `empty?` does not apply to structs and
tables. The more specific rule in §10.4 wins: `(empty? {})` and `(empty? @{})`
return `false`, and `(length {})` / `(first {})` are errors.

## Embedding

```c
#include "lang.h"

Lang *L = lang_new();
char *result = NULL, *error = NULL;
if (lang_eval_string(L, "(+ 1 2)", "example", &result, &error) != 0) {
    /* error holds a malloc'd diagnostic */
    free(error);
} else {
    /* result holds the printed value, e.g. "3" */
    free(result);
}
lang_free(L);
```

See [examples/embed.c](examples/embed.c) (`make example`). The API is minimal
on purpose: implementation.md lists it as TBD, so it only covers creation,
evaluation of strings/files, and destruction.

## Tests

`make test` runs:

- `tests/run.sh` — every program in `tests/cases` must exit 0 and print exactly
  its `.expected` file with `-p`; every program in `tests/errors` must exit
  non-zero and print exactly its `.expected` diagnostic. 36 cases cover the
  reader, flags and equality, views, mutation, control flow, `recur`, macros,
  builders, arithmetic and the standard library.
- `tests/embed_test` — the public API round trip and error reporting.

The suite is also clean under `-fsanitize=address,undefined`:

```sh
make clean
make CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined" \
     LDLIBS="-lm -fsanitize=address,undefined"
make test
```

To add a case, drop `tests/cases/NAME.lang` and `tests/cases/NAME.expected`
side by side (the runner substitutes `<file>` for the test path in output).

## Not implemented (by design)

- Amalgamated `lang.c`/`lang.h` build (implementation.md says "skip for now").
- A garbage collector (implementation.md: "GC: TBD"; see the arena note above).
- Buffers, string operations, destructuring, multi-arity functions,
  `trampoline`, `copy`/`compact` — all §13 backlog items.
- Any standard library beyond §10.8.

## Vibecoded implementation

- vibecoded implementation lives in `vibecoded/` and has it's own readme
