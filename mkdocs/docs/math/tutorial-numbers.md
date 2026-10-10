# 2 · Numbers from Nothing

*Prerequisites: [1 · Proving Identities](tutorial-identities.md).*

The previous tutorial proved polynomial identities without once asking what
a number *is* in zelph. This one goes underneath. The answer is unusual and
it matters: there is no integer type in the engine, no arithmetic in C++,
and — in one of the three substrates — no arithmetic table either. Just one
logic gate.

## A numeral is a list

Type a literal and zelph prints it back:

```
zelph> .import decimal-arithmetic
zelph+> ? &123456789 * &987654321
Answer: (&123456789 * &987654321) = &121932631112635269
```

The `&` prefix is input sugar for a **cons list of digit nodes**, stored
least-significant first. Nothing else. You can verify it directly:

```
zelph+> %(string "SAME-" (= (zelph/number "42") (zelph/list-chars "42")))
"SAME-true"
zelph> %(print (zelph/car (zelph/number "42")))
2
zelph> %(print (zelph/car (zelph/cdr (zelph/number "42"))))
4
```

`&42` is the list `2 cons (4 cons nil)`. The `2` and the `4` are ordinary
nodes named `"2"` and `"4"` — the same kind of node as `Berlin`. They carry
no numeric meaning; what makes them digits is a table of facts about them,
and what makes the list a number is a set of rules that recurse over it.

Least-significant first is not arbitrary: it is the orientation in which
carries propagate forward, so the digit recursion is a plain structural
recursion on the tail. The display reverses it again, which is why you read
`&42` and not `&24`. (The reversal applies only to *registered digit
alphabets* — an ordinary list like `<x y z>` keeps its storage order.)

Arbitrary precision is not a feature; it is the absence of one. There is no
machine word anywhere:

```
zelph> ? &2 ^ &64
Answer: (&2 ^ &64) = &18446744073709551616
```

## Addition is a rule, and you can watch it work

The recursion in
[`common-arithmetic.zph`](arithmetic.md) is the schoolbook algorithm,
written as five rule families: a **trigger** that seeds the carry,
**decompose** rules that peel one digit off each operand and propagate the
carry-out, **assemble** rules that prepend the sum digit once the inner
sum is known, and a **connect** rule that exposes the result under `+`.

`.explain` shows a concrete run. In binary, 5 + 3:

```
zelph> .import binary-arithmetic
zelph+> ? &5 + &3
Answer: (&5 + &3) = &8
zelph+> .explain 3
(&5 + &3) = &8
   ├─ ((&5 add &3) ci 0) sum &8
   │  ├─ (&5 add &3) ci 0
   │  │  └─ &5 + &3  [axiom]
   │  ├─ ((1 d+ 1) tci 0) sum 0  [axiom]
   │  ├─ ((1 d+ 1) tci 0) co 1  [axiom]
   │  └─ ((&2 add &1) ci 1) sum &4
   │     ├─ (&2 add &1) ci 1  … [depth limit -- use '.explain <pattern> 0' for the full proof]
   │     ├─ ((0 d+ 1) tci 1) sum 0  [axiom]
   │     ├─ ((0 d+ 1) tci 1) co 1  [axiom]
   │     └─ ((&1 add nil) ci 1) sum &2  … [depth limit -- use '.explain <pattern> 0' for the full proof]
   └─ &5 + &3  [axiom]
```

Read it as the addition you did at school. `ci 0` is carry-in zero.
`(1 d+ 1) tci 0` is the full-adder lookup for the least significant digits
of 101 and 11: sum 0, carry-out 1. That carry-out becomes the `ci 1` of the
recursive subproblem on the tails, `&2 add &1`, and so on until an operand
runs out. Every intermediate is a fact in the graph, addressable and
queryable.

Note the leaves marked `[axiom]`. Under this substrate, the full-adder
entries are given: `binary-arithmetic.zph` states 16 of them, the logical
minimum for positional addition in base 2. Remember that — the next section
removes them.

## One base, or another, or none at all

The recursive process described above makes no reference to a base. It
matches `(A cons R)` and requests from a digit table the entries `d+`,
`co`, `sum`. By exchanging the table, the identical rules operate under a
different base. Each of these lines, entered into a fresh session, selects
one:

```
zelph> .import decimal-arithmetic     # 2,700 generated digit-table facts, base 10
zelph> .import binary-arithmetic      # 52 hand-written digit-table facts
zelph> .import binary-nand-arithmetic # see below
```

All three claim the module ID `arithmetic` through `.provides`, meaning that components built on top – such as polynomials, the simplifier, differentiation – import the first-loaded variant and are otherwise indifferent. The test suite runs the symbolic layer’s own tests (`src/test/test_symbolic.cpp`) across all three.

## Arithmetic from a single NAND

`binary-nand-arithmetic.zph` is the interesting one. It contains **one**
gate fact:

```
(1 nand 1) out 0
```

plus one negation-as-failure completion rule

```
(A isdigit true, B isdigit true, ¬((A nand B) out 0)) => ((A nand B) out 1)
```

and from those two lines, across the digit alphabet, it derives NOT, AND, OR, XOR, majority – and from those, the full adder, the full subtractor, digit multiplication and digit comparison that `common-arithmetic` consumes (digit equality needs no gate whatsoever). The recursion layer remains unchanged; it is unable to tell the difference.

