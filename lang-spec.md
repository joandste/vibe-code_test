# Core Language Specification

**Version:** 0.8.0
**Status:** Draft
**License:** CC BY 4.0

A small, Janet-inspired Lisp with tuples and arrays as its core data structures,
explicit mutation, explicit tail calls, and macros.

This work is licensed under a Creative Commons Attribution 4.0 International
License. See LICENSE for details.

---

## Table of Contents

1. [Principles](#1-principles)
2. [Notation](#2-notation)
3. [Lexical Grammar](#3-lexical-grammar)
4. [Syntactic Grammar](#4-syntactic-grammar)
5. [Data Model](#5-data-model)
6. [Truthiness](#6-truthiness)
7. [Views](#7-views)
8. [Iteration and Tail Calls](#8-iteration-and-tail-calls)
9. [Special Forms](#9-special-forms)
10. [Core Primitives](#10-core-primitives)
11. [Limitations](#11-limitations)
12. [Open Questions](#12-open-questions)
13. [Backlog](#13-backlog)

---

## 1. Principles

- **[P-001]** **Janet-like syntax.** S-expressions with `()`, `[]`, `{}`, `@()`, `@[]`, `@{}` and symbols.
- **[P-002]** **Tuples and arrays are the core.** Everything else is built on them.
- **[P-003]** **Iteration is recursion.** Loops consume a sequence one element at a time via `first` and `next`.
- **[P-004]** **Views, not copies.** Advancing through a sequence MUST return a view over the same buffer.
- **[P-005]** **Three distinct non-values.** `true`, `false`, and `nil` are separate values.
- **[P-006]** **Truthiness matches Janet and Clojure.** Only `nil` and `false` are falsey.
- **[P-007]** **Homoiconic.** Code is tuples. There is no separate AST type.
- **[P-008]** **Explicit mutability.** `@` marks mutable literals; `!` marks mutating procedures; `set!` mutates bindings.
- **[P-009]** **Only `()` is a call.** Every other delimited form is a literal constructor.
- **[P-010]** **`@()` and `@[]` are identical.** Both produce the same mutable array.
- **[P-011]** **`{}` and `@{}` are structs and tables.** Immutable and mutable respectively.
- **[P-012]** **Explicit tail calls.** `recur` is the only tail call form. Ordinary calls grow the stack.
- **[P-013]** **Macros exist.** They run at compile time and return forms.
- **[P-014]** **No list or vector types.** Tuples carry a shape flag (paren or bracket); this is metadata, not a type.
- **[P-015]** **Unprefixed collection primitives.** `push!`, `pop!`, `peek`, `get`, `put!`, `del!`, `length`.
- **[P-016]** **Optional and variadic parameters.** `&opt` and `&` in parameter vectors.
- **[P-017]** **Builders return arrays.** Every operation that builds a new sequence returns a mutable array, regardless of input type. Freezing is explicit via `tuple`.
- **[P-018]** **Designed to be embedded.** Implementations SHOULD provide an embedding API and SHOULD support building as a library. The spec does not mandate any particular API or build system.

---

## 2. Notation

- `::=` lexical production; `:=` syntactic production.
- `|` alternation; `[ ]` optional; `{ }` zero or more; `( )` grouping.
- `'x'` literal character.
- **MUST**, **MUST NOT**, **SHOULD**, **MAY** — RFC 2119 style.
- Clause IDs (`[P-001]`, `[LEX-001]`, …) are stable. Never renumber; append or deprecate.

---

## 3. Lexical Grammar

### 3.1 Tokens

```
token       ::= whitespace | comment | symbol | keyword
              | number | string | punctuation | sigil

whitespace  ::= ' ' | '\t' | '\n' | '\r'
comment     ::= ';' { any-char-except-newline }
sigil       ::= '@'
punctuation ::= '(' | ')' | '[' | ']' | '{' | '}'
```

### 3.2 Symbols

```
symbol      ::= initial subsequent*
initial     ::= letter | special
subsequent  ::= letter | digit | special
letter      ::= 'a'..'z' | 'A'..'Z'
special     ::= '!' | '$' | '%' | '&' | '*' | '+' | '-' | '.' | '/'
              | '<' | '=' | '>' | '?' | '^' | '_'
```

### 3.3 Keywords, Numbers, Strings

```
keyword     ::= ':' symbol

integer     ::= ['-'] digit+ | '0x' hexdigit+
float       ::= ['-'] digit+ '.' digit+ [exponent]
exponent    ::= ('e'|'E') ['+'|'-'] digit+

string      ::= '"' (strchar | escape)* '"'
escape      ::= '\n' | '\t' | '\r' | '\\' | '\"' | '\0'
              | '\x' hexdigit hexdigit
```

### 3.4 Lexical Rules

- **[LEX-001]** Source is UTF-8.
- **[LEX-002]** Whitespace separates tokens and is otherwise insignificant.
- **[LEX-003]** A symbol MUST NOT begin with a digit.
- **[LEX-004]** Punctuation and `@` are reserved and MUST NOT appear inside symbols.
- **[LEX-005]** `true`, `false`, `nil`, `&`, `&opt` are reserved and MUST NOT be rebound.
- **[LEX-006]** `@` MUST be immediately followed by `(`, `[`, or `{`, with no intervening whitespace.
- **[LEX-007]** A trailing `!` on a symbol is a convention marking side effects, not a syntax rule.

---

## 4. Syntactic Grammar

```
form         := atom | call-form | literal-form

atom         := symbol | keyword | number | string | boolean | nil
boolean      := 'true' | 'false'
nil          := 'nil'

call-form    := '(' { form } ')'

literal-form := tuple-form | struct-form | array-form | table-form

tuple-form   := '[' { form } ']'
struct-form  := '{' { form form } '}'
array-form   := '@(' { form } ')' | '@[' { form } ']'
table-form   := '@{' { form form } '}'
```

- **[SYN-001]** A `call-form` reads as a tuple with the **paren** shape flag. It is the only call form.
- **[SYN-002]** A `tuple-form` reads as a tuple with the **bracket** shape flag.
- **[SYN-003]** A `struct-form` reads as a **struct** (immutable).
- **[SYN-004]** An `array-form` reads as a mutable **array**. Both spellings (`@()`, `@[]`) MUST produce identical values.
- **[SYN-005]** A `table-form` reads as a mutable **table**.
- **[SYN-006]** The reader MUST attach source location (line, column) to every sequence.
- **[SYN-007]** `@` MUST NOT appear outside a literal form.
- **[SYN-008]** `@foo` (sigil followed by a symbol) is invalid and MUST signal a reader error.
- **[SYN-009]** An array literal MUST allocate fresh storage. It MUST NOT share with the source form.

---

## 5. Data Model

### 5.1 Core Values

- **[T-001]** A **tuple** is an immutable, contiguous, indexed sequence.
- **[T-002]** An **array** is a mutable, contiguous, indexed sequence.
- **[T-003]** Tuples and arrays MUST share the same in-memory layout: a header plus a contiguous buffer.
- **[T-004]** Every slot MAY hold any value, including other tuples, arrays, structs, or tables.
- **[T-005]** `true`, `false`, and `nil` are three distinct values.
  - `(boolean? true)` and `(boolean? false)` → `true`; `(boolean? nil)` → `false`.
  - `(nil? nil)` → `true`; `(nil? true)` and `(nil? false)` → `false`.
  - `true`, `false`, and `nil` MUST NOT be equal to each other.

### 5.2 Flags

A value header MAY carry up to two flags.

- **[T-006]** A **mutability flag** is present on every sequence and associative collection:
  - **immutable** — used for tuples and structs
  - **mutable** — used for arrays and tables
- **[T-006a]** A **shape flag** is present on every tuple, and only on tuples:
  - **paren** — produced by `()`
  - **bracket** — produced by `[]`
- **[T-006b]** Arrays carry no shape flag. `@()` and `@[]` MUST be indistinguishable at runtime.
- **[T-006c]** Structs and tables carry no shape flag. Their only flag is mutability.
- **[T-006d]** The purpose of the shape flag is to let the evaluator distinguish call forms from literal data. A tuple with the **paren** shape is a call form; a tuple with the **bracket** shape is literal data. Arrays, structs, and tables are always literals, so they need no shape flag.

The reader can produce the following runtime values:

| Source | Runtime kind | Mutability | Shape |
|---|---|---|---|
| `(...)` | tuple | immutable | paren |
| `[...]` | tuple | immutable | bracket |
| `@(...)` | array | mutable | — |
| `@[...]` | array | mutable | — |
| `{...}` | struct | immutable | — |
| `@{...}` | table | mutable | — |

- **[T-007]** Flags do NOT affect layout, indexing, `first`, `next`, or view semantics.

### 5.3 Predicates

- **[T-008]** `(tuple? v)` → `true` iff `v` is an immutable sequence.
- **[T-009]** `(array? v)` → `true` iff `v` is a mutable sequence.
- **[T-010]** `(paren? v)` → `true` iff `v` is a tuple with the paren shape flag. Arrays, structs, and tables return `false`.
- **[T-011]** `(bracket? v)` → `true` iff `v` is a tuple with the bracket shape flag. Arrays, structs, and tables return `false`.
- **[T-012]** `(struct? v)` → `true` iff `v` is a struct.
- **[T-013]** `(table? v)` → `true` iff `v` is a table.

### 5.4 Equality

- **[T-014]** Two tuples with the same elements but different shape flags MUST NOT be equal.
- **[T-015]** Two sequences with the same elements but different mutability flags MUST NOT be equal.
- **[T-016]** `(= '@(1 2) '@[1 2])` → `true`. Arrays have no shape flag.

Examples:

- `(= '(1 2) '[1 2])` → `false` (paren vs bracket)
- `(= '[1 2] '@[1 2])` → `false` (immutable vs mutable)
- `(= '(1 2) '@(1 2))` → `false` (immutable vs mutable)
- `(= '@(1 2) '@[1 2])` → `true`
- `(= {1 2} @{1 2})` → `false` (struct vs table)

### 5.5 Structs and Tables

- **[T-017]** A **struct** is an immutable associative collection mapping keys to values. Produced by `{...}`.
- **[T-018]** A **table** is a mutable associative collection mapping keys to values. Produced by `@{...}`.
- **[T-019]** Structs and tables MUST share layout.
- **[T-020]** Neither structs nor tables carry a shape flag. Their only flag is mutability.
- **[T-021]** Structs and tables are NOT sequences. `first`, `next`, `empty?` MUST NOT apply.

### 5.6 Code is Data

- **[T-022]** Source code is represented entirely with tuples, arrays, structs, tables, symbols, keywords, and literals.
- **[T-023]** `(quote x)` returns `x` unchanged, including flags.
  - `(quote (f x))` → the tuple `(f x)` with the paren shape
  - `(quote [f x])` → the tuple `[f x]` with the bracket shape
  - `(quote @(f x))` → an array containing `f`, `x`
- **[T-024]** Every sequence primitive works on code.
- **[T-025]** A macro receives its arguments as sequences and returns a sequence or atom.

### 5.7 Building, Viewing, Mutating

Three operations can be performed on sequences and collections.

- **[T-026]** A **builder** produces a new sequence from an existing one. Builders return **mutable arrays** and never mutate their input. See §10.7.
- **[T-027]** A **view** operation produces a subrange of an existing buffer without copying. See §7.
- **[T-028]** A **mutator** modifies an existing array or table in place. See §10.6.

### 5.8 Conversion

- **[T-029]** `(array coll)` and `(tuple coll)` convert between mutable and immutable representations.
  - `(array coll)` returns a mutable array with the elements of `coll`.
  - `(tuple coll)` returns an immutable tuple with the elements of `coll`.
  - Both copy. Neither shares storage with `coll`.
  - `(array a)` where `a` is already an array MUST return `a` unchanged.
  - `(tuple t)` where `t` is already a tuple MUST return `t` unchanged.
  - `(array)` and `(tuple)` with no arguments return an empty sequence.
  - The tuple produced by `(tuple coll)` has the **bracket** shape flag.

### 5.9 Views Are Not a Type

- **[T-030]** A **view** is not a distinct runtime type. Every sequence value is a tuple or an array. "View" describes a relationship between a sequence and the buffer it references, not a kind of value.
- **[T-031]** Every sequence header contains a buffer reference, a start offset, and a length. A sequence with `start = 0` and `length = capacity` is indistinguishable from what other languages call "the base."
- **[T-032]** There is no `view?` predicate. The runtime does not track which sequences are views and which are not.
- **[T-033]** A freshly-constructed array or tuple is a view with `start = 0`. There is nothing special about it.

### 5.10 Memory Management

- **[T-034]** The language assumes automatic memory management. Values become eligible for reclamation when they are unreachable from any live root.
- **[T-035]** The spec does not require any particular GC algorithm. Implementations MAY use tracing, reference counting, arena allocation, or any other strategy that satisfies the reachability guarantees in §7.
- **[T-036]** The spec does not specify memory layout, allocation strategy, or stack representation. These are implementation details.

### 5.11 Non-Goals

The following are deliberately outside the scope of this spec:

- Evaluation strategy (tree-walking, bytecode, compilation to native code).
- Build system, packaging, or distribution format.
- Compiled artifact format.
- Runtime embedding API.
- REPL behavior beyond what §9 specifies.
- Standard library beyond the primitives listed in §10.
- Toolchain, editor integration, or language server.

---

## 6. Truthiness

- **[TR-001]** `nil` is falsey.
- **[TR-002]** `false` is falsey.
- **[TR-003]** Every other value is truthy — including `0`, `""`, `[]`, `()`, `@[]`, `{}`, `@{}`.
- **[TR-004]** Matches Janet and Clojure.

---

## 7. Views

### 7.1 View Semantics

- **[V-001]** A **view** refers to a contiguous subrange of an existing tuple or array buffer.
  - Views are produced by `next`, `drop`, and `slice`, and by rest-parameter binding.
  - Base tuples and base arrays are NOT views — see §5.9.
- **[V-002]** A view consists of a buffer reference, a start offset, and a length.
- **[V-003]** A view MUST NOT copy the source buffer.
- **[V-004]** Creating a view MUST be O(1).
- **[V-005]** Indexing a view at index `i` returns the element at `start + i`.
- **[V-006]** `(next v)` returns a view starting at `start + 1`, length `length - 1`.
  - `next` on a base returns a view over that base's buffer.
  - `next` on a view returns a view over the same buffer, offset by one.
  - `next` on an empty sequence MUST signal an error.
- **[V-007]** `(first v)` returns the element at `start`; on empty, signals an error.
- **[V-008]** A view MUST preserve the mutability and (for tuples) shape flags of its source.
- **[V-009]** A view keeps its source buffer alive for as long as the view is reachable.
- **[V-010]** A buffer MAY be freed once no base value and no view reference it. The GC only tracks buffer reachability; it does not track element usage.
- **[V-011]** A view of a tuple is immutable. A view of an array is mutable, and mutations through the view are visible to all sharers.
- **[V-012]** `next`, `drop`, and `slice` MUST return views. An implementation MUST NOT copy the source buffer for these operations. This makes subrange operations O(1).

### 7.2 Subrange Operations

These return views over the same buffer as their input. They do not copy and do not allocate a new buffer.

- **[V-013]** `(drop n coll)` returns a view over `coll` starting at `start + n`, with length `max(0, length - n)`.
  - `n <= 0` → view over the whole of `coll`.
  - `n >= length` → empty view at offset `length`.
  - `drop` MUST NOT signal an error for out-of-range `n`.
  - `next` errors on empty input; `drop` clamps. `(next [])` errors; `(drop 1 [])` returns an empty view.
  - For non-empty `coll`, `(drop 1 coll)` and `(next coll)` return equivalent views.
- **[V-014]** `(slice n m coll)` returns a view over `coll` from offset `n` to offset `m` (exclusive), both clamped to `[0, length]`.
  - If `n >= m`, returns an empty view at offset `min(n, length)`.
  - `(drop n coll)` is equivalent to `(slice n (length coll) coll)`.

### 7.3 Complexity Guarantees

The following operations are guaranteed O(1) in the length of the sequence:

- `first`, `next`, `drop`, `slice`, `length`, `get` at any index.
- `push!`, `pop!`, `peek` (at the end of a buffer).

The following are O(n) in the length of the sequence:

- `map`, `filter`, `take`, `concat`, `reverse`, `sort`.
- `reduce`, `each`, `some`, `every?`, `find`.

- **[V-015]** Implementations MUST NOT degrade the O(1) operations above to O(n).

> **Note on Janet.** Janet's `tuple/slice` and `array/slice` copy, making them O(n).
> Janet's `next` is a math primitive; sequence iteration uses a different mechanism.
> This spec's view model is a deliberate extension of Janet's design, not a
> description of it.

---

## 8. Iteration and Tail Calls

### 8.1 Canonical Loop

- **[I-001]** Iterate by recursing on `first` and `next`.
- **[I-002]** The loop terminates when the sequence is empty.
- **[I-003]** Each `next` returns a view over the same buffer — no allocation.

```lisp
(fn sum [xs acc]
  (if (empty? xs)
    acc
    (recur (next xs) (+ acc (first xs)))))
```

### 8.2 `recur`

- **[I-004]** `recur` is a special form. It MUST appear in tail position within a `fn` body.
- **[I-005]** `(recur arg...)`
  - The number of `arg`s MUST equal the parameter count of the nearest enclosing `fn`.
  - Args are evaluated left to right in the current environment.
  - Control jumps to the top of that `fn` with the new values.
  - `recur` is a jump, not a call — it MUST NOT grow the stack.
  - `recur` never returns; it either loops or fails at compile time.
- **[I-006]** `recur` outside any `fn` → compile-time error.
- **[I-007]** `recur` in non-tail position → compile-time error.

### 8.3 Tail Position

- **[I-008]** A position is tail if it is:
  - The last expression of a `fn` body.
  - The `then` or `else` branch of an `if` that is itself in tail position.
  - The last expression of a `do` that is itself in tail position.
  - The last expression of a `let` that is itself in tail position.
- **[I-008a]** Nothing else is in tail position.

### 8.4 No Implicit TCO

- **[I-009]** Ordinary calls (including self-calls) MUST grow the stack.
- **[I-010]** Mutual recursion grows the stack.

---

## 9. Special Forms

Reserved symbols:

```
def  var  set!  fn  defn  defmacro  macroexpand
if  do  let  recur  quote  quasiquote  unquote
next  first  empty?  and  or  &  &opt
true  false  nil
```

### 9.1 `if`

- **[KW-001]** `(if cond then [else])`
  - `cond` evaluated exactly once.
  - If truthy → evaluate and return `then`.
  - Otherwise → evaluate and return `else`; if absent, return `nil`.
  - `recur` allowed as last expression of `then`/`else` if the `if` is in tail position.

### 9.2 `def`

- **[KW-002]** `(def name value)`
  - `name` MUST be a symbol.
  - Binds `value` to `name` as an immutable binding.
  - Returns the bound value.
  - `set!` on a `def` binding MUST signal an error.
  - Re-`def`ing at top level replaces the binding for subsequent code. Existing closures keep the previous value.

### 9.3 `var`

- **[KW-003]** `(var name value)`
  - `name` MUST be a symbol.
  - Binds `value` to `name` as a mutable binding.
  - Returns the bound value.
  - Only `var` bindings can be targeted by `set!`.

### 9.4 `set!`

- **[KW-004]** `(set! name value)`
  - `name` MUST be a symbol bound to a `var`.
  - Evaluates `value`, stores it in the binding, returns it.
  - Special form, not a procedure.
  - MUST NOT create a binding. Unbound or immutable target → error.

### 9.5 `fn`

- **[KW-005]** `(fn [params] body...)`
  - Returns a callable closure over the current environment.
  - Anonymous.
  - Is a `recur` target.

#### Parameter Grammar

```
params   := fixed* [ '&opt' optional* ] [ '&' rest ]
          | fixed* [ '&' rest ]
fixed    := symbol
optional := symbol
rest     := symbol
```

- **[KW-005a]** `fixed` are required positional parameters.
- **[KW-005b]** `&opt` introduces optional parameters, each defaulting to `nil`.
- **[KW-005c]** `&` introduces a rest parameter, bound to a view over remaining arguments, or `nil` if none.
- **[KW-005d]** `&opt` and `&` MAY appear together: `[a &opt b c & rest]`.
- **[KW-005e]** Call arity MUST be ≥ fixed and ≤ fixed + optional (unbounded if `&` present).
- **[KW-005f]** Arity mismatch → runtime error.
- **[KW-005g]** `&` and `&opt` are markers, not bindable names.
- **[KW-005h]** No duplicate parameter names.
- **[KW-005i]** Parameter names MUST NOT be `&`, `&opt`, `true`, `false`, or `nil`.

### 9.6 `defn`

- **[KW-006]** `(defn name [params] body...)`
  - Equivalent to `(def name (fn [params] body...))`.
  - `params` MUST conform to §9.5.
  - Returns the bound function.

### 9.7 `let`

- **[KW-007]** `(let [binding...] body...)`
  - Bindings are an even-length vector-tuple of alternating symbol/expression.
  - Bindings are sequential: each sees the previous.
  - New lexical scope. Returns last body expression.
  - Bindings are immutable — `set!` on them is an error.
  - `let` is NOT a `recur` target. `recur` inside a `let` targets the enclosing `fn`.

### 9.8 `do`

- **[KW-008]** `(do form...)`
  - Evaluates forms left to right.
  - Returns last value.
  - `recur` allowed as last form if the `do` is in tail position.

### 9.9 `quote`

- **[KW-009]** `(quote form)`
  - Returns `form` unchanged (flags preserved).
  - `(quote (f x))` → tuple with paren shape.
  - `(quote [f x])` → tuple with bracket shape.
  - `(quote @(f x))` → array containing `f`, `x`.

### 9.10 `and` / `or`

- **[KW-010]** `(and a b ...)` — Special form. Returns the first falsey argument, or the last argument if all are truthy. With no args, returns `true`.
- **[KW-011]** `(or a b ...)` — Special form. Returns the first truthy argument, or the last argument if all are falsey. With no args, returns `nil`.
- **[KW-010a]** `and` and `or` are special forms because they short-circuit. They MUST NOT be procedures.

### 9.11 `defmacro`

- **[KW-012]** `(defmacro name [params] body...)`
  - Defines a compile-time procedure that returns a form.
  - `params` MUST conform to §9.5.
  - Arguments are received unevaluated as sequences.
  - The return value is substituted at the call site and compiled.
  - `defmacro` is a special form.
  - A macro name used as a value is an error.
  - `recur` inside a macro body is an error.

### 9.12 `macroexpand`

- **[KW-013]** `(macroexpand form)`
  - If `form` is a tuple with the paren shape whose head names a macro, expand once and return the result.
  - Otherwise return `form` unchanged.
  - Debugging aid.

### 9.13 Evaluation Rules

- **[KW-014]** A form is evaluated as follows:
  - `()` with a special-form head → apply the special form.
  - `()` with a macro head → expand, then evaluate the result.
  - `()` otherwise → evaluate head, evaluate args left to right, apply.
  - `()` empty → evaluates to itself.
  - `[...]` → evaluate elements left to right, build a new tuple with the bracket shape.
  - `{...}` → evaluate elements left to right, build a new struct.
  - `@(...)` or `@[...]` → literal, NOT a call. Evaluate elements left to right, build a new array with fresh storage.
  - `@{...}` → evaluate elements left to right, build a new table.
  - `nil`, `true`, `false`, numbers, strings, keywords → self-evaluating.
  - A symbol → look up in the current environment; error if unbound.

### 9.14 Mutation Conventions

- **[KW-015]** Mutating procedures MUST be named with a trailing `!`.
- **[KW-015a]** The `!` suffix is a convention, not enforced by the reader.
- **[KW-015b]** Procedures without `!` MUST NOT mutate their arguments.

---

## 10. Core Primitives

These are language-level, unprefixed primitives.

### 10.1 Arithmetic

All arithmetic operates on numbers. Integer and float types coexist; mixing produces a float.

| Primitive | Behavior |
|---|---|
| **[C-001]** `(+ a b ...)` | Sum. With no args, `0`. |
| **[C-002]** `(- a b ...)` | With one arg, negation. Otherwise subtraction left to right. |
| **[C-003]** `(* a b ...)` | Product. With no args, `1`. |
| **[C-004]** `(/ a b ...)` | With one arg, reciprocal. Otherwise division left to right. |
| **[C-005]** `(mod a b)` | Modulo. Sign follows divisor. |
| **[C-006]** `(rem a b)` | Remainder. Sign follows dividend. |
| **[C-007]** `(inc x)` | `(+ x 1)`. |
| **[C-008]** `(dec x)` | `(- x 1)`. |
| **[C-009]** `(abs x)` | Absolute value. |
| **[C-010]** `(min a b ...)` | Minimum. |
| **[C-011]** `(max a b ...)` | Maximum. |

### 10.2 Comparison

| Primitive | Behavior |
|---|---|
| **[C-012]** `(= a b ...)` | Structural equality. See §5.4 for sequence equality. |
| **[C-013]** `(not= a b ...)` | Complement of `=`. |
| **[C-014]** `(< a b ...)` | Numeric less-than, chained. |
| **[C-015]** `(<= a b ...)` | Numeric less-than-or-equal, chained. |
| **[C-016]** `(> a b ...)` | Numeric greater-than, chained. |
| **[C-017]** `(>= a b ...)` | Numeric greater-than-or-equal, chained. |

### 10.3 Boolean Logic

| Primitive | Behavior |
|---|---|
| **[C-018]** `(not x)` | `true` iff `x` is falsey. |

`and` and `or` are special forms. See §9.10.

### 10.4 Sequence Access

| Primitive | Behavior |
|---|---|
| **[C-019]** `(length v)` | Number of elements. `nil` → `0`. |
| **[C-020]** `(empty? v)` | `true` iff length is 0. `nil` → `true`. Non-sequence, non-`nil` → `false`. |
| **[C-021]** `(first v)` | First element. `nil` → `nil`. Empty → error. |
| **[C-022]** `(next v)` | View offset by one. `nil` → `nil`. Empty → error. |
| **[C-023]** `(get v i)` | Element at index `i`, or `nil` if out of range. |
| **[C-024]** `(drop n coll)` | View dropping first `n`. See §7.2. |
| **[C-025]** `(slice n m coll)` | View from `n` to `m`. See §7.2. |

### 10.5 Type Predicates

| Primitive | Returns `true` iff… |
|---|---|
| **[C-026]** `(nil? v)` | `v` is `nil` |
| **[C-027]** `(boolean? v)` | `v` is `true` or `false` |
| **[C-028]** `(number? v)` | `v` is an integer or float |
| **[C-029]** `(string? v)` | `v` is a string |
| **[C-030]** `(symbol? v)` | `v` is a symbol |
| **[C-031]** `(keyword? v)` | `v` is a keyword |
| **[C-032]** `(tuple? v)` | `v` is an immutable sequence |
| **[C-033]** `(array? v)` | `v` is a mutable sequence |
| **[C-034]** `(paren? v)` | `v` is a tuple with the paren shape flag |
| **[C-035]** `(bracket? v)` | `v` is a tuple with the bracket shape flag |
| **[C-036]** `(struct? v)` | `v` is a struct |
| **[C-037]** `(table? v)` | `v` is a table |
| **[C-038]** `(fn? v)` | `v` is callable |
| **[C-039]** `(type v)` | Returns a keyword naming the kind of `v` (see below) |

`type` returns one of:

```
:nil  :boolean  :number  :string  :symbol  :keyword
:tuple  :array  :struct  :table  :function
```

- **[C-039a]** `(type '(1 2))` and `(type '[1 2])` both return `:tuple`. Shape is not part of the type.
- **[C-039b]** `(type nil)` returns `:nil`.

### 10.6 Mutators

These mutate their input and return it. They MUST be named with a trailing `!`. Passing an immutable value where a mutable one is required MUST signal an error.

**Arrays:**

| Primitive | Behavior |
|---|---|
| **[C-040]** `(push! arr x)` | Append `x`. Returns `arr`. `arr` MUST be a mutable sequence extending to the end of its buffer. |
| **[C-041]** `(pop! arr)` | Remove and return last element. `nil` if empty. `arr` MUST be a mutable sequence. |
| **[C-042]** `(peek arr)` | Return last element without removing. `nil` if empty. `arr` MUST be an array. |
| **[C-043]** `(sort! arr [cmp])` | Sort in place. Returns `arr`. On a view, sorts only the viewed range. |
| **[C-044]** `(reverse! arr)` | Reverse in place. Returns `arr`. On a view, reverses only the viewed range. |
| **[C-045]** `(shuffle! arr)` | Shuffle in place. Returns `arr`. |

- **[C-040a]** `push!` requires `start + length == capacity`. If not, it MUST signal an error. This prevents writing into slots that other views consider their own.

**Tables:**

| Primitive | Behavior |
|---|---|
| **[C-046]** `(get t key)` | Value at `key`, or `nil`. Works on structs and tables. |
| **[C-047]** `(put! t key value)` | Associate. Returns `t`. `t` MUST be a table. |
| **[C-048]** `(del! t key)` | Remove key. Returns `t`. `t` MUST be a table. |
| **[C-049]** `(has? t key)` | `true` iff key present. Works on structs and tables. |
| **[C-050]** `(keys t)` | Array of keys. Order unspecified. Works on structs and tables. |
| **[C-051]** `(values t)` | Array of values. Order unspecified. Works on structs and tables. |

### 10.7 Builders

These take any sequence (tuple, array, view, or `nil`) and build a **fresh mutable array**. They do not mutate their input. `nil` is treated as an empty sequence.

| Primitive | Behavior |
|---|---|
| **[C-052]** `(map f coll)` | Array of `(f x)` for each `x` in `coll`. Single collection only. |
| **[C-053]** `(filter pred coll)` | Array of elements `x` for which `(pred x)` is truthy. |
| **[C-054]** `(reduce f init coll)` | Left fold. Returns whatever `f` returns, not an array. `init` is required. |
| **[C-055]** `(each f coll)` | Calls `f` on each element for side effects. Returns `nil`. |
| **[C-056]** `(some pred coll)` | Returns the first truthy `(pred x)`, else `nil`. |
| **[C-057]** `(every? pred coll)` | Returns `true` iff `(pred x)` is truthy for every `x`. |
| **[C-058]** `(find pred coll)` | Returns the first `x` where `(pred x)` is truthy, else `nil`. |
| **[C-059]** `(take n coll)` | Array of the first `n` elements. |
| **[C-060]** `(concat a b...)` | Array of all elements of all arguments in order. |
| **[C-061]** `(reverse coll)` | Array of elements in reverse order. |
| **[C-062]** `(sort coll [cmp])` | Array of elements sorted by `cmp`, default `<`. |
| **[C-063]** `(zip a b...)` | Array of tuples, one per position. Length is the shortest input. |
| **[C-064]** `(range [start] end [step])` | Array of numbers from `start` (default 0) to `end` (exclusive) by `step` (default 1). |
| **[C-065]** `(array coll)` | Mutable array with the elements of `coll`. Identity if `coll` is already an array. |
| **[C-066]** `(tuple coll)` | Immutable tuple with the elements of `coll`. Identity if `coll` is already a tuple. |

- **[C-052a]** `map`, `filter`, `take`, `concat`, `reverse`, `sort`, `zip`, and `range` all return freshly allocated mutable arrays.
- **[C-052b]** The input to any of these MAY be a tuple, an array, or a view. The input's flags do not affect the result's flags.
- **[C-052c]** `reduce` and `each` return the values specified above, not arrays.
- **[C-065a]** `(array)` and `(tuple)` with no args return an empty sequence.
- **[C-066a]** The tuple produced by `(tuple coll)` has the **bracket** shape flag.

### 10.8 Standard Library (Partial)

Implementations MAY provide these. If they do, they MUST match these signatures.

| Function | Behavior |
|---|---|
| **[C-067]** `(apply f args)` | Calls `f` with elements of `args` as arguments. |
| **[C-068]** `(identity x)` | Returns `x`. |
| **[C-069]** `(complement f)` | Returns a function returning the logical negation of `(f ...)`. |
| **[C-070]** `(comp f g ...)` | Function composition, right to left. |
| **[C-071]** `(const x)` | Returns a function that always returns `x`. |

---

## 11. Limitations

- **[L-001]** **Mutual recursion grows the stack.** Two functions calling each other in tail position are not optimized. `recur` is self-only.
- **[L-002]** **Higher-order tail calls grow the stack.** Calling a function value in tail position is an ordinary call.
- **[L-003]** **No cross-function `recur`.** A `recur` can only target the nearest enclosing `fn`.
- **[L-004]** **Views can pin large buffers.** A small view keeps its entire source buffer alive. No automatic compaction.
- **[L-005]** **Structs and tables are not sequences.** `first`, `next`, `empty?` do not apply.
- **[L-006]** **No lazy sequences.** Every builder consumes its input fully and returns an array.
- **[L-007]** **Single-collection `map`.** No parallel `map` across multiple collections.
- **[L-008]** **No destructuring in `let` or `fn`.** Parameter and binding positions accept symbols only.
- **[L-009]** **No multi-arity `defn`.** A function has exactly one parameter list.
- **[L-010]** **Mutation of a shared buffer is visible through all views.** The spec does not prevent aliasing surprises; the `push!` restriction in §10.6 is a partial safeguard.

---

## 12. Open Questions

- `<!-- TODO: what happens when set! targets a var captured by a closure? -->`
- `<!-- TODO: does macroexpand fully expand or expand one level? -->`
- `<!-- TODO: are macros hygienic, partially hygienic, or unhygienic? -->`
- `<!-- TODO: is there a copy/compact operation to release a buffer pinned by a small view? -->`
- `<!-- TODO: specify quasiquote/unquote semantics. -->`
- `<!-- TODO: specify trampoline or equivalent for constant-stack mutual recursion. -->`
- `<!-- TODO: should map/filter/etc. accept nil, or should nil only be accepted by sequence access? -->`
- `<!-- TODO: how are runtime errors represented? Exception type? Panic? Result value? -->`
- `<!-- TODO: is (type 3) :number or :integer? Should integer and float be separate types? -->`
- `<!-- TODO: should type return :nil for nil, or nil (the value)? Clojure returns nil. -->`
- `<!-- TODO: should there be a full "describe" function returning type, flags, and shape in one call? -->`

---

## 13. Backlog

Ideas under consideration. Not part of the spec.

### 13.1 Buffers

Add `@"..."` as a mutable string literal, called a **buffer**. A buffer is to a string what an array is to a tuple.

- **Literal syntax:** `@"hello"` produces a buffer. `@` sigil before a string.
- **Type name:** `buffer`. `(type @"hello")` → `:buffer`.
- **Predicate:** `(buffer? v)`.
- **Symmetry:** immutability/mutability pairing would be complete: tuple/array, struct/table, string/buffer.
- **Conversion:** `(buffer str)` and `(string buf)`, both copying, with identity optimization if already the target type.
- **Sequences:** A buffer would be a sequence, so `first`, `next`, `length`, `slice`, `drop`, `get`, `push!`, `pop!`, `peek`, `sort!`, `reverse!`, `shuffle!` all work on it.
- **Mutation safety:** Same `start + length == capacity` rule as arrays for `push!`. Views over buffers share the buffer, so mutation through one is visible through all.
- **String-specific mutators:** `upcase!`, `downcase!`, `trim!`, and possibly `insert!` and `remove!`. Length-changing operations must be restricted to the end of the buffer or error on views.
- **Element type:** TBD. Options: bytes (like Janet), code points, graphemes, or one-character strings (Python-like). Affects `get`, `slice`, and iteration.
- **Unicode:** TBD. Whether strings and buffers are UTF-8 bytes, code points, or graphemes.
- **Slicing strings:** If strings are sequences, `(slice n m str)` would return a string view (immutable), consistent with tuples. If strings are not sequences, slicing is a copy.

### 13.2 String Operations (non-mutating)

If strings are sequences, these become natural additions:

- `(upcase str)`, `(downcase str)`, `(trim str)`
- `(split str sep)`, `(join sep coll)`
- `(replace str from to)`
- `(starts-with? str prefix)`, `(ends-with? str suffix)`, `(contains? str sub)`
- `(find str sub)`

### 13.3 Array Mutation on Views

The current rule is that `push!` requires `start + length == capacity`. This is a safety check, but it's not the only possible design.

- **Alternative A:** Allow `push!` on any mutable view; if there's no room, allocate a new buffer and detach. Silent copy, violates the "views don't copy" principle, but convenient.
- **Alternative B:** Disallow `push!` and `pop!` on any view at all. Simpler rule (`start == 0`), but disallows valid cases.
- **Alternative C:** Introduce explicit detach/compact operations the user calls before mutating. More verbose, but explicit about the copy.

Current choice is the "start + length == capacity" rule, documented in §10.6. Revisit later.

### 13.4 Macros and Hygiene

Currently unspecified whether macros are hygienic. Options:

- **Hygienic** (Scheme-style): requires renaming, complex to implement, robust.
- **Unhygienic** (Clojure-style): user manages capture with `gensym`.
- **Partially hygienic** (Janet-style): some auto-renaming, some manual.

For a first implementation, unhygienic with `gensym` is the simplest and most common choice. Revisit.

### 13.5 `macroexpand` Behavior

Does `macroexpand` expand once or fully? Clojure has both `macroexpand-1` and `macroexpand`. Current spec says "expand once." Could add a separate `macroexpand-all`.

### 13.6 Destructuring

Currently `let` and `fn` accept symbols only. Could allow vector-tuple patterns like `[a b c]` for positional destructuring and struct patterns for named destructuring. Adds power, adds complexity.

### 13.7 Multi-Arity `defn`

Currently `defn` takes one parameter list. Could allow multiple `([params] body...)` clauses, dispatched by arity. Useful but not essential.

### 13.8 Compact / Copy

An explicit operation to release a buffer pinned by a small view:

- `(copy coll)` — allocate a fresh buffer with just the viewed elements.
- `(compact coll)` — same, but only if the view doesn't span the whole buffer.

Would release `[L-004]`. Deferred.

### 13.9 Trampoline

For constant-stack mutual recursion, a library-level `trampoline` could be specified:

```
(trampoline f args...)
```

Where `f` returns a thunk instead of tail-calling directly. Deferred.

### 13.10 Error Representation

Currently unspecified. Options:

- **Exceptions** — `try`/`catch` special forms, throwable values.
- **Result values** — functions return `[:ok value]` or `[:err reason]`.
- **Panics** — fatal, no recovery.

For a first version, a simple exception mechanism is probably easiest. Deferred.
