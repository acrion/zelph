# Why zelph for Mathematics

zelph is not a computer algebra system. It is a forward-chaining reasoning
engine over a semantic graph, built for knowledge bases — its two existing
applications are Wikidata and a legal ontology. There is no algebra in its
C++ core, not even syntactic sugar for it.

That sounds like a disqualification. It is the opposite, and this section
exists to show why.

## The core knows no mathematics — so you can watch it being built

In a conventional CAS, `expand((1+x)*(1-x))` is answered by code someone
wrote for exactly that purpose. You can read that code, but you cannot
interrogate it in the language you are using. The polynomial arithmetic is
below the waterline.

In zelph there is no waterline. Every layer of the following stack is
written in ordinary zelph rules, in files you can open, edit and reason
about:

| Layer | Module | What it contributes |
|---|---|---|
| Digits | `binary-arithmetic`, `decimal-arithmetic`, `binary-nand-arithmetic` | the digit-level truth tables — or, in the NAND variant, a *single* gate axiom from which they are derived |
| Positional arithmetic | `common-arithmetic` | `+ - * / mod cmp ^` as digit recursions over cons lists, base-agnostic |
| Integers | `integer-arithmetic` | ℤ as `(pos zint N)` / `(neg zint N)`, a thin façade over the naturals |
| Polynomials | `polynomial` | multivariate normal forms over ℤ: `padd`, `psub`, `pmul`, `pneg`, `ppow` |
| Terms | `symbolic-core` + `-minus`, `-pow`, `-integers` | a terminating simplifier, `diff` for derivatives |
| Compiler | `topoly` | symbolic term → canonical polynomial |
| Front end | `math`, `math-syntax` | `<x y> ~ polyring` and `$( … )` infix notation |


None of that is privileged. `+` is a node in the graph, exactly like
`is capital of`. A rule about polynomial coefficients and a rule about
Wikidata's `P361` are matched by the same unifier in the same fixpoint
loop.

## Three lines

```
zelph> .import math
zelph> <x> ~ polyring
zelph> ? $( (1+x)*(1-x) ) ≡ $( 1 - x^2 )
Answer: (((&1 + x) * (&1 - x)) ≡ $( &1 - x ^ &2 )) = proven
```

The second line is not a declaration in a configuration sense — it is an
ordinary fact stating that the list `<x>` is an instance of `polyring`.
Rules in [`math.zph`](frontend.md) turn it into the sort and ordering facts
the polynomial layer consumes. You could have written those facts yourself;
you could also write rules that quantify over *all* declared rings, because
the declaration is visible to inference like everything else.

Scale costs remarkably little. Euler's four-square identity, eight
indeterminates, degree four on both sides:

```
zelph> <a1 a2 a3 a4 b1 b2 b3 b4> ~ polyring
zelph> ? $( (a1^2+a2^2+a3^2+a4^2) * (b1^2+b2^2+b3^2+b4^2) ) ≡ $( (a1*b1-a2*b2-a3*b3-a4*b4)^2 + (a1*b2+a2*b1+a3*b4-a4*b3)^2 + (a1*b3-a2*b4+a3*b1+a4*b2)^2 + (a1*b4+a2*b3-a3*b2+a4*b1)^2 )
Answer: … = proven
```

on the order of two tenths of a second, cold start included.

## Proof by node identity

`≡` does not compare strings and does not evaluate at sample points. Both
sides are compiled to a canonical polynomial normal form, and because every
node in zelph is hash-consed — one value, one node — *equality of normal
forms is identity of nodes*. The proof obligation collapses to a pointer
comparison, and the rule that performs it is a single line of
[`topoly.zph`](topoly.md):

```
(A ≡ B, (:topoly A) = P, (:topoly B) = P) => ((A ≡ B) = proven)
```

The same variable `P` in both conditions is the entire argument. Unification
binds it only if both compilations arrived at the identical node.

## Every answer carries its proof

Nothing above is a black box, and you do not have to take the verdict on
trust. `.explain` reconstructs the justification from the saturated graph:

```
zelph> .explain 3
($( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))) = proven
   ├─ $( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))  [axiom]
   ├─ (:topoly ((x - &1) * (x + &1))) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)
   │  ├─ ((x - &1) * (x + &1)) aspoly (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)
   …
   └─ (:topoly $( x ^ &2 - &1 )) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)
```

Read the normal form: `(x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)`
is the coefficient list −1, 0, 1 of the polynomial in `x` — that is
x² − 1, written the way the machine holds it. Both sides reach it. The
tree continues down to the digit level if you ask it to.

## What the generality buys you

Because nothing is specialised, extending the system is not "writing a
plug-in" — it is stating facts.

Two facts give you hyperbolic functions, chain rule included:

```
zelph> sinh hasderivative cosh
zelph> cosh hasderivative sinh
zelph> ? $( cosh(x*x) ) diffby x
Answer: ($( cosh(x * x) ) diffby x) = $( sinh(x * x) * (x + x) )
```

There is no table of known functions in the engine. `hasderivative` is an
ordinary predicate, consumed by one generic rule, and `sinh` is an ordinary
node — the same kind of node as `Berlin`.

And because the substrate is interchangeable, the *same* symbolic layer runs
over decimal digits, binary digits, or binary digits synthesised from a
single NAND axiom. The test suite exercises every symbolic test against all
three.

## Honest scope

zelph is not competing with Singular or Macaulay2 on Gröbner bases, and it
does not have real or complex numbers, quotient fields, or side conditions
on identities. What it has is a complete, inspectable chain from a Boolean
gate to a multivariate polynomial identity, in one uniform formalism, with
provenance at every step — and enough performance that this is a working
tool rather than a demonstration. The
[Jacobian tutorial](tutorial-jacobian.md) verifies a July-2026
counterexample to a sixty-year-old conjecture, over ℤ, in under a second.

## Where to go next

| If you want to | Start at |
|---|---|
| prove your first identity | [1 · Proving Identities](tutorial-identities.md) |
| see arithmetic emerge from a single logic gate | [2 · Numbers from Nothing](tutorial-numbers.md) |
| build terms, simplify them, add your own operator | [3 · Terms and Rewriting](tutorial-terms.md) |
| differentiate, and extend differentiation | [4 · Differentiation](tutorial-differentiation.md) |
| understand the polynomial representation | [5 · Inside the Normal Form](tutorial-polynomials.md) |
| see the whole stack at work | [6 · Refuting the Jacobian Conjecture](tutorial-jacobian.md) |
| look up a predicate | [Module and Predicate Index](reference.md) |

