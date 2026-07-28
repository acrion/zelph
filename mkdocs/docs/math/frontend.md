# The math Front End

Modules: [`stdlib/math.zph`](https://github.com/acrion/zelph/blob/main/stdlib/math.zph)
and [`stdlib/math-syntax.zph`](https://github.com/acrion/zelph/blob/main/stdlib/math-syntax.zph)

Two small modules that make the rest of the mathematical standard library
usable in three lines. `math` is the single import and the ring
declaration; `math-syntax` is the infix notation.

## `math` — one import and one declaration

```
zelph> .import math
math loaded: declare indeterminates with <x y z> ~ polyring
```

pulls in [`topoly`](topoly.md), [`math-syntax`](#notation),
[`symbolic-core`](symbolic.md), `symbolic-minus`, `symbolic-pow`,
`symbolic-integers` and [`diff`](symbolic.md#differentiation) — and, through
them, an arithmetic substrate, [`integer-arithmetic`](integers.md) and
[`polynomial`](polynomial.md).

### The ring declaration

```
zelph> <x y z> ~ polyring
```

The subject is a cons list of the ring's indeterminates, **outermost
first**. Four rules turn it into the declarations the polynomial layer
consumes:

```
(L ~ polyring) => (L needsring L)                              Trigger
((A cons R) needsring (A cons R), R != nil) => (R needsring R)  Decompose
((A cons R) needsring (A cons R)) => (A ~ symvar)               Sorts
((A cons (B cons S)) needsring (A cons (B cons S)))
=> (A pouter B)                                                 Order
```

so every element becomes an indeterminate and every **adjacent** pair fixes
the nesting order. Transitivity is already provided by
[`polynomial`](polynomial.md#variable-order), so adjacent pairs suffice:

```
zelph> %(string "ADJ-"   (and (zelph/exists "x" "pouter" "y") (zelph/exists "y" "pouter" "z")))
"ADJ-true"
zelph> %(string "TRANS-" (zelph/exists "x" "pouter" "z"))
"TRANS-true"
zelph> %(string "DIR-"   (zelph/exists "y" "pouter" "x"))
"DIR-false"
```

The order is directional; the reverse is not derivable.

Constants that should stay opaque are declared as usual with `~ symconst`
and do **not** belong in the list — though note that
[`topoly`](topoly.md#vocabulary) treats them as indeterminates too, so they
still need a place in the `pouter` order.

The decompose rule is guarded with `R != nil` for the same reason as
elsewhere in the standard library: no marker state is ever created on
`nil`, which is the graph's biggest hub.

### Why a list and not a Janet helper

The declaration is an ordinary fact about an ordinary node, so it is
visible to inference like everything else. Rules can quantify over rings,
further facts can attach to the same list node, and it survives `.save`. A
Janet helper would have produced the same facts and left nothing to reason
*about* — which would have contradicted the point of the system.

## Notation

`math-syntax` registers the `$( … )` **term island**: conventional infix
notation inside a statement that is otherwise ordinary zelph.

```
$( x^2 + 2*x + 1 ) diffby x
(T red $( X * 1 )) => (T rw X)        # node-identical to (X * &1)
```

An island desugars to exactly the graph structure the verbose syntax
builds. Hash-consing makes both spellings meet at identical nodes, so they
are freely mixable — in facts and in rules.

### Grammar

```
expr    := <one level per declared precedence, loosest first>
factor  := '-' factor | power
power   := primary ('^' INTEGER)?
primary := INTEGER | IDENT '(' expr ')' | IDENT | '(' expr ')'
```

| Form | Builds |
|---|---|
| `INTEGER` | `(zelph/number "…")`, i.e. the `&`-literal |
| `IDENT` | a zelph variable if variable-shaped (single uppercase letter, or leading `_`), otherwise a named node in the current language |
| `f(u)` | `(f of u)` — single argument only |
| `-u` | `(neg of u)`; with `symbolic-integers` loaded, `-3` promotes to `(neg zint &3)` |
| `t^n` | `(t ^ &n)`, `n ≥ 0` an integer literal. `^` is a term former, **not** sugar for a product |

Identifiers are `[A-Za-z_][A-Za-z0-9_]*`; atoms outside that charset need
the verbose syntax. Deliberate omissions: no implicit multiplication (`2x`
is an error, write `2*x`), no unquote inside islands, no comparison
operators, no string literals, no user-defined prefix or postfix operators.

Note that `x^2` and `x*x` are **different nodes**. Their equality is a
statement of the polynomial layer, not an assumption of the parser.

### Adding an operator

The infix levels of the grammar are *generated* from an operator table
that also feeds the display scheme — one table, so the parser and the
printer cannot drift apart. To extend the notation:

```
zelph> %(math-syntax/operator "circ" 15)
zelph> %(math-syntax/operator "**" 40 :right)
```

The built-ins are `+ -` at 10, `* /` at 20, `^` at 30. Associativity
defaults to `:left`.

Word-shaped operator names are matched with an identifier boundary, so
`circ` never matches inside `circle`; they need surrounding whitespace,
symbolic ones do not. Operators sharing a precedence must share an
associativity — mixing them is an error rather than a silent choice — and a
rejected table leaves neither the grammar nor the display registry changed.

Registering an operator for **display only**, with `zelph/set-infix-display`
on the `math-syntax` scheme, is possible but ill-advised: zelph would then
print island syntax its own parser refuses to read.

### Display

The scheme renders a term in island form only where the default rendering
would **deviate** — where precedence actually removes parentheses, or where
a numeral drops its sigil. Everything else keeps its ordinary form, which
is why one side of an answer often prints verbosely and the other as an
island.

`of` is registered in **application** form, so `(f of u)` reads back as
`f(u)`. That also covers unary minus: `$( -x )` builds `(neg of x)` and
renders as `neg(x)`, which this grammar parses. `=` stays unregistered — it
belongs to the arithmetic modules and has no place in this grammar, so a
result fact keeps `=` outside the island.

### Mechanics

The island is an [inline keyword](../janet.md) — the three-argument form of
`zelph/register-keyword`. The host's close-delimiter scan is raw, so the
handler arbitrates nested `)` via an `:incomplete` veto keyed on
parenthesis balance. Per the inline-keyword contract the handler is
side-effect-free until it accepts: the PEG parse runs first, graph
construction only afterwards. Balanced but unparsable content is an
**error**, never `:incomplete` — a veto there would swallow the surrounding
statement text.

## Testing

`src/test/test_math.cpp` covers the ring declaration;
`src/test/test_math_syntax.cpp` covers the grammar, precedence,
associativity, the operator extension and its error paths, across all three
arithmetic substrates.
