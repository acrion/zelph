# 1 · Proving Identities

*Prerequisites: a built `zelph` binary — see the [Quick Start Guide](../quickstart.md).
Nothing else. This page assumes no zelph knowledge.*

## Three lines

Start the REPL and type:

```
zelph> .import math
math loaded: declare indeterminates with <x y z> ~ polyring
zelph> <x> ~ polyring
(:needsring <x>) ⇐ (<x> ~ polyring)
zelph> ? $( (1+x)*(1-x) ) ≡ $( 1 - x^2 )
Answer: (((&1 + x) * (&1 - x)) ≡ $( &1 - x ^ &2 )) = proven
```

That is a complete session. Three lines, and the last one is the theorem.

Take the lines in turn.

**`.import math`** pulls in the whole mathematical standard library — an
arithmetic substrate, signed integers, polynomial normal forms, a symbolic
term layer, differentiation, and the infix notation used below. It is a
convenience: every module can also be imported on its own, and
[the reference](reference.md) lists what each contributes.

**`<x> ~ polyring`** declares the indeterminates. `<…>` is zelph's list
syntax and `~` reads *is an instance of*, so this is a plain fact: the list
`<x>` is an instance of `polyring`. It is not a directive to the parser —
it is knowledge, and the echo shows a rule already reacting to it. With
several indeterminates you write them in one list, outermost first:

```
zelph> <x y z> ~ polyring
```

**`? …`** is the result-query prefix. It materialises the statement, runs
inference to the fixpoint quietly, and then reports what the graph now
knows about it. Without the `?` you would enter the statement, watch the
deductions scroll past, and query the result separately; `?` is the
one-line form.

**`≡`** asks whether two terms are the same polynomial. Not "equal at the
points I tried" and not "textually equal after expansion" — the same
element of ℤ[x].

## The notation

`$( … )` is a *term island*: conventional infix notation with the usual
precedence, spelled inside a statement that is otherwise ordinary zelph.
Inside an island you may write `+ - * / ^`, function application `f(u)`,
unary minus, integer literals, and parentheses.

Islands are pure surface. `$( 1 - x^2 )` builds exactly the same graph
structure as the verbose form `(&1 - (x ^ &2))`, and because zelph
hash-conses every node, the two spellings *meet at the identical node*.
You can mix them freely — the answer line above does exactly that, printing
one side verbose and one side as an island, because zelph only reaches for
island notation where precedence actually lets it drop parentheses.

The `&` in `&1` marks a numeral. It is not decoration: it distinguishes the
*number* 1 from a node that happens to be called `1`. You do not type it
inside islands, but you will see it in output.

## A gallery

Everything below is a complete session after `.import math`. Timings are
wall-clock on a laptop, cold start included.

**Sophie Germain's identity** — the classic factorisation that shows
x⁴ + 4y⁴ is composite for every y > 1:

```
zelph> <x y> ~ polyring
zelph> ? $( x^4 + 4*y^4 ) ≡ $( (x^2 + 2*y^2 - 2*x*y) * (x^2 + 2*y^2 + 2*x*y) )
Answer: ($( x ^ &4 + &4 * y ^ &4 ) ≡ $( (x ^ &2 + &2 * y ^ &2 - &2 * x * y) * (x ^ &2 + &2 * y ^ &2 + &2 * x * y) )) = proven
```

**The Brahmagupta–Fibonacci identity** — the product of two sums of two
squares is a sum of two squares, which is why the Gaussian integers are
multiplicative:

```
zelph> <a b c d> ~ polyring
zelph> ? $( (a^2 + b^2) * (c^2 + d^2) ) ≡ $( (a*c - b*d)^2 + (a*d + b*c)^2 )
Answer: … = proven
```

**Euler's four-square identity** — the quaternionic analogue, eight
indeterminates:

```
zelph> <a1 a2 a3 a4 b1 b2 b3 b4> ~ polyring
zelph> ? $( (a1^2+a2^2+a3^2+a4^2) * (b1^2+b2^2+b3^2+b4^2) ) ≡ $( (a1*b1-a2*b2-a3*b3-a4*b4)^2 + (a1*b2+a2*b1+a3*b4-a4*b3)^2 + (a1*b3-a2*b4+a3*b1+a4*b2)^2 + (a1*b4+a2*b3-a3*b2+a4*b1)^2 )
Answer: … = proven
```

**A cyclotomic factorisation:**

```
zelph> <x> ~ polyring
zelph> ? $( x^6 - 1 ) ≡ $( (x-1)*(x+1)*(x^2+x+1)*(x^2-x+1) )
Answer: … = proven
```

**The binomial theorem, instantiated:**

```
zelph> <x y> ~ polyring
zelph> ? $( (x+y)^5 ) ≡ $( x^5 + 5*x^4*y + 10*x^3*y^2 + 10*x^2*y^3 + 5*x*y^4 + y^5 )
Answer: … = proven
```

## What "proven" means here

It is worth being precise, because the mechanism is unusually simple.

Both sides are compiled to a **canonical normal form**: a recursive, dense,
variable-tagged representation of a multivariate polynomial over ℤ. Two
polynomials are equal exactly when their normal forms are equal. And since
every node in zelph is hash-consed, equal normal forms are *the same node*.

So the whole proof rule is:

```
(A ≡ B, (:topoly A) = P, (:topoly B) = P) => ((A ≡ B) = proven)
```