It works exactly as before:

```
zelph> .import binary-nand-arithmetic
zelph+> ? &12 * &34
Answer: (&12 * &34) = &408
zelph+> ? &100 - &58
Answer: (&100 - &58) = &42
zelph+> ? &17 mod &5
Answer: (&17 mod &5) = &2
```

Now ask for the digit fact that was an `[axiom]` a moment ago:

```
zelph+> ? &5 + &3
Answer: (&5 + &3) = &8
zelph+> .explain ((1 d+ 1) tci 0) sum 0
((1 d+ 1) tci 0) sum 0
   ├─ 1 isdigit true  [axiom]
   ├─ 1 isdigit true  [axiom]
   ├─ 0 isdigit true  [axiom]
   ├─ (1 xor 1) out 0
   │  ├─ 1 isdigit true  [axiom]
   │  ├─ 1 isdigit true  [axiom]
   │  ├─ (1 nand 0) out 1
   │  │  ├─ 0 isdigit true  [axiom]
   │  │  ├─ 1 isdigit true  [axiom]
   │  │  └─ ¬((1 nand 0) out 0)  [absent]
   │  ├─ (1 nand 1) out 0  [axiom]
   │  ├─ (1 nand 0) out 1  [see above]
   │  └─ (1 nand 1) out 0  [axiom]
   └─ (0 xor 0) out 0
      ├─ 0 isdigit true  [axiom]
      ├─ 0 isdigit true  [axiom]
      ├─ (0 nand 1) out 1
      │  ├─ 1 isdigit true  [axiom]
      │  ├─ 0 isdigit true  [axiom]
      │  └─ ¬((0 nand 1) out 0)  [absent]
      …
```

It is no longer an axiom. The sole mathematical foundation of this tree consists of `(1 nand 1) out 0` and the statement that `0` and `1` are digits. The `[absent]` leaves represent negation-as-failure steps, accurately marked: `(1 nand 0) out 1` holds *because* `(1 nand 0) out 0` could not be derived. That the answer stays right is not assured by [stratified evaluation](../logic.md#stratified-evaluation) in this case, as the completion rule operates at the same level as the gate rules that read its outcomes. This is a property of the module: no rule derives a `nand` fact with output 0, hence nothing can arrive later that the negation should have seen. `.strata` still classifies the completion rule as not stratifiable, despite this, because the analysis compares predicates, and the completion rule both negates and derives `out` facts.

Follow the chain upward and you get: one Sheffer stroke → the Boolean
connectives → the full adder → positional addition → multiplication →
the integers → polynomial normal forms → the identity you proved in
tutorial 1. Every arrow is a rule you can read.

`.explain <pattern> 0` prints without a depth limit, if you want the whole
thing at once.

## Numbers are ordinary knowledge

Because arithmetic is rules over graph structure rather than a separate
evaluator, it composes with everything else. A rule can *cause* a
computation:

```
zelph> .import decimal-arithmetic
zelph+> (N ~ answer) => (N + &1)
zelph> &41 ~ answer
(&41 + &1) ⇐ (&41 ~ answer)
((&41 add &1) ci 0) ⇐ (&41 + &1)
((&41 + &1) = &42) ⇐ {(((&41 add &1) ci 0) sum &42) (&41 + &1)}
zelph+> ? &41 + &1
Answer: (&41 + &1) = &42
```

The rule asserted an addition *fact*; the arithmetic module noticed and
derived its result. (The line in between represents the carry state from
which the addition begins. The subsequent stages of the addition are
likewise deduced, though not displayed, indicated by the `+` on the next
prompt; `.deductions all` reveals them.) There is no distinction between
this and a rule concluding that Berlin is in Europe — the same unifier, the
same fixpoint loop. That is the property the whole mathematics stack is
built on, and the next tutorial uses it directly: a symbolic simplifier that
folds numeric constants without containing a single line of arithmetic.

## Where partiality lives

Natural subtraction is partial, and zelph does not paper over it:

```
zelph> .import decimal-arithmetic
zelph+> ? &3 - &5
zelph+>
```

No answer, not a wrong one, and not an exception. `&3 - &5` has no natural
result, so no rule derives one. [Tutorial 5](tutorial-polynomials.md) shows
how loading `integer-arithmetic` closes this gap for signed operands –
`(pos zint &3) - (pos zint &5)` folds to `(neg zint &2)` – without altering
any rule within the natural layer: `&3 - &5` continues to derive nothing.

## Exercises

1. `? &0 ^ &0` — what does zelph answer, and which convention is that?
2. Under `binary-arithmetic`, ask `? &7 / &0`. Explain the result in terms
   of partiality by absence.
3. Load `decimal-arithmetic` and use `.explain` on a multiplication to find
   the point where multiplication delegates to addition.
4. Compute the same product under all three substrates and confirm the
   answers agree. The *display* is identical because every substrate
   registers a digit alphabet — but the structure is not. Compare
   `%(print (zelph/car (zelph/number "5")))` under `decimal-arithmetic`
   (answers `5`) and under `binary-arithmetic` (answers `1`, the least
   significant bit of 101).

## Next

[3 · Terms and Rewriting](tutorial-terms.md) leaves the numerals behind and
builds symbolic terms — and shows how to teach zelph an operator it has
never heard of.
