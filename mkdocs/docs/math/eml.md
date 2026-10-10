# Case Study: The EML Operator

Module: [`stdlib/eml.zph`](https://github.com/acrion/zelph/blob/main/stdlib/eml.zph)
· Prerequisites: [`symbolic-core`](symbolic.md), [`symbolic-minus`](symbolic.md#subtraction-and-negation)

In *All elementary functions from a single binary operator*
([arXiv:2603.21852](https://arxiv.org/abs/2603.21852)), Odrzywołek shows
that the single operator

\[ \mathrm{eml}(x, y) = \exp(x) - \ln(y) \]

together with the constant 1 generates the entire repertoire of a
scientific calculator — a Sheffer stroke for continuous mathematics, as
NAND is for Boolean logic.

This module causes `eml` to become a first-class symbolic operator, established through its **definition** rather than through rewrite rules. It serves as the standard library’s reference application of **definitional extension**, and its compiler stands as the reference application of **delegation**.

The NAND analogy is taken literally in another part of the standard library: [`binary-nand-arithmetic`](arithmetic.md) derives every digit table in binary arithmetic starting from a single NAND axiom and the digit alphabet. The two modules are intentional counterparts.

## The operator extension protocol

An operator typically joins the simplifier through three contributions: decompose rules that pass the `needssimp` marker on to its operands, a congruence rule responsible for constructing its reduced form, and rewrite rules applied to reduced forms. `eml` keeps the first and replaces the remaining two with its definition:

```
# (1) decompose
((U eml V) needssimp (U eml V)) => (U needssimp U)
((U eml V) needssimp (U eml V)) => (V needssimp V)

# (2) the definition eml(x, y) = exp(x) - ln(y), over the operands' normal forms
((U eml V) needssimp (U eml V), U simp P, V simp Q)
=> ((U eml V) expandsto ((exp of P) - (ln of Q)))
```

The rules SE of [`symbolic-core`](symbolic.md#definitional-extension) complete the remaining tasks:

```
(T expandsto R) => (R needssimp R)
(T expandsto R, R simp S) => (T simp S)
```

The right-hand side is simplified as an ordinary term, and its normal form serves as the normal form of the `eml` term. An `eml` term does not possess a reduced form of its own, thus the identity fallback is never triggered for it, and no normal form contains `eml`. The contract for a defined operator is short: the right-hand side must not contain the operator itself, and it is built from the operands' normal forms. That is the whole interface – there is no interface.

## The identity table

No identity described in the paper functions as a **rule**. Instead, each constitutes a **result** derived from the definition, together with the formal identities that the simplifier recognizes for `exp`, `ln`, and `-` (specifically, ln 1 = 0, the inverse pair, X − 0 = X, X − X = 0, and x − (x − a) = a), and the definition of e provided below:

| Identity | Input | Expanded and simplified | Result |
|---|---|---|---|
| exp(x) = eml(x, 1) | `(x eml &1)` | exp(x) − ln 1 = exp(x) − 0 | `(exp of x)` |
| e = eml(1, 1) | `(&1 eml &1)` | exp(1) − ln 1 = e − 0 | `e` |
| ln z = eml(1, eml(eml(1, z), 1)), Eq. (5) | `(&1 eml ((&1 eml x) eml &1))` | e − ln(exp(e − ln x)) = e − (e − ln x) | `(ln of x)` |
| 0 = ln 1 = eml(1, eml(eml(1, 1), 1)) | `(&1 eml ((&1 eml &1) eml &1))` | e − ln(exp(e)) = e − e | `&0` |
| x − y = eml(ln x, exp y) | `((ln of x) eml (exp of y))` | exp(ln x) − ln(exp y) | `(x - y)` |

Simplification runs in the **reduction** direction: it removes `eml` by substituting it with named functions, and every outcome requires a single request:

```
zelph> .import eml
zelph+> x ~ symvar
zelph> y ~ symvar
zelph> ? :simplify (&1 eml ((&1 eml x) eml &1))
Answer: (:simplify (&1 eml ((&1 eml x) eml &1))) = (ln of x)
zelph+> ? :simplify (&1 eml &1)
Answer: (:simplify (&1 eml &1)) = e
zelph+> ? :simplify (&1 eml ((&1 eml &1) eml &1))
Answer: (:simplify (&1 eml ((&1 eml &1) eml &1))) = &0
zelph+> ? :simplify ((ln of x) eml (exp of y))
Answer: (:simplify ((ln of x) eml (exp of y))) = (x - y)
```

Not verified through numerical means, nor taken for granted – *derived*, through a chain of ordinary deductions processed by the identical fixpoint engine responsible for reasoning over Wikidata. zelph keeps no record of this chain: `.explain` rebuilds it from the graph later (refer to [Looking at the proof](tutorial-identities.md#looking-at-the-proof)).

**A single request.** Rewriting takes place in a single pass: the outcome of a rewrite is not re-evaluated within the same request. A definition does not function as a rewrite. Its right-hand side is marked as a new term and simplified bottom-up, just like any other term, thereby attaining its normal form in the same request as the `eml` term. Up to version 1.0.1, which encoded identities as rewrite rules, e = eml(1, 1) stopped at `(exp of &1)` and needed a second request, and eml(ln x, 1) stopped at `(exp of (ln of x))`. Both now return fully resolved outcomes in a single request:

```
zelph+> ? :simplify ((ln of x) eml &1)
Answer: (:simplify ((ln of x) eml &1)) = x
```

**e is defined as exp(1).** To the simplifier, the constant `e` remains opaque (`e ~ symconst`), and the rewrite from `(exp of &1)` to `e` serves as its definition, not as an identity. The equation ln e = 1 represents the same definition expressed in reverse, and this is needed: congruence turns `(ln of (exp of &1))` into `(ln of e)` before the inverse-pair rule is able to detect the `exp`, so without this, ln(exp(1)) would stop at `(ln of e)`. The [compiler](#the-compiler) expands `e` using the same definition: it compiles to `(&1 eml &1)`, which is the output the reference compiler emits for E.

```
e ~ symconst
(T red (exp of &1)) => (T rw e)
(T red (ln of e)) => (T rw &1)
```

```
zelph+> ? :simplify (ln of (exp of &1))
Answer: (:simplify (ln of (exp of &1))) = &1
zelph+> ? :simplify (e - e)
Answer: (:simplify (e - e)) = &0
```

`simp` stays single-valued. [`symbolic-core`](symbolic.md) states this as the rule `(T simp A, T simp B, A != B) => !`, which reports a second answer as a contradiction by naming both answers, and every one of the 36 `eml` trees with depth no greater than two, constructed from 1 and x, simplifies to exactly one answer.

**Cost.** A request demands greater effort than it did with the rewrite rules in version 1.0.1: every `eml` level builds its right-hand side as a new term and marks the normal forms of its operands anew.

## The compiler

The expansion direction builds pure-EML forms bottom-up:

```
zelph+> ? :emlcompile (exp of x)
Answer: (:emlcompile (exp of x)) = (x eml &1)
zelph+> ? :emlcompile (ln of x)
Answer: (:emlcompile (ln of x)) = (&1 eml ((&1 eml x) eml &1))
```

Coverage follows the macro chain defined by the paper’s reference compiler (`eml_compiler_v4.py`; macros from SI Sect. 2.1, along with division x/y expressed as x·(1/y), the compiler’s `eml_div`, SI Sect. 2.5, item 7): `exp`, `ln`, `-`, `+`, `neg`, `inv`, `*`, `/`, `eml` itself, the constant `e`, and leaves.

The interesting part is *how*. Composite operators do not get their own
expansion rules — they **delegate** to their defining term:

```
# Negation: -z = ln(1) - z
((neg of V) needseml (neg of V))
=> (((ln of &1) - V) needseml ((ln of &1) - V))
((neg of V) needseml (neg of V), ((ln of &1) - V) emlform F)
=> ((neg of V) emlform F)
```

Each operator materializes its defining term as a graph node, marks it, and harvests its `emlform`. The reference compiler’s recursion turns into standard fact flow, and the output trees match structurally the ones that the reference compiler’s emit primitives (`eml_exp`, `eml_log`, `eml_sub`, `eml_neg`, `eml_add`, `eml_inv`, `eml_mul`, `eml_div`) build. The delegation DAG – `e` → {`exp`}, `+` → {`-`, `neg`}, `neg` → {`-`, `ln`}, `*` → {`exp`, `+`, `ln`}, `/` → {`*`, `inv`}, `inv` → {`exp`, `neg`, `ln`} – contains no cycles, ensuring the cascade terminates.

There are two intentional departures from the reference implementation. First, zelph compiles the expression exactly as it appears, whereas the reference program transforms the input into SymPy’s canonical form before compilation. SymPy represents x − y as x + (−1)·y, −x as (−1)·x, 1/x as x⁻¹, and x / y as x·y⁻¹, causing the program to generate larger expression trees for `-`, `neg`, `inv`, and `/` ([see below](#compile-then-simplify), where x − y expands to a size of 83), while zelph compiles each of these operations directly via their native primitives. For `exp`, `ln`, `+`, and `*`, the two approaches align only when SymPy preserves the original structure, but SymPy also reorders operands (e.g., y + x becomes x + y), combines identical terms (x·x becomes x², x + x becomes 2·x), and evaluates exp(ln x) to x. Second, numerals remain opaque leaves. The paper’s `eml_int` double-and-add expansion would depend on representation, which this module avoids. Other declared constants besides `e` are also kept as leaves, consistent with how SymPy Symbols behave. The output remains strictly pure EML whenever `&1` is the sole numeral present in the input. Unknown function symbols yield no `emlform` whatsoever.

A structural advantage arises from the engine: since each term is hash-consed, EML trees become maximally shared DAGs – repeated subexpressions are stored, matched, and reduced only once. The sharing is based on syntax: the structures representing `(x * y)` and `(y * x)` are equal in value and persist as two distinct nodes.

### Compile, then simplify

Simplifying a compiled form takes it back out of EML. Regarding subtraction, which forms the foundation of the macro chain, the round trip returns the initial input:

```
zelph+> ? :emlcompile (x - y)
Answer: (:emlcompile (x - y)) = ((&1 eml ((&1 eml x) eml &1)) eml (y eml &1))
zelph+> ? :simplify ((&1 eml ((&1 eml x) eml &1)) eml (y eml &1))
Answer: (:simplify ((&1 eml ((&1 eml x) eml &1)) eml (y eml &1))) = (x - y)
```

The remaining operators come back equal in value, though expressed using subtraction. Each row illustrates the outcome produced by the simplifier from the compiled form of the term:

| Term | Its compiled form, simplified |
|---|---|
| `(x + y)` | `(x - (&0 - y))` |
| `(x * y)` | `(exp of ((ln of x) - (&0 - (ln of y))))` |
| `(x / y)` | `(exp of ((ln of x) - (ln of y)))` |
| `(neg of x)` | `(&0 - x)` |
| `(inv of x)` | `(exp of (&0 - (ln of x)))` |

The `&0 - y` expressions stem from the compiler’s negation template, −z = ln(1) − z. The compiled ln(1), eml(1, eml(eml(1, 1), 1)), simplifies to `&0` (the fourth row of the identity table), and the subtraction persists: inside the naturals, 0 − y yields no value, and no rule rewrites `(&0 - y)` into `(neg of y)`. Loading `symbolic-integers` preserves these expressions unchanged. In x / y, the two negations meet and cancel via the identity x − (x − a) = a.

The reference compiler `eml_compiler_v4` works on SymPy expressions, where x − y is represented as the sum x + (−1 · y). It transforms this sum into a tree with a size of 83 (LeafCount), expressed as `add('x', mul(neg('1'), 'y'))` within the primitives defined in `dev_scripts/paper/eml_dag_stats.py`; zelph’s compiler generates an identical tree from `(x + ((neg of &1) * y))`. When simplified, the resulting tree gives

```
(x - (&0 - (exp of ((ln of (&0 - &1)) - (&0 - (ln of y))))))
```

It becomes trapped at the logarithm of `(&0 - &1)`, which does not fold over the naturals. When `symbolic-integers` is loaded, `(&0 - &1)` folds to `(neg zint &1)` instead, and the outcome retains `(ln of (neg zint &1))` in its place.

## Scope

Identities are **formal**. Principal-branch and domain caveats apply as in [`symbolic-core`](symbolic.md#scope-and-honest-limitations): `exp inverseof ln` assumes u > 0 over the reals, and side conditions are not tracked. When simplifying a compiled negation, for example, the process goes through ln(ln 1) = ln 0 and uses exp(ln 0) = 0, which is valid solely as a formal identity.

Among the operators processed by the compiler, beyond those from `symbolic-core`, the operations `-` and `neg` are simplified by [`symbolic-minus`](symbolic.md#subtraction-and-negation), a module this one imports since the definition of `eml` is a difference; `inv` is governed solely by rules within the compiler.

`eml` is excluded from the self-fact display sugar: a fact like
`(&1 eml &1)` is a self-fact only through hash-consing — a coincidence of
value, not a request marker — and must render verbosely.

## Testing

`src/test/test_symbolic.cpp` covers the results of the definition, the forms to which compiled terms reduce, a sweep across all 36 `eml` trees with depth no greater than two, and the compiler’s macro chain, across all three arithmetic substrates, consistently operating in `.semi-naive check` mode.
