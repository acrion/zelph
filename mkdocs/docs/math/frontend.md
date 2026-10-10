# The math Front End

Modules: [`stdlib/math.zph`](https://github.com/acrion/zelph/blob/main/stdlib/math.zph)
and [`stdlib/math-syntax.zph`](https://github.com/acrion/zelph/blob/main/stdlib/math-syntax.zph)

Two small modules that make the rest of the mathematical standard library
usable in three lines. `math` is the single import and the ring
declaration; `math-syntax` is the infix notation.

## `math` — one import and one declaration

```
zelph> .import math
math-syntax loaded: $( ... ) term islands (infix with precedence)
math loaded: declare indeterminates with <x y z> ~ polyring
```

pulls in [`topoly`](topoly.md), [`math-syntax`](#notation),
[`symbolic-core`](symbolic.md), `symbolic-minus`, `symbolic-pow`,
`symbolic-integers` and [`diff`](symbolic.md#differentiation) — and, through
them, an arithmetic substrate, [`integer-arithmetic`](integers.md) and
[`polynomial`](polynomial.md).

### The ring declaration

```
zelph+> <x y z> ~ polyring
(:needsring <x y z>) ⇐ (<x y z> ~ polyring)
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
zelph+> %(string "ADJ-"   (and (zelph/exists "x" "pouter" "y") (zelph/exists "y" "pouter" "z")))
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
expr    := <one level per declared precedence below 30, loosest first>
factor  := '-' factor | power
power   := tight ('^' factor)?
tight   := <one level per declared precedence above 30, loosest first>
primary := INTEGER | NAME '(' expr ')' | NAME | '(' expr ')'
NAME    := IDENT | '"' IDENT '"'
```

The tightest level of `expr` reads factors, the tightest level of `tight` reads primaries; with no operator declared above 30, `tight` equates to `primary`. An operand that comes after an operator of `tight` can also be `'-' factor`.

| Form | Builds |
|---|---|
| `INTEGER` | `(zelph/number "…")`, specifically the `&`-literal |
| `IDENT` | a zelph variable when shaped like a variable (single uppercase letter, or prefixed with `_`), otherwise a named node within the current language |
| `"IDENT"` | always a named node; this is the way the printer writes a node whose name follows variable shape, like `"A"` or `"_k"` |
| `f(u)` | `(f of u)` – only one argument permitted |
| `-u` | `(neg of u)`; when `symbolic-integers` is loaded, `-3` promotes to `(neg zint &3)` |
| `t^u` | `(t ^ u)`, applicable to any factor `u`: `x^y^z` becomes `(x ^ (y ^ z))`, `x^-1` becomes `(x ^ (neg of &1))`. The `^` symbol functions as a term former, **not** as syntactic sugar for multiplication |

Identifiers are `[A-Za-z_][A-Za-z0-9_]*`; atoms outside that charset need
the verbose syntax. Deliberate omissions: no implicit multiplication (`2x`
is an error, write `2*x`), no unquote inside islands, no comparison
operators, no string literals, no user-defined prefix or postfix operators.

Note that `x^2` and `x*x` are **different nodes**. Their equality is a
statement of the polynomial layer, not an assumption of the parser.

### Adding an operator

The precedence levels within the grammar are *generated* from an operator table that simultaneously supplies the display scheme – a single source, ensuring the parser and the printer remain synchronized, and guaranteeing that whatever the island writes, it reads back correctly. To extend the notation:

```
zelph> %(math-syntax/operator "circ" 15)
zelph> %(math-syntax/operator "**" 40 :right)
```

The built-in operators `+ -` are assigned level 10, `* /` are at level 20, and `^` is at level 30; all except `^` are left-associative, and `:left` serves as the default associativity for any newly declared operator. The unary minus operator occupies a position between `^` and the lower levels: it binds more tightly than any level below 30, and less tightly than `^` and any operator declared at a level above 30, meaning that `-x^2` is `(neg of (x ^ &2))`. A sign operator binds the same way when following an operator declared at a level above 30: with `**` set at level 40, the expression `a ** -b ** c` becomes `(a "**" (neg of (b "**" c)))`. An operator declared at level 30 is integrated into the same level as `^` and must be specified as `:right`.

A name that a statement has to quote is printed within quotation marks inside an island too, and is then read back by the island. For an operator like `**`, either form is acceptable: `$( a "**" b + c )` equates to `$( a ** b + c )`. For a leaf or a call head, the quotation marks prevent a node named `A` or `_k` from being interpreted as a variable upon reading back.

Word-shaped operator names are matched with an identifier boundary, so
`circ` never matches inside `circle`; they need surrounding whitespace,
symbolic ones do not. Operators sharing a precedence must share an
associativity — mixing them is an error rather than a silent choice — and a
rejected table leaves neither the grammar nor the display registry changed.

Registering an operator for **display only**, with `zelph/set-infix-display`
on the `math-syntax` scheme, is possible but ill-advised: zelph would then
print island syntax its own parser refuses to read.

### Display

The scheme produces an island-form representation of a term only when the default rendering would **deviate** – specifically, when precedence genuinely eliminates parentheses, or when an application prints in call notation (refer to `of` below). Numerals keep their `&`, ensuring a numeral never opens an island by itself. All other elements maintain their ordinary form, which explains why one side of a result frequently appears verbose while the other appears as an island.

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
