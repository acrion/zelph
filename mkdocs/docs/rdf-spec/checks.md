# Three Checks on Wikidata

The mapping must enable three Wikidata checks that zelph cannot execute today, as all three require values: a date of death occurring before the date of birth within the same item; a work citing another work that was published afterwards; and the identification of prime numbers among Wikidata’s number items, including those claims of primality that are absent or incorrect. This page writes out the rules each check needs. The mapping has not yet been implemented; the rules, however, are valid rules of today’s engine, and every transcript outside a box is an actual execution of them on a few statements manually written in place of imported data.

Each check yields a list of items, each accompanied by its justification: `.explain` reconstructs, for every reported item, the statements and the comparison upon which it relies. The lists are meant as work-lists for editors, with each entry justified, and not as counts.

## What the Checks Share

**Full values, not truthy literals.** The checks read dates via the direct full-value facts `S fv:P569 V` ([Wikidata under the Mapping](wikidata-mapping.md)), where the objects carry precision and calendar, and through the [intervals](dates.md) they expand to. A truthy literal says "1 January" for every date with year-level precision and would result in incorrect checks.

**Expansion is requested for the properties that a check reads**, one standing request existing for each property ([Expansion](expansion.md#requests)):

!!! example "Specified – not yet implemented"

    ```
    fv:P569 zelph:expand-objects zelph:interval
    fv:P570 zelph:expand-objects zelph:interval
    fv:P577 zelph:expand-objects zelph:interval
    ```

**The session operates in `zelph` and reaches Wikidata via declared prefixes.** The standard library’s vocabulary (`interval`, `neg`, `zint`, `testprime`) comprises names from the language `zelph`, so the checks are typed under `.lang zelph`, the language a session initializes with. The nodes from Wikidata are reached through declared prefixes, which the [namespace table](iris.md#the-namespace-table) maps onto their names in `wikidata`:

!!! example "Specified – not yet implemented"

    ```
    .import decimal-arithmetic
    .import integer-arithmetic
    .import wikibase
    .prefix wd: <http://www.wikidata.org/entity/>
    .prefix wdt: <http://www.wikidata.org/prop/direct/>
    .prefix pq: <http://www.wikidata.org/prop/qualifier/>
    .prefix fv: <https://zelph.org/ns/wikidata/full-value/>
    .prefix wikibase: <http://wikiba.se/ontology#>
    ```

`decimal-arithmetic` is imported deliberately at the outset. `integer-arithmetic` brings the binary substrate unless a different one is already present, and Julian Day Numbers are a third as long in decimal digits as in binary ones.

**Violations only.** Each rule derives a fact solely in the case of a violation. A rule that derived a verdict for every pair it compares would write a fact for each of several hundred million citations; a rule whose comparison is a condition writes nothing for the pairs that are in order ([Costs](costs.md)).

**Doubtful statements are skipped.** A deprecated statement is one that editors have flagged as wrong, and reporting it would report what is already known. A statement qualified as approximate (sourcing circumstances `P1480` with the value circa, `Q5727902`) carries an interval that claims more than the editor intended. The rules below bypass both by relying on the marks carried by the direct full-value facts.

**Variables are single uppercase letters**, as is consistent throughout zelph. In the rules that follow, `F` and `L` denote the first and last bound of one interval, while `G` and `M` denote those of the second.

## A Date of Death before the Date of Birth

An item is reported when a date of death that is not doubtful certainly comes before a date of birth that is not doubtful: the last day the death could signify lies prior to the first day the birth could signify.

!!! example "Specified – not yet implemented"

    ```
    (X fv:P569 V, X fv:P570 W,
     V zelph:interval (F interval L), W zelph:interval (G interval M),
     M zelph:lt F,
     ¬((X fv:P569 V) wikibase:rank wikibase:DeprecatedRank),
     ¬((X fv:P570 W) wikibase:rank wikibase:DeprecatedRank),
     ¬((X fv:P569 V) pq:P1480 wd:Q5727902),
     ¬((X fv:P570 W) pq:P1480 wd:Q5727902))
      => (X died-before-born W)
    ```

A value lacking an interval, such as an unknown value, fails to meet any `zelph:interval` condition and is bypassed immediately, without additional processing.

The same rule runs today. In the run below, the intervals are written by hand, and `anna`, `ben` and `carl` stand for items: `anna` and `carl` were born in 1990 and passed away on 11 March 1952, `ben` in the reverse order, and `carl`’s date of death is marked as deprecated.

```
zelph> .import decimal-arithmetic
zelph+> .import integer-arithmetic
zelph> %(zelph/no-selffact-sugar "interval")
zelph> v1990 zelph:interval (&2447893 interval &2448257)
zelph> v1952-03-11 zelph:interval (&2434083 interval &2434083)
zelph> anna fv:P569 v1990
zelph> anna fv:P570 v1952-03-11
zelph> ben fv:P569 v1952-03-11
zelph> ben fv:P570 v1990
zelph> carl fv:P569 v1990
zelph> carl fv:P570 v1952-03-11
zelph> (carl fv:P570 v1952-03-11) wikibase:rank wikibase:DeprecatedRank
zelph> (X fv:P569 V, X fv:P570 W, V zelph:interval (F interval L), W zelph:interval (G interval M), M zelph:lt F, ¬((X fv:P569 V) wikibase:rank wikibase:DeprecatedRank), ¬((X fv:P570 W) wikibase:rank wikibase:DeprecatedRank), ¬((X fv:P569 V) pq:P1480 wd:Q5727902), ¬((X fv:P570 W) pq:P1480 wd:Q5727902)) => (X died-before-born W)
...
zelph> X died-before-born W
Answer: anna died-before-born v1952-03-11
zelph> .explain anna died-before-born v1952-03-11
anna died-before-born v1952-03-11
   ├─ anna fv:P570 v1952-03-11  [axiom]
   ├─ v1952-03-11 zelph:interval (&2434083 interval &2434083)  [axiom]
   ├─ v1990 zelph:interval (&2447893 interval &2448257)  [axiom]
   ├─ anna fv:P569 v1990  [axiom]
   ├─ &2434083 zelph:lt &2447893  [compared]
   ├─ ¬((anna fv:P570 v1952-03-11) wikibase:rank wikibase:DeprecatedRank)  [absent]
   ├─ ¬((anna fv:P569 v1990) wikibase:rank wikibase:DeprecatedRank)  [absent]
   ├─ ¬((anna fv:P570 v1952-03-11) pq:P1480 wd:Q5727902)  [absent]
   └─ ¬((anna fv:P569 v1990) pq:P1480 wd:Q5727902)  [absent]
```

Only `anna` is reported. The explanation is the entry of the work-list: the two statements, their intervals, the comparison, and the four marks that were looked for but not found.

## A Work That Cites a Later Work

Here, the two dates belong to two items linked by a statement: `X P2860 Y` says that `X` cites `Y`. A citation is reported when a publication date of the citing work certainly comes before a publication date of the cited one.

!!! example "Specified – not yet implemented"

    ```
    (X wdt:P2860 Y, X fv:P577 V, Y fv:P577 W,
     V zelph:interval (F interval L), W zelph:interval (G interval M),
     L zelph:lt G,
     ¬((X fv:P577 V) wikibase:rank wikibase:DeprecatedRank),
     ¬((Y fv:P577 W) wikibase:rank wikibase:DeprecatedRank),
     ¬((X fv:P577 V) pq:P1480 wd:Q5727902),
     ¬((Y fv:P577 W) pq:P1480 wd:Q5727902))
      => (X cites-later-work Y)
    ```

Run today on two works that cite one another, published in 1990 and in 2001:

```
zelph> .import decimal-arithmetic
zelph+> .import integer-arithmetic
zelph> %(zelph/no-selffact-sugar "interval")
zelph> v1990 zelph:interval (&2447893 interval &2448257)
zelph> v2001 zelph:interval (&2451911 interval &2452275)
zelph> paper-a fv:P577 v1990
zelph> paper-b fv:P577 v2001
zelph> paper-a P2860 paper-b
zelph> paper-b P2860 paper-a
zelph> (X P2860 Y, X fv:P577 V, Y fv:P577 W, V zelph:interval (F interval L), W zelph:interval (G interval M), L zelph:lt G) => (X cites-later-work Y)
...
zelph> X cites-later-work Y
Answer: paper-a cites-later-work paper-b
```

**Several publication dates.** A work may include multiple ones, such as a first edition and a reprint, and the rule above reports a citation as soon as any pair of dates is out of order. That is the right list for an initial pass; it contains every citation worth examining and some that are fine. A second stage narrows it to the citations where no pair of dates is in order, with doubtful dates being skipped as previously, and it operates exclusively on the reported candidates, thus incurring no cost for citations that never were candidates:

!!! example "Specified – not yet implemented"

    ```
    (X cites-later-work Y, X fv:P577 V, Y fv:P577 W,
     V zelph:interval (F interval L), W zelph:interval (G interval M),
     L zelph:ge G,
     ¬((X fv:P577 V) wikibase:rank wikibase:DeprecatedRank),
     ¬((Y fv:P577 W) wikibase:rank wikibase:DeprecatedRank),
     ¬((X fv:P577 V) pq:P1480 wd:Q5727902),
     ¬((Y fv:P577 W) pq:P1480 wd:Q5727902))
      => (X may-follow Y)
    (X cites-later-work Y, ¬(X may-follow Y)) => (X cites-only-later-work Y)
    ```

## Primes among the Number Items

Wikidata holds an item for numerous whole numbers, and certain ones include the statement "instance of prime number" (`P31` with the value `Q49008`). The check computes which numbers are prime and compares: an item whose number is prime yet lacks the statement is a **missing** claim, an item that carries the statement yet has a composite number is a **wrong** one. It needs a network containing the number items, which the full networks provide but the pruned ones do not ([Binary Data Files](../binaries.md)).

### Which Items, Which Numbers

The check must not start from every node whose name is interpreted as a number. Numerous items share the same English label, and a label made of digits pertains to years, albums, and films alike: 1999 is a prime, and a check conducted across all nodes named using digits would incorrectly indicate that the year 1999 lacks its primality claim. The check begins from a class.

- **The items** are the instances of a class located beneath "integer" (`Q12503`). The class to which an item belongs is traced upward via "subclass of", ensuring the selection remains independent of the current subclass chosen by Wikidata’s editors: in the dump from 9 March 2026, the positive integers were instances of `Q28920044`, which was merged into `Q16317911` on 27 April 2026.
- **The number** of an item is taken from its statement "numeric value" (`P1181`) when present and the unit is the number 1 (`Q199`), since some items with `P1181` are physical quantities with a real unit. The amount is a decimal literal such as `"+113"^^xsd:decimal`, which expands to `&113` ([Numbers](numbers.md)).
- **Otherwise the name is read.** For a selected item lacking such a statement, the [name reader](expansion.md#name-readers) `zelph:decimal-integer` reads the item’s name as a whole number, strictly in the language `mul` and then in `en`, and only if the name consists of digits, optionally preceded by a sign. The link it writes does not merge anything: the item and the numeral remain as two separate nodes.

!!! example "Specified – not yet implemented"

    ```
    (X wdt:P31 C, C wdt:P279∗ wd:Q12503) => (X number-item yes)

    (X number-item yes, X fv:P1181 V, V wikibase:quantityAmount A) => (A zelph:expand zelph:value)
    (X number-item yes, X fv:P1181 V, V wikibase:quantityUnit wd:Q199,
     V wikibase:quantityAmount A, A zelph:value N) => (X number N)

    (X number-item yes, ¬(X fv:P1181 V)) => (X zelph:read-name zelph:decimal-integer)
    (X number-item yes, X zelph:value N) => (X number N)
    ```

The second and fourth rule each have requests as their consequences. `.explain` therefore shows, for every expanded value, the reason it was expanded.

### The Check

The `primes` module determines primality through rules, starting from the self-fact `N testprime N`, and derives `(N testprime N) = prime` or `composite` ([Primality](../math/primality.md)). The check asks for every number within its range and performs a comparison:

!!! example "Specified – not yet implemented"

    ```
    (X number N, N zelph:ge &2, N zelph:lt &10000) => (N testprime N)

    (X number N, (N testprime N) = prime, ¬(X wdt:P31 wd:Q49008)) => (X missing-prime-claim N)
    (X number N, (N testprime N) = composite, X wdt:P31 wd:Q49008) => (X wrong-prime-claim N)
    ```

Run today on three items written by hand, with the initial two asserting they are prime:

```
zelph> .import decimal-arithmetic
zelph+> .import primes
zelph> item113 number &113
zelph> item113 P31 Q49008
zelph> item111 number &111
zelph> item111 P31 Q49008
zelph> item97 number &97
zelph> .deductions off
...
zelph> (X number N) => (N testprime N)
...
zelph+> (X number N, (N testprime N) = prime, ¬(X P31 Q49008)) => (X missing-prime-claim N)
...
zelph+> (X number N, (N testprime N) = composite, X P31 Q49008) => (X wrong-prime-claim N)
...
zelph+> X missing-prime-claim N
Answer: item97 missing-prime-claim &97
zelph> X wrong-prime-claim N
Answer: item111 wrong-prime-claim &111
```

113 is prime and asserts as much; 111 is 3 · 37 and falsely claims to be prime; 97 is prime and does not assert it.

### The Range

Primality by rules is a computation in which every step is a fact, and it tries the candidate divisors in turn, up to the square root of the number, until one divides. The check thus establishes a **bound**: it tests the numbers beneath it and explicitly states this. The bound is widened by rules that operate exclusively on prime divisors, because a number lacking a prime divisor up to its square root cannot have any divisor there whatsoever; for numbers up to 121,232, this reduces the count to 945,977 divisions instead of 3,579,464 (computed). Such rules form a module distinct from `primes`, which remains as described in the mathematics pages.

What remains unverified is stated with the result, and it is most of what Wikidata designates as prime: among the 136,822 items that were instances of `Q49008` on 4 November 2026, 125,360 are primes named by a formula, such as "9037·2^805780 + 1", that lack a numeric value and whose names are not composed of digits (as counted via the Wikidata Query Service). No rule set examines these. The check covers the number items with a simple number under the bound, provides a report for each chosen item on whether it was tested, and leaves the rest to a separate list.

## What the Checks Cost

At the scale of the full network, the first check compares the dates of approximately four million items, while the second check assesses up to 420 million citations (estimated on 4 November 2026 based on a sample from the dump of 9 March 2026). The expenditure of time and memory, with the comparison as a condition and with the rules of `cmp`, is computed in [Costs](costs.md).