Read it as: if `A` compiles to `P`, and `B` compiles to *the same* `P` —
the same variable, so unification demands the identical node — then the
identity holds. There is no equality checker to trust. The
[normal-form tutorial](tutorial-polynomials.md) opens the representation
up; here it is enough that it is canonical.

## Looking at the proof

`.explain` reconstructs the justification of the last answer. It is a
backward search over the saturated graph — zelph records no provenance
during inference, so this is genuinely reconstructed, not replayed.

```
zelph> <x> ~ polyring
zelph> ? $( x^2 - 1 ) ≡ $( (x-1)*(x+1) )
Answer: ($( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))) = proven
zelph> .explain 3
($( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))) = proven
   ├─ $( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))  [axiom]
   ├─ (:topoly $( x ^ &2 - &1 )) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)
   │  ├─ :topoly $( x ^ &2 - &1 )
   │  │  └─ $( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))  [axiom]
   │  └─ $( x ^ &2 - &1 ) aspoly (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)
   │     ├─ :needstopoly $( x ^ &2 - &1 )  … [depth limit -- use '.explain <pattern> 0' for the full proof]
   │     ├─ ((x poly <(pos zint &0) (pos zint &0) (pos zint &1)>) psub (pos zint &1)) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)  … [depth limit …]
   │     ├─ &1 aspoly (pos zint &1)  … [depth limit …]
   │     └─ (x ^ &2) aspoly (x poly <(pos zint &0) (pos zint &0) (pos zint &1)>)  … [depth limit …]
   └─ (:topoly ((x - &1) * (x + &1))) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)
      ├─ :topoly ((x - &1) * (x + &1))
      │  └─ $( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))  [axiom]
      └─ ((x - &1) * (x + &1)) aspoly (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)
         ├─ :needstopoly ((x - &1) * (x + &1))  … [depth limit …]
         ├─ (x - &1) aspoly (x poly <(neg zint &1) (pos zint &1)>)  … [depth limit …]
         ├─ (x + &1) aspoly (x poly <(pos zint &1) (pos zint &1)>)  … [depth limit …]
         └─ ((x poly <(neg zint &1) (pos zint &1)>) pmul (x poly <(pos zint &1) (pos zint &1)>)) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)  … [depth limit …]
```

Three things are worth noticing.

The only leaf marked `[axiom]` is the statement you typed. Everything else
was derived.

Both branches end at the *same* normal form,
`(x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)` — a polynomial in
`x` with coefficient list −1, 0, 1, least significant first. That is
x² − 1 as the machine holds it, and it is why the proof works.

The tree is cut at depth 3. `.explain <pattern> 0` prints it in full, all
the way down to the digit-level arithmetic that multiplied the
coefficients. Nothing is hidden below a native implementation, because
there is no native implementation.

## Three answers, one of them silence

A false identity is answered, not merely left out:

```
zelph> <x> ~ polyring
zelph> ? $( (x+1)^2 ) ≡ $( x^2 + 1 )
Answer: (((x + &1) ^ &2) ≡ $( x ^ &2 + &1 )) = disproven
```

`disproven` needs no cleverness. Both sides compiled, to ⟨1, 2, 1⟩ and
⟨1, 0, 1⟩, and those are different nodes — so the verdict follows from the
same node identity that gives `proven`, just the other way round.

The third answer is silence, and it means something specific:

```
zelph> .import math
zelph> ? $( (1+x)*(1-x) ) ≡ $( 1 - x^2 )
zelph>
```

Here `<x> ~ polyring` is missing, so `x` has no sort, neither side compiles,
and there is nothing to compare. zelph does *not* report that as
`disproven` — "I could not compile this" is not "these differ". Throughout
the standard library, *partiality is expressed by absence*, so that a
missing premise can never turn into a wrong verdict.

When you get silence, ask for the normal forms directly to see which side
failed:

```
zelph> ? :topoly $( (x+1)^2 )
Answer: (:topoly ((x + &1) ^ &2)) = (x poly <(pos zint &1) (pos zint &2) (pos zint &1)>)
zelph> ? :topoly $( x^2 + 1 )
Answer: (:topoly $( x ^ &2 + &1 )) = (x poly <(pos zint &1) (pos zint &0) (pos zint &1)>)
```

`:topoly T` is the same request the `≡` rule makes internally. The leading
colon is zelph's shorthand for a self-fact — `:topoly T` is `(T topoly T)`
— which is the standard-library idiom for "please compute this".

## Exercises

1. Prove the Lagrange identity
   (a²+b²)(c²+d²) − (ac+bd)² ≡ (ad−bc)². You will need to state it in the
   form `A ≡ B`; `≡` takes two terms, so move the subtraction to one side.
2. Prove that x⁵ − 1 factors as (x−1)(x⁴+x³+x²+x+1), then use
   `? :topoly …` to read off the normal form of each factor.
3. `? :topoly $( 2^10 )` — what comes back, and why is it not a polynomial?
4. Declare `<x y> ~ polyring` and then ask for `? :topoly $( y*x )`. Now
   try a session where you declare `<y x> ~ polyring` instead. The normal
   forms differ. What does the declaration order mean? (Answer:
   [Polynomial Normal Forms](polynomial.md#variable-order).)

## Next

[2 · Numbers from Nothing](tutorial-numbers.md) goes underneath everything
used here: how `&1` and `&512` are represented, and how addition,
multiplication and comparison are derived — in one case from a single NAND
axiom.
