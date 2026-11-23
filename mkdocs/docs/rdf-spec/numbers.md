# Numbers

A numeric literal expands to a number from zelph’s standard library. This page specifies the number each literal expands to, the one new form of number the mapping needs, how the floating-point types are integrated, the two number constants a script can write, and the method by which numbers are compared. Nothing on this page has been implemented yet; a sentence about the present engine says "today", and a transcript outside a box is a real run of the present engine.

## One Node per Value

The principle of this page is that a number is one node, regardless of the datatype that wrote it or how it was spelled. `"113"^^xsd:integer`, `"+113"^^xsd:decimal`, `"113.0"^^xsd:decimal`, `"0113"^^xsd:byte` and `"1.13E2"^^xsd:double` are five [value nodes](literals.md), since they are five RDF terms, and they share one [expansion](expansion.md): the numeral `&113`. A rule that joins two statements on a number, or compares two numbers, works on the expansions and never needs to know how a source wrote them.

The numbers within the standard library are available today:

- **A natural number** is a numeral, the list of its digits within the session’s arithmetic substrate, written `&113` ([Arithmetic Substrates](../math/arithmetic.md)).
- **A negative whole number** is the term `(neg zint N)` constructed over a numeral, after importing `integer-arithmetic` ([Integers over ℤ](../math/integers.md)).

The mapping introduces a single form, for numbers that are not whole.

## The Decimal Family

The decimal family comprises `xsd:decimal`, `xsd:integer`, and the twelve types XSD derives from `xsd:integer` (`xsd:long`, `xsd:int`, `xsd:short`, `xsd:byte`, their four unsigned counterparts, and the four types restricted by sign). XSD defines the values of the family as the numbers expressible as a whole number divided by a power of ten, and each such value expands to exactly one node that represents it exactly:

- **A whole value** expands to the number the standard library already has for it: a numeral, or `(neg zint N)`. It does so even when written as a decimal.
- **Every other value** expands to a term `(M dec K)`, which stands for M · 10^−K: `M` is a whole number that is not divisible by ten, `K` a natural number starting from 1.

| Literal | Expansion |
|---|---|
| `"113"^^xsd:integer`, `"+113"^^xsd:decimal`, `"113.0"^^xsd:decimal`, `"0113"^^xsd:byte` | `&113` |
| `"-5"^^xsd:integer`, `"-5.00"^^xsd:decimal` | `(neg zint &5)` |
| `"13.05"^^xsd:decimal`, `"013.0500"^^xsd:decimal` | `(&1305 dec &2)` |
| `"0.1"^^xsd:decimal`, `".1"^^xsd:decimal` | `(&1 dec &1)` |
| `"-0.5"^^xsd:decimal` | `((neg zint &5) dec &1)` |
| `"-0"^^xsd:integer`, `"0.0"^^xsd:decimal` | `&0` |

The form is selected such that every value is exact regardless of the substrate. 13.05 lacks a finite representation in binary digits, and a representation reliant on the substrate would make a decimal network meaningless when using binary arithmetic. `(M dec K)` is constructed from the whole numbers that every substrate provides. Since `M` is not divisible by ten and `K` is at least 1, a value has exactly one such term.

`dec` serves as a constructor and not as an operation. It must not function as an arithmetic operator within the standard library: `(&7 / &2)` invokes division and triggers rules, whereas `(&35 dec &1)` is a number and asks for nothing. It is a name in the language `zelph`, beside `neg` and `zint`, and, like those, is registered so that a term such as `(&1 dec &1)`, where a node relates to itself, is printed in full rather than using the self-fact prefix.

**The expander** of the family functions through three steps, all implemented in the host language and none governed by rules:

