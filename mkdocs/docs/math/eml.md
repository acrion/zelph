# Case Study: The EML Operator

Module: [`stdlib/eml.zph`](https://github.com/acrion/zelph/blob/main/stdlib/eml.zph)
· Prerequisite: [`symbolic-core`](symbolic.md)

In *All elementary functions from a single binary operator*
([arXiv:2603.21852](https://arxiv.org/abs/2603.21852)), Odrzywołek shows
that the single operator

\[ \mathrm{eml}(x, y) = \exp(x) - \ln(y) \]

together with the constant 1 generates the entire repertoire of a
scientific calculator — a Sheffer stroke for continuous mathematics, as
NAND is for Boolean logic.

This module makes `eml` a first-class symbolic operator. It is the standard
library's reference application of the **operator extension protocol**, and
its compiler is the reference application of **delegation**.

The NAND analogy is taken literally elsewhere in the standard library:
[`binary-nand-arithmetic`](arithmetic.md) derives every digit table of
binary arithmetic from a single NAND axiom. The two modules are deliberate
counterparts.

## The operator extension protocol

Three contributions turn any new operator into a citizen of the simplifier:

```
# (1) decompose
((U eml V) needssimp (U eml V)) => (U needssimp U)
((U eml V) needssimp (U eml V)) => (V needssimp V)

# (2) congruence
((U eml V) needssimp (U eml V), U simp P, V simp Q) => ((U eml V) red (P eml Q))

# (3) rewrites on reduced forms — the identity table
(T red (X eml &1)) => (T rw (exp of X))
(T red (&1 eml (exp of (&1 eml X)))) => (T rw (ln of X))
(T red (exp of &1)) => (T rw e)
```

[`symbolic-core`](symbolic.md)'s connect stage and identity fallback then
work unchanged. That is the whole interface — there is no interface.

## The identity table

The rewrites run in the **reduction** direction: they eliminate `eml` in
favour of named functions. The flagship is the paper's Eq. (5),
ln z = eml(1, eml(eml(1, z), 1)):

```
zelph> .import eml
zelph> x ~ symvar
zelph> ? :simplify (&1 eml ((&1 eml x) eml &1))
Answer: (:simplify (&1 eml ((&1 eml x) eml &1))) = (ln of x)
```

Not checked numerically, not assumed — *derived*, as a chain of ordinary
deductions by the same fixpoint engine that reasons over Wikidata, with the
full `⇐` provenance attached.

**Patterns match normalised forms.** A subtlety worth recording for anyone
writing an identity table: because simplification is bottom-up, the literal
Eq.-(5) tree never reaches the top-level rewrite stage intact — its inner
`(… eml &1)` has already been reduced to `(exp of …)` by the exp rule.
Table patterns must therefore be stated over bottom-up-normalised forms,
which is why the `ln` rule recognises `(&1 eml (exp of (&1 eml X)))`,
matched as a single deep condition by
[deep unification](../logic.md#deep-unification).

**Single-pass semantics.** Rewrite results are not re-processed within a
request, so e = eml(1, 1) reduces in two:

```
zelph> ? :simplify (&1 eml &1)
Answer: (:simplify (&1 eml &1)) = (exp of &1)
zelph> ? :simplify (exp of &1)
Answer: (:simplify (exp of &1)) = e
```

`simp` stays single-valued throughout; one-shot deep normalisation would be
the future iterated / e-graph design.

## The compiler

The expansion direction builds pure-EML forms bottom-up:

```
zelph> ? :emlcompile (exp of x)
Answer: (:emlcompile (exp of x)) = (x eml &1)
zelph> ? :emlcompile (ln of x)
Answer: (:emlcompile (ln of x)) = (&1 eml ((&1 eml x) eml &1))
```

Coverage follows the macro chain of the paper's reference compiler
(`eml_compiler_v4.py`, SI Sect. 2.1): `exp`, `ln`, `-`, `+`, `neg`, `inv`,
`*`, `/`, `eml` itself, and leaves.

The interesting part is *how*. Composite operators do not get their own
expansion rules — they **delegate** to their defining term:

```
# Negation: -z = ln(1) - z
((neg of V) needseml (neg of V))
=> (((ln of &1) - V) needseml ((ln of &1) - V))
((neg of V) needseml (neg of V), ((ln of &1) - V) emlform F)
=> ((neg of V) emlform F)
```

Each operator materialises its defining term as a graph node, marks it, and
harvests its `emlform`. The reference compiler's recursion becomes ordinary
fact flow, and the output trees are structurally identical to the Python
implementation's. The delegation DAG — `+` → {`-`, `neg`}, `neg` → {`-`,
`ln`}, `*` → {`exp`, `+`, `ln`}, `/` → {`*`, `inv`}, `inv` → {`exp`, `neg`,
`ln`} — is acyclic, so the cascade terminates.

One deliberate deviation: numerals stay opaque leaves. The paper's
`eml_int` double-and-add expansion would be representation-dependent, and
this module is not. Output is strictly pure EML whenever `&1` is the input's
only numeral. Unknown function symbols yield no `emlform` at all.

A structural bonus falls out of the engine: since all terms are
hash-consed, EML trees are canonical DAGs — repeated subexpressions are
stored, matched and rewritten once.

## Scope

Identities are **formal**. Principal-branch and domain caveats apply as in
[`symbolic-core`](symbolic.md#scope-and-honest-limitations): `exp inverseof
ln` assumes u > 0 over the reals, and side conditions are not tracked.

The operators the compiler adds (`-`, `neg`, `inv`) join only the compiler,
not the simplifier. Simplification support for `-` and `neg` arrived later,
in [`symbolic-minus`](symbolic.md#subtraction-and-negation).

`eml` is excluded from the self-fact display sugar: a fact like
`(&1 eml &1)` is a self-fact only through hash-consing — a coincidence of
value, not a request marker — and must render verbosely.

## Testing

`src/test/test_symbolic.cpp` covers the identity table and the compiler's
macro chain against all three arithmetic substrates, permanently in
`.semi-naive check` mode.