1. **Check.** The lexical form must conform precisely to the lexical space of its type, just as XSD specifies: no leading or trailing whitespace, an optional sign, digits, and for `xsd:decimal` optionally a decimal point with digits before or after it. `"5."` qualifies as an `xsd:decimal` but not as an `xsd:integer`. For restricted types, the value must fall within the type’s permissible range; the check operates on digit strings and does not require handling large numbers. A literal that fails this validation is ill-typed and gets no expansion ([Datatypes](literals.md#datatypes)).
2. **Normalize.** The `+` sign is removed, leading zeros are eliminated, trailing zeros after the decimal point are stripped, and any fraction consisting entirely of zeros is discarded.
3. **Build.** A whole value is constructed using `zelph/int`, which builds the numeral via the substrate’s reader `zelph/number` and encloses a negative value in `(neg zint N)`; a fractional value is built as `(M dec K)` from two such numbers.

Wikidata writes every amount as an `xsd:decimal` with a sign, including whole ones: `"+113"^^xsd:decimal`. Today `zelph/int`, which lives in `integer-arithmetic`, accepts a leading `-` and refuses a `+`:

```
zelph> .import decimal-arithmetic
zelph+> .import integer-arithmetic
zelph> %(zelph/int "-5")
<zelph/node neg zint &5>
zelph> %(zelph/int "113")
<zelph/node &113>
zelph> %(zelph/int "+113")
error: invalid decimal literal &+113
...
```

Through the mapping `zelph/int` moves into `common-arithmetic`, which each substrate imports, accepts the `+`, and shares its normalization with the expanders and with the [constants](#constants).

## Doubles and Floats

`xsd:double` and `xsd:float` expand to their **exact value**, in the same forms as the decimal family. Every finite floating-point number is a whole number divided by a power of two, and thus qualifies as a finite decimal: `"0.5"^^xsd:double` is `(&5 dec &1)`, the node that `"0.5"^^xsd:decimal` expands to.

- **Exact means the value the type holds, not the digits that were written.** `"0.1"^^xsd:double` is the double nearest to one tenth, which is 0.1000000000000000055511151231257827021181583404541015625, and that is what it expands to: `(M dec &55)` with the 55 digits after the point as `M`. It is not the node of `"0.1"^^xsd:decimal`, and rightly so, since the two literals denote different numbers.
- **The value is the one XSD’s lexical mapping gives**: the lexical form rounded to the nearest representable number, ties to even, in the precision of the type, with `xsd:float` rounded to its own precision directly and not via a double. This requires a conversion that rounds correctly. Janet’s parser for numbers does not (it was off by one unit in the last place in 203 of 4,120 test cases, measured on 4 November 2026), so the conversion and the exact digits come from a function of the engine, just as the digits of a numeral do.
- **The lexical space** is XSD’s: digits with an optional point and an optional exponent, and the three words `INF`, `-INF` (also `+INF`) and `NaN`, in exactly this case.
- **The special values** are three nodes from the engine’s vocabulary that are not numbers: `zelph:INF`, `zelph:-INF` and `zelph:NaN`, identical for both types.
- **Negative zero** expands to `(neg zint &0)`.

**A deliberate departure.** XSD, and OWL with it, keeps the values of `xsd:double` and `xsd:float` apart from decimal values and from each other: there, the double 0.5 and the decimal 0.5 are two distinct values. In zelph they are a single number, since "one node per value" enables a rule to compare a coordinate with a bound, or a measurement with a threshold, without needing to know which type each was written in. The draft of XPath and XQuery Functions and Operators 4.0 performs comparisons between mixed numeric types exactly as well (as of 3 November 2026). The value nodes for the two literals remain distinct, ensuring no information from the source is lost.

**What negative zero means for the standard library.** `integer-arithmetic` documents today that `(neg zint &0)` is never produced: zero is a single node. The mapping produces it from exactly one source, a negative zero from a floating-point type, and adds no rule to the mathematics for it. The term is equal to `&0` under every value comparison and is a distinct node; arithmetic over it answers the ordinary zero, as the rules of the standard library already accept a signed zero as an operand and lift it accordingly. A rule over values relies on the comparisons presented on this page, never on the identity of nodes.

**What an exact value costs.** A double contains around 52 significant decimal digits on average, whereas Wikidata’s text representation for the same coordinate holds 8 (estimated on 4 November 2026 from a sample of coordinates), with each digit occupying a single cell. Floating-point values thus present the clearest case for expansion on request ([Costs](costs.md)).

**What it opens.** A later floating-point module can compute on exact values and treat rounding as a distinct operation that an explanation shows: IEEE 754 defines the result of an operation as the exact result, followed by rounding, and with exact values present in the graph, both stages become facts. This holds true for addition, subtraction, multiplication, and conversion, whose exact results are finite decimals; a quotient is exact solely as a rational number, and roots as well as transcendental functions possess no exact finite value whatsoever. For these cases, and as an alternative across the board, such a module can answer with an **enclosure**: an interval whose bounds are rounded outwards, ensuring that a derived "less than" relation is never wrong. The three interval relations – certainly before, certainly after, and overlapping – then apply uniformly to dates, the bounds of Wikidata’s quantities, and computed values alike ([Dates](dates.md#the-three-relations)).

For the ACE front end, nothing shifts at this time; its specification gives real numbers no route ([DEC-DATA](../ace-spec/decisions.md#dec-data)), and a module for decimal arithmetic would subsequently provide them with a target.

## Constants

A script writes a negative or a fractional number using the `&` constant ([Writing and Displaying Terms](notation.md#number-constants)):

!!! example "Specified – not yet implemented"

    ```
    zelph> .import decimal-arithmetic
    zelph+> reading value &13.05
    zelph> reading value X
    Answer: reading value &13.05
    zelph> freezer celsius &-18
    zelph> freezer celsius X
    Answer: freezer celsius &-18
    ```

- **One normalization.** The content following `&` undergoes the normalization process of the decimal family: `zelph/decimal-parts` splits a lexical form into sign, whole digits, and fraction, while `zelph/decimal` constructs the node. Thus, `&13.05` is the expansion of every decimal literal having that value, the identical node, across every substrate. It is not the expansion of `"13.05"^^xsd:double`, which is a distinct number.
- **One hook.** The parser hands every `&` token to `zelph/number`, as it does today. The substrate’s `zelph/number` builds a token of digits as it always has and forwards all other tokens to the normalization. A token of digits follows precisely today’s path.
- **Only a substrate is required** to write the constants; `neg`, `zint`, and `dec` are names of `zelph` regardless of the imported module. Performing arithmetic with negative numbers requires `integer-arithmetic`, and computing with `dec` terms necessitates the later module below.
- **Display.** Within a term, `(neg zint &5)` appears as `&-5` and a `dec` term is rendered in its canonical form as a decimal number, ensuring that what is printed reads back in every session language. Published output evolves accordingly: whereas a transcript today displays `jdet (neg zint &512)`, it will show `jdet &-512`. A statement that is itself such a term keeps its full form when it serves as the answer or the deduction reported on a line, as do `(neg zint &0)` and the operand form `(pos zint N)`.

## Comparing Numbers

zelph compares numbers in two ways today, and both persist.

**By rules.** The fact `A cmp B` asks the standard library to perform a comparison, and its rules derive one of `A < B`, `A > B` and `A == B` alongside the corresponding `(A cmp B) = lt`, `gt`, or `eq`:

```
zelph> .import decimal-arithmetic
zelph+> .import integer-arithmetic
zelph> &1999 cmp &2001
(&1999 lcmp &2001) ⇐ (&1999 cmp &2001)
...
(&1999 < &2001) ⇐ {(&1999 cmp &2001) ((&1999 lcmp &2001) res lt)}
((&1999 cmp &2001) = lt) ⇐ {(&1999 cmp &2001) ((&1999 lcmp &2001) res lt)}
zelph+> (&1999 cmp &2001) = X
Answer: (&1999 cmp &2001) = lt
...
```

The comparison is a digit-by-digit derivation that remains within the graph and that `.explain` can show. This is the semantics of numbers in zelph, and the mapping changes nothing in it.

**By a condition.** In a rule or a query, the engine answers `A zelph:lt B` and its five relatives autonomously ([Numeric Comparisons](../logic.md#numeric-comparisons)). The condition derives nothing, yet it orders every pair of numbers by value: a property test confirms its alignment with `cmp` at every instance where `cmp` gives a verdict. It serves as the form for a check across many pairs ([Checks](checks.md)).

What the mapping adds:

- **`dec` terms are numbers for the condition.** The engine’s comparison reads `(M dec K)` alongside numerals and signed integers, arranging all such values by value, in any combination; the comparator has had a dedicated slot reserved for exactly this term since it was written.
- **The special values are not numbers.** No relation holds between `zelph:NaN` and any entity, just as none holds today for a term that is not a number, nor between infinity and a number. Only the ordering in a SPARQL `ORDER BY`, which zelph makes total, determines their placement: `NaN` first, followed by `-INF`, then the numbers, then `INF` ([Querying Terms](querying.md#comparing-and-ordering)).
- **A module for decimal arithmetic** brings the rules: `cmp` over `dec` terms yields outcomes on `<`, `>`, `==` and `=` as for whole numbers, along with the four operations. It accepts naturals, signed integers, and `dec` terms in any combination, and compares two `dec` terms by aligning them to a common power of ten. It is not included in the mapping; prior to its existence, `cmp` gives no verdict for a `dec` term, and the condition does.
- **Equality of numbers is a comparison, never identity of nodes.** `&7`, `<007>` and `(pos zint &7)` are three distinct nodes sharing one value, just as `&0` and `(neg zint &0)` are. `zelph:eq` affirms this for every such pair, and `==` wherever `cmp` gives a verdict, which it does not for a numeral against `(pos zint &7)`; `!=`, which asks whether two nodes differ, does not.

**Rationals later.** A module for rational numbers keeps decimal values in the form presented on this page: a rational whose denominator, when expressed in lowest terms, possesses no prime factor other than 2 and 5, is a whole number or a `dec` term, and only the remaining rationals are assigned a constructor of their own. A result is always transformed into this form, ensuring that one value stays one node.
