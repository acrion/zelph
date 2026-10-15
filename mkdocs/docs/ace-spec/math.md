# Numbers and Formulas

This part of the front end lets an ACE text talk about whole numbers: give a person an age, ask for the
value of a term, compare two quantities, state that two names name one thing, and write rules whose
conditions are comparisons, such as "If a person's age is A and A > 17 then the person is an adult."
It is milestone M5 of the [work plan](plan.md#m5-arithmetic), and it is loaded with an import of its
own, `.import ace-math`, because importing zelph's mathematics changes the whole session: 6135 nodes
and 456 rules (measured on the current tree with the evaluation module's starting text), and an
inference cost on every later input line.

zelph already computes all of this. Its mathematical standard library — numbers as lists of digit
nodes, the integers, polynomial normal forms, a term simplifier and a primality test, all written as
ordinary rules ([Why zelph for Mathematics](../math/index.md)) — adds, subtracts, multiplies, raises to
powers, folds a ground term to its value and decides whether two polynomials are equal. What it lacks
for ACE is small: a division that is exact or silent instead of truncating, and a three-way verdict
"less, equal, greater" over the difference of two terms. Both are 23 rules of a new general module,
`stdlib/evaluation.zph`, which knows nothing about ACE and is useful without it. Everything else is the
mapping, written in `stdlib/ace-math.zph`: from ACE's terms to the math stack's terms, from a formula
to a request, and from the request's answer back to an ACE sentence. The C++ core learns nothing
about ACE arithmetic, in the same way as `stdlib/sparql.zph` puts SPARQL on top of the Janet API.

A formula is never stored as a fact. A statement such as `1 + 2 = 3.` is *checked*, a question is
*decided*, and a condition in a rule is *compiled* into requests the rule makes. Where zelph cannot
decide, `ace` says so ("It is not provable that …") and never guesses.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace-math
    zelph+> ace John is a person. John's age is 30. Mary is a person. Mary's age is 15.
    If a person's age is A and A > 17 then the person is an adult.
    Who is an adult?

    Answer: John is an adult.
    zelph> ace What is 2 + 3 * 4? What is 2^3^2? What is 7 / 2?

    Answer: 2 + 3 * 4 = 14.
    Answer: 2^3^2 = 64.
    No answer.
    zelph> ace For every integer X is it true that (X + 1)^2 = X^2 + 1?

    Answer: No. It is false that for every integer X (X + 1)^2 = X^2 + 1. (X + 1)^2 - (X^2 + 1) = 2*X.
    ```

The import notice of `ace`, which `.import ace-math` pulls in, is left out, as the transcripts of
this documentation leave out module banners. The `+` in the prompt after the import is zelph's own
mark that the import's inference withheld deductions from the output (`.help .deductions`); the
block's quiet inference clears it.

The first block stores four facts and two rules. The rule's condition `A > 17` becomes a request for a
verdict, which the evaluation module answers with `gt`, `eq` or `lt`; a second rule, the *seed*, makes
that request for every person with an age, because zelph's mathematics computes only what is asked
for. In the form `ace/zelph-of` prints them (lines that name the math stack are exact under `.lang
zelph`):

```
John ~ person
John age &30
Mary ~ person
Mary age &15
((X ~ person), (X age Y)) => (Y compare &17)
((X ~ person), (X age Y), ((Y compare &17) = gt)) => (X ~ adult)
```

The rule variables are named in the order of their first occurrence ([Variables](api.md#variables),
[DEC-VAR-LETTERS](decisions.md#dec-var-letters)): `X` for the person, `Y` for the age. The class
pattern `X ~ person` has no variable of its own in the core profile and takes no letter. The age gets
no class pattern `Y ~ age`, because it is a number
([DEC-NUMERIC-REFERENT](decisions.md#dec-numeric-referent)). `7 / 2` has no
whole-number value, so the question has no answer rather than the truncated 3. The last answer is
false for every integer X and says why: the difference of the two sides is `2*X`, not zero. The
[section on formulas in rules](#formulas-in-rules) runs exactly these lines on today's engine.

What a user can rely on, in short:

- **Integers only.** Decimals and units are refused by construct id, before anything is written.
- **`/` is exact.** `12 / 4` is 3; `7 / 2` and `7 / 0` have no value. zelph's own `/`, which truncates
  on the naturals, is never used.
- **ACE's operator rules.** `^` binds tighter than `*` and `/`, which bind tighter than `+` and `-`;
  every operator is left-associative, `^` included, so `2^3^2` is 64; a negative literal is one token,
  so `-2^2` is 4; `X-1` is a word, not a subtraction.
- **Free variables are existential.** `Is it true that (X + 1)^2 = X^2 + 1?` is not provable, because
  X = 0 satisfies it; "For every integer X …" asks the universal question.
- **Formulas are checked, decided or compiled, never stored**, and unknown is said as unknown.
- **Different names name different things.** `John is Bill.` is a contradiction unless the lexicon
  says that both words name one node; "a man" is not a name, so whether it is John is not provable.

## Invariants

These hold for every part of the design below; each is settled, and the reason is given.

1. **An ACE formula is never asserted as a zelph `=` fact.** In zelph, `=` exposes the single result
   of a request; an asserted `(&1 + &2) = &4` gives the request `&1 + &2` two answers, and the wrong one
   leaks into other computations (the run below). Formulas are decided (questions), checked
   (statements) or compiled (rules).
2. **ACE `/` is `exquo`, never zelph's `/`.** Native `/` truncates on the naturals: `(&7 / &2) = &3`.
   `exquo` is the exact quotient with all sign cases and is silent when the division is inexact or the
   divisor is zero ([the evaluation module](#the-evaluation-module)). An arithmetic term is stored
   only by its value, never as a term, because every stored subterm is a claim and therefore a request.
3. **ACE's operator rules, not those of math-syntax.** The `$( … )` island grammar is not embedded:
   all ACE operators are left-associative, `^` included; a negative literal is atomic; the hyphen is a
   word character.
4. **Free variables are existential**, as in ACE's semantics (corpus 3633 `X=X.` gives a referent
   `something` and the formula `A = A`). A universal reading comes only from an ACE universal
   ([math-universal implications](#math-universal-implications)).
5. **Canonical numbers.** Every number node `ace` writes is a natural numeral `&n` (n ≥ 0) or
   `(neg zint &n)` (n < 0), never `(pos zint &n)`, never an island term `(neg of &n)`. `zelph/int` builds
   exactly this shape, `-0` included. `(pos zint N)` lives on only as the coefficient form of the
   polynomial layer, which `ace` reads and never writes.
6. **Every arithmetic condition of a rule gets a seed rule.** zelph's mathematics is request-driven:
   `((X age Y), (Y > &17)) => (X ~ adult)` alone derives nothing.
7. **The ring order is only extended, never contradicted** ([math variables](#math-variables-and-the-ring-order)).
8. **All arithmetic evaluation runs in the scratch cluster `ace:math`, which is dropped**; results
   cross the drop as text, never as node handles (a value handle read inside the dropped cluster prints
   `??` afterwards).
9. **Unknown is said as unknown** (`It is not provable that …`, `[ace/w-not-checkable]`), never as
   false.
10. **Reals, units, string concatenation and comparisons over non-numbers are refused by construct
    id** before any graph change. The parser still accepts them, so that the DRS stays the one APE
    gives.
11. **No zelph syntax in an `ace` block or in its output**; `math-syntax/operator` does not extend ACE.
12. **Knowledge lives in rules.** The exact quotient, the difference normal form and the verdict are
    rules of `stdlib/evaluation.zph`; the value of a ground term is the simplifier's `(:simplify T)`,
    which that module teaches `exquo`. Janet builds requests, reads results and words answers. `ace`
    has no value request of its own: it would be a second implementation of `:simplify`.
13. **Every node of the math stack is resolved in the naming language `zelph`**, under every `.lang`.
14. **Code that calls `zelph/number` or `zelph/int` is compiled after `.import math`**: it lives in
    `ace-math.zph` below that file's import lines, and every other module reaches it only through the
    hook ([below](#the-hook)).
15. **Values before negation.** A negated formula condition of a rule tests a verdict over values bound
    positively in the same rule ([values before negation](#values-before-negation)).
16. **A math result counts only when its scratch run met no contradiction and defeated no conclusion
    under "not provably", and its request has exactly one answer** ([trust](#trust)).

The run shows the first three invariants against what zelph does natively. The island reads `2^3^2`
as 2^(3^2) = 512, where ACE reads (2^3)^2 = 64, and `-2^2` as −4, where ACE reads 4; zelph's own `/`
truncates `7 / 2` to 3. Then one asserted equation: after `(&1 + &2) = &4` the request `&1 + &2` has two
answers, zelph reports contradictions (the simplifier's rule that a term has one normal form now
fires), and the earlier, unrelated request `2^3^2` has gained a second value.

```
zelph+> ? :simplify $( 2^3^2 )
Answer: (:simplify $( &2 ^ &3 ^ &2 )) = &512
zelph+> ? :simplify ((&2 ^ &3) ^ &2)
Answer: (:simplify ((&2 ^ &3) ^ &2)) = &64
zelph+> ? :simplify $( -2^2 )
Answer: (:simplify $( neg(&2 ^ &2) )) = (neg zint &4)
zelph+> ? &7 / &2
Answer: (&7 / &2) = &3
zelph+> .deductions off
Deduction printing mode: off
  No derivations are printed; a run says how many it hid and marks the prompt with '+' (e.g. "zelph+> ").
zelph+> (&1 + &2) = &4
Note: 137 deductions were hidden.
Found one or more contradictions!
zelph+> ? &1 + &2
Answer: (&1 + &2) = &4
Answer: (&1 + &2) = &3
zelph> ? :simplify $( 2^3^2 )
Answer: (:simplify $( &2 ^ &3 ^ &2 )) = &4096
Answer: (:simplify $( &2 ^ &3 ^ &2 )) = &512
```

## Modules, the Hook and the Load Order

### Where the parts live

| Part | File | Why there |
|---|---|---|
| typing, the detection of the `formula/` and `expr/` construct ids, negated formulas, kinds of referent, definitions, identity planning | `ace-drs.zph`: private `ace-drs/…` helpers called by `ace/drs-constructs`, the normalisation passes and `ace/plan` | pure; needed without `ace-math` (corpus classification; identity needs no arithmetic) |
| identity verdicts by node identity | the resolution step of `ace/execute!` in `ace-zelph.zph`, without the hook | needs the graph ([DEC-NEQ](decisions.md#dec-neq) (b)), not arithmetic |
| `ace/math-text`: the user's term from the DRS as ACE text | `ace-verbalise.zph` | pure; identity answers and the verbaliser round trip over corpus DRSs run without arithmetic |
| `ace/num!`, `ace/math-term!`, `ace/math-indet-node!`, `ace/math-order!`, `ace/math-value!`, `ace/math-decide!`, `ace/math-numeral`, `ace/math-nf-text`, `ace/math-plan` | `ace-math.zph` | below `.import math` (invariant 14) |
| `exquo`, `difference`, `compare` and their hook into the simplifier | `stdlib/evaluation.zph`, a general module, imported by `ace-math.zph` and **not** by `math.zph` ([DEC-MATH-MODULE](decisions.md#dec-math-module)) | knowledge in zelph, useful outside ACE |
| the math vocabulary: the ordinary entries of `prime`, `divisor`, `derivative` (`ace-math/ulex`) and the `math_word` entries (`ace-math/math-words`) | Janet data in `ace-math.zph`, registered with `(ace-lexicon/add-shipped "math" …)` | shipped vocabulary is Janet data, never written to the graph, not even as `ace-lex` facts ([DEC-LEX-STORE](decisions.md#dec-lex-store) (d)) |

The signatures and effect classes of the public functions are on [Modules, API and Data
Shapes](api.md#ace-math); the format of shipped vocabulary is [Tokenizer, Grammar and
Lexicon](grammar.md#shipped-vocabulary-ace-core)'s.

### `ace-math.zph`

The file runs in this order:

```
.import ace
%(def ace-math/user-lang (zelph/lang))
%(zelph/lang "zelph")
.import math
.import evaluation
.import primes
%
  … ace/num!, ace/math-term!, … ace/math-plan, ace-math/ulex, ace-math/math-words …
  (zelph/lang ace-math/user-lang)
  (ace-lexicon/add-shipped "math" {:ulex ace-math/ulex :math-words ace-math/math-words})
  (put ace/state :math @{…hook table… :epoch (zelph/epoch)})   # last
%
```

- It remembers the user's language, switches to `zelph` for its own lines and the imports, and
  restores the user's language as its last act (the host also restores it at the end of every module).
- The registration of the vocabulary writes no node.
- `ace.zph` never imports `ace-math`, so there is no import cycle (a module in progress counts as
  imported anyway). Dependencies are `.import` statement lines, never `(zelph/import …)` from Janet.
- After a replacing load the file runs again in the same Janet environment (the load clears the
  import registry), so every line must survive a second run ([DEC-RELOAD](decisions.md#dec-reload)).

### The hook

Janet compiles a function body when the function is defined. A symbol that is not yet defined is a
compile error, and a function compiled before a later `defn` of the same name keeps calling the earlier
value: a function compiled before `.import binary-arithmetic` still raises `number literal &30 has no
representation` afterwards, while one compiled after it returns `&30` (evidence E1, E2).
`ace-drs`, `ace-verbalise` and `ace-zelph` are compiled before `ace-math`, so they MUST NOT name an
`ace/math-*` function (other than `ace/math-text`, which lives in `ace-verbalise`), `zelph/number` or
`zelph/int`. They call through the table that `ace-math` stores under `(ace/state :math)`:

| Key | Function | Called by |
|---|---|---|
| `:num!` | `ace/num!` | `ace/term-node!` for the integer term `(:num Text)` |
| `:term!` | `ace/math-term!` | `ace/term-node!` for an arithmetic term `(:math MT)` |
| `:plan` | `ace/math-plan` | `ace/plan`, for every box that holds a `formula/compare` formula or an `expr/arith` term, and for every implication that holds a formula |
| `:native` | a function from an `MT` to native zelph text | `ace/zelph-of` |
| `:numeral` | `ace/math-numeral`: a canonical number node → decimal text | `ace-verbalise` (number nodes in answers) |
| `:value!` | `ace/math-value!` | the check step (ground terms in atoms) |
| `:execute` | runs a math `:check` or `:ask-value` payload; a payload that already carries `:verdict` (identity) is answered without it | `ace/execute!` |

The table holds the `(zelph/epoch)` of its import and counts only while that epoch is current. A
replacing load takes the math stack with the graph (`Rules: 0` afterwards) while the Janet table
survives; from then on arithmetic is refused with `[ace/needs-math]` until `.import ace-math` runs
again, which it does, since the load cleared the import registry (DEC-MATH-IMPORT, DEC-RELOAD).
`ace/supported?` reads the table the same way. A hook call without a current table is
`[ace/internal]`: the refusal by the `:import "ace-math"` field of a construct row guarantees that no
arithmetic action reaches planning or execution without it.

### What works without `ace-math`

- `.import ace` (and `.import ace-drs`): every math sentence parses, gets its DRS and its construct ids
  (`ace/analyse` over all 69 math cases of the corpus). `:formula/identity` and `:copula/identity` are
  fully supported from M5 without arithmetic ([identity](#identity)). Every arithmetic id is refused
  with `[ace/needs-math]` naming `.import ace-math`, plain integers too (`:data/integer`, [DEC-DATA](decisions.md#dec-data) (b)).
- Strings need no arithmetic: they are character lists built in `zelph`, and the empty string is the
  internal node `ace:empty-string` (DEC-DATA (b)).
- The math vocabulary is absent: `great`, `small`, `large` are `ace-core`'s adjectives without their
  arithmetic reading, and `prime`, `divisor`, `derivative` are read like any word without an entry.
- No shared test fixture of non-math tests imports math: a sweep over 3733 corpus cases went from 82 ms
  to 93 s that way (an old measurement, kept as the reason).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John's age is 30.

    Error in line "ace John's age is 30.": Correct ACE, but zelph cannot use it [ace/needs-math]: sentence 1, line 1, column 12: a number, construct data/integer.
      John's age <>is 30.
      Numbers and arithmetic need the math stack: .import ace-math
    ```

After `.import ace-math` the same block is accepted and writes `John age &30`. The number is an
argument of the copula and has no token of its own, so the message points at `is`, the token of the
condition that holds it ([Positions](constructs.md#positions)). The message layout
(the host's `Error in line "…":` frame, the position of the token that is the evidence of the
construct, the marker `<>`, the hint lines) and the catalogue of message ids are [The ace
Block](block.md#messages)'s. On this page, as there, the ids, the layout and the positions are
normative and the wording is illustrative: it is data in `ace/messages`.

### Importing `ace-math`: language, reloads, the substrate

- **The import is explicit** ([DEC-MATH-IMPORT](decisions.md#dec-math-import) (b)) and pulls in `ace`.
  Arithmetic always parses; without the import it is refused with `[ace/needs-math]`, whose hint names the import. Importing math
  changes the whole session (nodes, rules, an inference cost on every later line), which should be the
  user's visible act and not the side effect of one sentence. An import from inside the handler became
  possible with the host work and is deliberately not used.
- **Language.** All 17 modules of the arithmetic stack begin with `.lang zelph`, and a module's `.lang`
  ends with the module (also when it stops at an error), so math imported under any language names its
  nodes in `zelph` ([Multi-language Support](../modules.md#multi-language-support)). Math imported
  earlier under another language is therefore the same stack, and the refusal `[ace/math-lang]` that an
  earlier draft needed for that case is withdrawn. `ace` executes its blocks under `.lang zelph` and
  resolves every math-stack node with an explicit `"zelph"` anyway. A native arithmetic statement typed
  under `.lang en` names `en` nodes and computes nothing.
- **After a load.** A replacing load takes the math stack with the graph and clears the digit alphabet,
  a display registration, so a typed `&12` then prints as a digit list `<0 0 1 1>`; Janet's
  `zelph/number` and the hook table survive. Arithmetic is `[ace/needs-math]` after the load, and
  `.import ace-math` runs `.import math` again (418 rules again; the node count depends on the loaded
  graph). Within one graph a second `.import ace-math` is skipped
  ([DEC-RELOAD](decisions.md#dec-reload)).
- **The substrate stays the user's choice**: `.import decimal-arithmetic` (under `.lang zelph`) before
  `.import ace-math`. One substrate per session: a second explicit substrate import warns `skipping
  import of 'binary-arithmetic'` and, in a script, ends the run with exit status 1, so no test or
  documentation example imports two.
- `.import math` as a statement line inside a module works. `(zelph/import "math")` from Janet is
  safe since the Janet update of the host work, but `ace-math` uses statement lines either way.

## Surface Syntax

The tokenizer and the grammar are [Tokenizer, Grammar and Lexicon](grammar.md)'s, including the one
rule for minus signs ([Minus and negative literals](grammar.md#minus-and-negative-literals)): a `-` in
operand position followed by a number is a negative literal (whitespace between them allowed), a `-`
after a complete operand is the binary operator, and `--` is one token no grammar rule accepts. Arithmetic depends on the following properties; each row is a test oracle.

| Requirement | Evidence |
|---|---|
| precedence `^` over `*` and `/` over `+`, `-` and `&`; every operator left-associative, `^` included: `2^3^4` is `(2^3)^4` | ACE's public interpretation rules; corpus 2406, a chain of `+` and `-` folded to the left (as `2 + 7 - 3 + 8` is `((2 + 7) - 3) + 8`); 3776, a power tower under a product (as in `3*2^3^2`) |
| negative literals: 2630, a measured value with a space after the minus sign, is accepted (`A change is - 2.5 kg.` gives `real(-2.5, kg)`); 3677 `0- -1 = 1.` is `0 - (-1)`; 3678 `0--1 = 1.` rejected; 2852 `-0` is `0`; 3524 `-007` is `-7`, `-002.200` is `-2.2`; 3592 `-.5` rejected; 1720 and 1721, a negative count (`-2 dogs bark.`, `At least -2 dogs bark.`), rejected (counts are unsigned) | corpus |
| a negative literal is atomic, so `-2^2` is (−2)² = 4; there is no unary minus on a variable or a bracket (`-X`, `-(X+1)` are not ACE) | ACE's grammar |
| the hyphen is a word character: `X-1` and `X1-Y` are one word each; `X - 1` is a subtraction; `3-5` is `3`, `-`, `5`, since the `-` follows a complete operand; `2-X` is read as `2 - X` by the tokenizer's rule (no corpus case and no public document decides it) | corpus 2370 (`3-5` inside a term); E12 |
| exactly six comparison operators: `=`, `\=`, `<`, `>`, `=<`, `>=`; `\=`, `=<`, `>=` are two characters each; no `<=`, `!=`, `/=`, `≠`, `≤` | 2851 `-0 /= -1.` rejected |
| variables are a capital letter, optionally followed by digits (`X`, `X1`); `x`, `a1`, `Xy` are neither variables nor terms | ACE's grammar |
| operator chains are typed (`+ - * / ^` numeric, `&` strings): 2372, a number times a list (`A total is 3*[4,5].`), is rejected; formulas are not typed: 3319, a set compared with a number (`{ Mondays, the World-Bank } > 3.`), is accepted (and refused later by construct id) | corpus |
| a formula is a sentence of its own that coordinates, negates, embeds and stands in conditions and consequences like any other (2387, 2386, 3722, 2360, 2369); a term is a noun phrase (2406, 3527: `John likes Sue + 2.` in its shape) | corpus |
| forms that follow from the grammar but have no corpus case, pinned by our own tests: `Is it true that 1 + 2 = 3?`, `What is 2 + 3?`, `For every integer X is it true that (X - 1) * (X + 1) = X^2 - 1?`, `For every integer X (X + 1)^2 = X^2 + 2*X + 1.`, `It is false that for every integer X (X + 1)^2 = X^2 + 1.`, `Is 5 greater than 3?`, `What is the derivative of X^3 - 2*X + 7?` | ACE's grammar |

**Why the island grammar is not embedded.** The `$( … )` island of `math-syntax`
([Notation](../math/frontend.md#notation)) reads `^` right-associatively with any factor as the
exponent, so `$( 2^3^2 )` is `&2 ^ (&3 ^ &2)` and simplifies to 512, where ACE's `2^3^2` is 64; `$( -2^2 )`
is −4, where ACE's is 4; the island reads a bare `X` as a zelph variable and `x-1` as a subtraction.
The island no longer fails on such terms, it computes another value, which makes the case for ACE's
own term grammar stronger. Where the meanings agree, the terms `ace` builds are node-identical to the
island's, and a quoted `"X"` reads back in an island as the named node.

## Typing and Construct Ids

A construct id names one construct of the language; a block either supports every construct it
contains or is refused naming the first one that is not supported ([Constructs and
Refusals](constructs.md), which lists every id; the arithmetic ones are in [Supported from
M5](constructs.md#supported-from-m5) and [Without a Route in v1](constructs.md#without-a-route-in-v1)).
This section owns the detection rules of the arithmetic ids and the detail of their rows.

### Typing

Typing is pure and runs over the resolved DRS of one paragraph.

- **Numeric data**: an integer `(:int …)`, a real `(:real …)`, an arithmetic term `(:expr Op …)` with Op
  one of `+ - * / ^`.
- **Non-numeric data**: a string, a list, a set, a string concatenation `(:expr "&" …)`.
- **Numeric referent** (the least fixpoint over the paragraph): a referent that is an operand of an
  arithmetic term; or an operand of a formula with `<`, `>`, `=<` or `>=`; or the first argument of an
  `object` condition `number` or `integer`; or one side of a `=` formula whose other side is numeric.
- A formula **side is numeric** iff it is numeric data or a numeric referent. A proper name is never
  numeric by itself: inside an arithmetic term a name is a symbolic constant whose value is unknown, so
  `Pi * X = X` stays undecided. The unique names of [DEC-NEQ](decisions.md#dec-neq) (b) concern
  identity formulas, which by this typing have no numeric side.
- **A numeric referent gets no class pattern from a relational noun**
  ([DEC-NUMERIC-REFERENT](decisions.md#dec-numeric-referent)). In "If a
  person's age is A and A > 17 then …" the age is numeric (an operand of `>`), so the of-relation
  gives the atom `X age Y` and no condition `Y ~ age`: no number is an instance of `age`, and with
  that condition the rule would never fire. This extends the rule of the of-relation row of [Atoms of
  Simple Conditions](semantics.md#atoms-of-simple-conditions), which writes no class atom on a data
  value, to referents that the typing makes numeric.
- A copula `be` between two sides of which neither can be substituted (the copula pass of [From
  Sentences to the Graph](semantics.md#normalization)) is typed like the `=` formula with the same
  sides: with a numeric side (`John is 5.`) it is `:formula/compare`, not `:copula/identity`; without one it is
  `:copula/identity`.

### The ids

| Id | Family | Detected when | Since | Needs `.import` | Rests on |
|---|---|---|---|---|---|
| `:formula/compare` | A | a formula with `< > =< >=` and no non-numeric data operand; or with `=`, `\=`, a numeric side and no non-numeric data side; also a comparative with an arithmetic reading and a numeric side ([vocabulary](#vocabulary)) | M5 | `ace-math` | P-number-lang; P-drop-delta, P-scratch-drop, P-drop-names; P-quiet-run (a); P-parts; the evaluation module |
| `:formula/identity` | A | a formula with `=` or `\=` and no numeric side | M5 | — | P-lookup; [identity](#identity) |
| `:copula/identity` | basic | a copula `be` with neither side substitutable and no numeric side | M5 | — | as `:formula/identity`; mapped as the `=` formula |
| `:formula/non-numeric` | A | a formula with a non-numeric data operand and either `< > =< >=` or a numeric other side | — | — | no route: zelph has no order on strings, lists or sets |
| `:expr/arith` | A | an arithmetic term with `+ - * / ^` | M5 | `ace-math` | as `:formula/compare` |
| `:expr/concat` | A | a term with `&` | — | — | no route: strings as character lists would need a concatenation rule set |
| `:expr/as-data` | A, level context | an arithmetic term without an integer value used as an argument of a stored atom (`John likes Sue + 2.`, `John likes 7 / 2.`): found by `ace/plan` when the term holds a name or a variable, and by the check step when a ground term has no value | — | — | no route in v1 (below) |
| `:neg/formula` | N | a classical negation that is a [negated formula](#negated-formulas) | M5 | — | — (the `formula/` ids inside carry the requirements) |
| `:naf/formula` | F | a "not provably" box in a condition or a question that is a negated formula | M5 | — | as `:neg/formula` |

The host item ids (P-…) are described on [What zelph Already Provides](foundation.md); all of them are
built and part of zelph 1.1.0. The evaluation module is M5's own work. Reals and measured numbers are
refused earlier, by `:data/real` and `:data/measured`. Every `formula/` and `expr/` id is family A, which keeps the corpus count A = 59 that the classifier must
reproduce (the 45 accepted formula cases plus the 14 accepted cases with a term and no formula);
`:neg/formula` and `:naf/formula` keep N = 509 and F = 19. The comparative evidence of
`:formula/compare` needs the arithmetic reading of a word, which only `ace-math`'s shipped vocabulary
gives; the corpus classification never has it (the corpus lexicon hides every shipped reading, and
the classification runs without `ace-math`), so 2123 and 2297, comparatives between two numbers, add
nothing to those counts.

**`:expr/as-data` has no route in v1** ([DEC-DATA](decisions.md#dec-data)). A stored term would be a
request (invariant 2), and it cannot be stored without being claimed: a plain statement claims every
fact nested in it, and only an argument position of an opaque predicate keeps a nested statement
unclaimed ([DEC-MODAL](decisions.md#dec-modal) (d)). Since ACE `/` maps to `exquo`, a stored term
would no longer derive a wrong value (`exquo` is silent where native `/` truncated); storing such terms
as terms is left for a later round ([not in v1](#not-in-v1)).

### Rows of the construct table

The rows that `ace-constructs.zph` holds for these ids. Examples must be accepted once the id is
supported; negatives must be rejected. Summaries and reasons are data, worded by us. The native forms
use the variable letters of this page.

| Id | Summary | Examples | Negatives | Corpus | Native form |
|---|---|---|---|---|---|
| `:formula/compare` | an arithmetic comparison | `1 + 2 = 3.`, `Is it true that X \= 0?`, `If a person's age is A and A > 17 then the person is an adult.` | `1 <= 2.`, `0--1 = 1.` | 2384 2385 2386 2387 2448 2739 2852 3677 | `((L compare R) = gt)` |
| `:formula/identity` | an equation between names or individuals | `X = X.`, `John \= Mary.`, `If John = John then John = John.`, `If a man X likes a man Y and X \= Y then X is a friend of Y.` | `John /= Mary.` | 3254-3257 3262-3266 3633 3656 | `X != Y` |
| `:copula/identity` | one thing named twice | `John is John.`, `Moon is the Moon.`, `John is not Mary.` | — | 2810 3751 3778 | none: decided, nothing stored |
| `:formula/non-numeric` | a comparison of something that is not a number | — | — | 3319 | — |
| `:expr/arith` | an arithmetic term | `A total is 2 + 7 - 3 + 8.`, `What is 2^3^2?`, `John's age is 29 + 1.` | `A total is 3*[4,5].` | 2406 2423 3771 3776 | `(&2 * (&3 ^ &4))` |
| `:expr/concat` | joining strings with & | — | — | 2873 2874 2875 3364 3669 | — |
| `:expr/as-data` | a term without a whole-number value used as data | — | — | 3525 3526 3527 | — |
| `:neg/formula` | a negated formula | `It is false that 2 = 5 - 4.`, `It is false that John = Mary.`, `If it is false that X = X then X = X.` | — | 2358 2386 3259 3260 3261 3262 3263 | the formula's mapping, polarity flipped |
| `:naf/formula` | a formula that is not provable | `If a person's age is A and it is not provable that A > 18 then the person is a minor.` | — | — | `¬((Y compare &18) = gt)` |

The reasons of the rows without a route are "zelph has no order on strings, lists or sets", "zelph
cannot join strings yet" and, for `:expr/as-data`, "zelph stores a term by its value, and this term
has no whole-number value". All wording lives in `ace/messages`.

### The math corpus cases at M5

Computed from the rules of this page; the gate counts are [Tests and Quality Gates](tests.md)'s.

| Outcome | Cases |
|---|---|
| accepted, nothing stored | 2384 2385 2386 2387 2448 2739 3677 (true checks); 2394 (a numeric `X = X`), 3633 (an identity), 3656 (a definition); 3771 3773 3774 3775 3776 (definitions); 3254 3255 3256 3257 3264 (rules that always hold); 2406 2423 (a copula onto a data value) |
| accepted with `[ace/w-vacuous]`, nothing stored | 3259 3260 3261 3266 (an identity condition decided false) |
| accepted with `[ace/w-not-checkable]`, nothing stored | 2370 (inexact `/`), 2396 (an inequality over an existential natural number) |
| accepted, rules stored | 2360, 2367 (a seed and a denial each, [formulas in rules](#formulas-in-rules)) |
| refused by the context id `:expr/as-data` (in the graph layer only; the text-to-DRS layer accepts them) | 3525 3526 3527 (`John likes Sue + 2.` in their shape) |
| rejected with `[ace/contradiction]` | 2852 (`-0 = -1`); 3262 3263 3265 (an identity in a head decided false: different names) |
| refused at M5 | 2358 2363 (`:object/group`, M7), 2369 2393 (`:object/at-least`, M7), 3319 (`:data/set`, M7; then still `:formula/non-numeric`), 2359 3722 (`:sub/that-clause`, M8), 2362 (`:modal/can`, M8), 2839 (`:modal/must` and more), 2388 3712 (`:or/in-head-or-statement`), 3596 3598 3772 2364 (`:data/real`; 3598 is three paragraphs, fed as three blocks, and the first, `Pi = 3.14.`, is refused; in strict mode the text is rejected as a whole), 2624 2625 (`:data/measured`), 2873 2874 2875 3364 3669 (`:expr/concat`) |

The identity copulas outside this slice behave as [identity](#identity) says: 3751, a name and the same
name with the article (`Moon is the Moon.` in its shape), is accepted and stores nothing; 2810, two
different place names (`Berlin is not Paris.`), and the eight `X is not Y.` of 3778 are true and store
nothing.

## Negated Formulas

"It is false that 2 = 5 - 4." negates a formula, not a statement about the world, so it must not
become a denial rule: it is checked, found true and stores nothing (the [session on
statements](#statements) shows it). The classifier therefore recognises a negated formula before it
applies its general rules for negation.

A classical negation `(:neg D)` anywhere, or a "not provably" box `(:naf D)` in a condition or a
question, is a **negated formula** when D holds at least one formula and otherwise only *type
conditions* — `object` conditions `something`, `somebody`, `number`, `integer`, and the property
`natural` — on referents that D declares and uses only in its formulas. A copula `be` that the copula
pass leaves in D counts as its `=` formula (with a numeric side as a comparison), so `Berlin is not
Paris.` (the shape of 2810) is a negated formula and yields `:neg/formula`. At top level and in a
question box any number of formulas qualifies; in an if-box or a then-box exactly one.

- A negated formula reports the `formula/` ids of its formulas plus `:neg/formula` (family N) or
  `:naf/formula` (family F), and **no other** `neg/` or `naf/` id. Otherwise `It is false that
  2 = 5 - 4.` (the shape of 2386) would be `:neg/statement` and become a denial.
- A "not provably" box at top level or in a then-box stays `:naf/top-level`, refused like every
  top-level "not provably" ([DEC-NEG](decisions.md#dec-neg) (a)).
- **Family counts** ([Family Counts](constructs.md#family-counts)). One id of the operator's family
  per negation keeps the classifier's counts: seven accepted cases have their N evidence only in
  negated formulas (2358 2386 3259 3260 3261 3262 3263);
  without an N id, N would fall from 509 to 502. The copula cases add more, 2810 and 3778 among them.
  The one F case whose only "not provably" box holds just a formula (2839) has it in a then-box, so it
  keeps F through `:naf/top-level`.
- **Routing.** `ace/plan` hands both ids to this page's mapping — identity, statements, questions, rule
  conditions and consequences — never to a denial (`:neg/statement`) or to a Lloyd-Topor helper rule
  (`:naf/complex-in-body`).
- **Order.** The classifier tests for a negated formula **before** its generic rules for `(:neg D)`
  and `(:naf D)`.

## Referents in Formulas

### Definitions

A positive `=` formula is a **definition** when one side is a referent R declared in the box where the
formula stands (the top-level box, a then-box, a question box or an if-box — for an if-box also any
if-box that encloses it in the same nest of conditionals), R does not occur in the other side T, and
the formula is not inside a negated formula. Then R := T everywhere in R's scope (for a rule: the whole
nest), and the formula is deleted. When both sides are such referents, the later-declared one is
replaced by the earlier one. Substituting in the whole nest is sound both for flattened rules and for
the rule generators of [DEC-NESTED-IMP](decisions.md#dec-nested-imp) (c): a generator whose outer
variables are unified generates exactly the inner rules that could fire. Afterwards an `object` condition that stands on a data value
is deleted, as the copula pass does for `be`; any other condition stays on T (`There is a man X.
X = John.` gives `John ~ man`).

This is a normalisation step: the copula pass of [From Sentences to the
Graph](semantics.md#normalization), extended from `be` to `=`. Its consequences: `A = 2*3^4. What is
A?` asks for the value of `2*3^4`; `If John = X then …` (3262) substitutes X := John; `"" = X.` (3656) leaves nothing to store. A
definition holds within its block only: the next block's `A` is a new referent.

### Kinds of referent

What a referent left in a `:formula/compare` formula becomes:

| Kind | Recognised by | Term (`MT`, below) | In the scratch cluster |
|---|---|---|---|
| rule variable | declared in an if-box or an outer box of a rule, and bound by a positive condition that is not a formula | `(:var Name)`, named like every rule variable | — |
| constant C | a proper name, or a top-level referent the plan makes an individual | `(:const Term)` | `~ symconst`, placed in the ring order |
| math variable E | a top-level or question-box referent whose only conditions are type conditions and formulas | `(:ind Name)` | `~ symvar`, placed in the ring order |
| math variable U | an antecedent referent of a math-universal implication | `(:ind Name)` | `~ symvar` |
| unbound | a rule referent that occurs in a comparison and is bound only by comparisons and type conditions (`If X > 2 then X > 1.`; `For every integer X if X > 2 then John waits.`) | — | refused by `:rule/unsafe` |

The last row is a requirement on the safety check of [From Sentences to the
Graph](semantics.md#static-checks): a referent of a comparison, in the body or in a head, counts as
used like one in a head atom;
comparisons never bind; and type conditions do not bind a variable of a comparison, since no fact
`X ~ integer` exists for a number, so such a rule would never fire. A math-universal implication is
planned by the hook and never reaches this check.

The `Name` of a math variable is the ACE variable as written (`X`, `X1`), carried by the referent's
pre-resolution marker. The type conditions of math variables are consumed: such a referent is never a
top-level individual.

## Identity

"John is Bill." and "John = Bill." say that two names name one thing. ACE gives both the same
identity DRS, and `ace` reads both the same way: the copula pass hands every copula `be` with neither
side substitutable and no numeric side to this section as an `=` formula, and a negated one (`Berlin
is not Paris.`) as a negated formula. Identity needs no arithmetic: everything here works with `.import
ace` alone.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John is a man. Moon is the Moon. John \= Mary. Berlin is not Paris.
    Is John Mary? Is it true that John = John?

    Answer: No. It is false that John is Mary.
    Answer: Yes. John = John.
    zelph> ace John is Bill.

    Error in line "ace John is Bill.": Contradiction [ace/contradiction]: sentence 1, line 1, column 1: "John" is the node John and "Bill" is a new name; different names name different things, so this cannot be true.
      <>John is Bill.
      To give one thing a second name, declare the name before using it: ace-lexicon pn_sg('Bill', 'John', masc).
    ```

The first block stores one fact, `John ~ man`; the three identity statements are true and store
nothing. The second block is rejected before anything is written: `John` is a node and `Bill` a new
name, and two names are two things unless the lexicon binds them to one node (`ace` never merges
nodes by name, and never calls `.name`: [DEC-NAME-MERGE](decisions.md#dec-name-merge)).

**The sides.** After the definitions above, each side of a remaining identity formula is one of:

- a **named constant**: a proper name after resolution (an `ace` name, or a Wikidata item reached by
  id or by label, see [Resolving a Word](words.md#resolving-a-word)), a string (the empty string
  included) or a number;
- an **individual**: a top-level individual (a node named `<Head>-<16 hex digits>`) or a witness that
  a rule generated. Individuals are not names: two descriptions may denote one thing, and ACE does not
  entail that "a man" is not John;
- a **variable**: a rule or query variable.

**When two sides are identical.** Two sides are identical when they are the same referent or resolve
to one node. Two named constants that resolve to different nodes are **distinct** (unique names,
[DEC-NEQ](decisions.md#dec-neq) (b)). A name not yet in the graph is compared by its node name (its
canonical symbol, a quoted word verbatim), so `John = John.` holds and `John = Bill.` is false on an empty graph, and two
spellings of one Wikidata item (`p:Q42`, `Douglas Adams`) are identical. An identity with an
individual on a side is true when both sides are one node and **unknown** otherwise.

**When it is decided.** `ace/plan` stays pure: it decides what needs no graph (the same referent, equal
strings, the same canonical symbol) and leaves every other ground identity to **the resolution step of
`ace/execute!`**, which runs before the dry run and creates nothing: it resolves both names and
compares the nodes. A plan carries an undecided ground identity as `(:check {:kind :identity …})` in a
statement, as `(:ask-value {:mode :decide :kind :identity …})` in a question box, and as the pattern
`(:same A B)` in a rule's body or heads; the resolution step gives each its verdict before either run
(`:verdict` in the payload; the pattern is replaced), so an identity never reaches the hook.

| Where | `A = B` | `A \= B` |
|---|---|---|
| statement (top level) | identical: nothing stored; distinct named constants: `[ace/contradiction]`, block rejected; an individual and another node: `[ace/unsupported]` (`ace` never merges two existing nodes) | identical: `[ace/contradiction]`; otherwise nothing stored (two nodes are distinct for every `!=` guard) |
| if-box | a variable involved: substituted (definition); identical: condition dropped; otherwise the rule can never fire: not stored, `[ace/w-vacuous]` | a variable involved: the guard `(:ne A' B')`, whose variables a positive condition must bind (else `:rule/unsafe`); identical: not stored, `[ace/w-vacuous]`; otherwise the condition is dropped |
| then-box | a rule variable involved: the denial `(body…, (:ne A' B')) => !`; identical: head dropped; distinct named constants: the rule becomes the denial `(body) => !`; an individual and another node: `[ace/unsupported]` | rule variables: the denial over the body with B := A; identical: `(body) => !`; otherwise the head is dropped |
| question box | substituted (definition) or decided: identical: `Yes.`; distinct named constants: `No.`; an individual and another node: `It is not provable that …` | a query variable: a `(:ne …)` pattern; decided otherwise: identical: `No.`, distinct named constants: `Yes.`, an individual and another node: not provable |

- Rule conditions are closed-world over nodes, as zelph's `!=` is: `\=` over variables is a `!=` guard
  that holds for two different nodes even where they might denote one thing, and a ground condition
  with an individual is judged the same way. The user page `ace.md` says so (DEC-NEQ).
- A rule whose body is decided true becomes its heads as statements; a rule with an empty body whose
  head is decided false is a statement decided false: `[ace/contradiction]` (3262, 3263, 3265). A rule
  with no head left stores nothing (3254-3257, 3264).
- The messages of these cases (the last one for "If John is Bill then John waits."):

  ```
  Contradiction [ace/contradiction]: sentence 1, line 1, column 1: "Mark-Twain" is Q… (Mark Twain) and "Samuel-Clemens" is a new name; different names name different things, so this cannot be true.
    <>Mark-Twain is Samuel-Clemens.
    To give one thing a second name, declare the name before using it: ace-lexicon pn_sg('Samuel-Clemens', 'Q…', masc).
  Correct ACE, but zelph cannot use it [ace/unsupported]: sentence 1, line 1, column 22: this says that Man-<16 hex> and John are one thing; ace does not merge two existing nodes. Construct copula/identity.
    Man-<16 hex> <>is John.
  Warning [ace/w-vacuous]: sentence 1, line 1, column 9: "John is Bill" can never be true, because different names name different things, so this rule never applies. It was not stored.
  ```

  The refusal and the warning point at the copula, the token of the identity condition; the column 22
  assumes the 16 hex digits of a real individual name. Wikidata's aliases are not imported, only its
  labels ([DEC-WIKIDATA](decisions.md#dec-wikidata)), so an alias is a new name until it is declared
  with the item's id as its symbol.
- The same machinery carries "at most", "less than" and the upper bound of "exactly" (M7,
  [DEC-PLURALS](decisions.md#dec-plurals)): denials over witnesses kept pairwise apart by `!=` that are named constants — every individual `ace`
  creates carries the internal mark `I ace:individual nil`, and the denial skips it with
  `¬(W ace:individual nil)` — or variables the user keeps distinct with `\=`. A witness the engine
  generates for a rule head carries no mark and counts as distinct there, unlike in an identity
  formula: closed-world for witnesses, documented with "Every man owns a red car. Every man owns a
  blue car. John is a man. John owns at most 1 car."

The run shows the two native forms on today's engine, typed under `.lang ace`, where `ace` keeps its
words. "If somebody X likes somebody Y and X \= Y then X is generous." becomes a rule with a `!=`
guard: Tom, who likes himself, is not generous, John is. "If somebody X marries Ann then X = Tom."
becomes a denial: a second husband is a contradiction.

```
zelph> .lang ace
ace> ((X likes Y), (X != Y)) => (X is generous)
ace> Tom likes Tom
ace> John likes Mary
(John is generous) ⇐ {(John likes Mary) (John != Mary)}
ace> X is generous
Answer: John is generous
ace> ((X marries Ann), (X != Tom)) => !
ace> Tom marries Ann
ace> Bill marries Ann
! ⇐ {(Bill != Tom) (Bill marries Ann)}
Found one or more contradictions!
```

## Math Terms in Plans

A plan is the list of actions `ace/plan` computes from a DRS; plans hold names, never nodes, so that
planning needs no graph ([Plan](api.md#plan) on Modules, API and Data Shapes). Arithmetic appears in a
plan as the term `(:math MT)`, also in the predicate position of an atom.

### The term `MT` and the formula `MF`

```
MT = (:num Text)            canonical decimal integer ("162", "-3", "0")
   | (:op Op MT MT)         Op one of "+" "-" "*" "/" "^", ACE's operator ("/" is built as exquo)
   | (:ind Name)            math variable (indeterminate), Name = the ACE variable ("X", "X1")
   | (:const Term)          constant; Term (:name N :name), (:id Id :name) or (:skolem …)
   | (:var Name)            rule or query variable
   | (:req K MT …)          request, K one of :value :quot :diff :cmp, or :testprime :diffby (vocabulary)
   | (:sym Name)            any other node of the math stack, by its name ("=", "gt", "eq", "lt", "prime", "composite")
MF = (:f Op MT MT)          Op one of "=" "\\=" "<" ">" "=<" ">=", as in the DRS
```

The request keys map to the evaluation module's names in **one table** in `ace-math.zph`
(DEC-MATH-MODULE):

| Key | Built as | Answer |
|---|---|---|
| `:value` | `(T simplify T)`, that is `(:simplify T)` | `= V`; V is a value iff it is a natural numeral or `(neg zint N)` |
| `:quot` | `(A exquo B)` | `= Q` |
| `:diff` | `(L difference R)` | `= D`, the polynomial normal form |
| `:cmp` | `(L compare R)` | `= lt`, `eq` or `gt` |

```
(:math (:op "*" (:num "2") (:op "^" (:num "3") (:num "4"))))
(:atom (:math (:req :cmp (:var "Y") (:num "17"))) (:math (:sym "=")) (:math (:sym "gt")))
(:f ">=" (:op "*" (:num "3") (:op "-" (:num "2") (:op "/" (:num "4") (:num "13")))) (:op "/" (:num "1") (:num "3")))
```

### From the DRS to `MT`

| DRS argument | `MT` |
|---|---|
| `(:int N)` | `(:num N)`, already canonical: `-0` gives `"0"`, `-007` gives `"-7"` |
| `(:expr Op A B)`, Op one of `+ - * / ^` | `(:op Op A' B')`; the DRS is already folded to the left |
| a referent | by its kind ([kinds of referent](#kinds-of-referent)) |
| `(:named N)` | `(:const T)`, T the name term `ace/plan` makes for it: `(:name N' :name)` with N' the node name, or `(:id Id :name)` for an explicit Wikidata id |
| `(:real …)`, units, strings, lists, sets, `&` | never: refused before planning |

### Nodes and the naming language

`(ace/math-term! mt resolve)` builds the node of an `MT` inside the active cluster (a scratch cluster or
a rule-building thunk). `resolve` is a function the caller passes (`ace/term-node!` with its execution
context), used for `(:const …)` and `(:var …)`.

- `(:op …)` gives `(zelph/fact A' Op' B')` with Op' the node `+`, `-`, `*` or `^` of the same name, or
  `exquo` for `/`; `(:ind …)` gives `ace/math-indet-node!`; `(:req …)` the request fact of the key
  table; `(:sym …)` and every operator `(zelph/resolve name "zelph")`.
- Inside a rule-building thunk only `zelph/fact`, `zelph/var`, `zelph/resolve` and the number builders
  are used: the host refuses queries, cluster switches and imports there, and rules never hold math
  variables, so no ring order is built in a thunk.
- **Naming language.** The math stack's nodes are named in `zelph`, because `ace-math` imports math
  there. Under `.lang en`, `(zelph/resolve "+" "en")` and `(zelph/resolve "gt" "en")` are other nodes
  and a typed `? &1 + &2` answers nothing. `ace` runs its execute step under `.lang zelph`
  ([DEC-CONTENT-NS](decisions.md#dec-content-ns)) and still resolves every math-stack node with an
  explicit `"zelph"`, never through the string arguments of `zelph/fact`.
- **`(ace/num! text)`**: `text` is canonical decimal; the result is `(zelph/int text)`, which is `&n` for
  n ≥ 0 and `(neg zint &n)` for n < 0 on every substrate: `-0` gives `&0`, `007` gives `&7`, `-3` gives
  `(neg zint &3)`. Leading zeros are canonical on the binary, decimal and NAND substrates.
- **Numbers and `.lang`** ([DEC-MATH-LANG](decisions.md#dec-math-lang) (a), host item P-number-lang).
  Number literals and the characters of `zelph/list-chars` are language-independent, resolved in `zelph`, so a native `&30`
  typed under `.lang ace` or `.lang en` is the node `ace` wrote, and strings meet likewise
  ([Number Literals](../logic.md#number-literals)). `ace` never rebuilds numbers itself: that would be a
  second implementation of `zelph/number`.
- Island negatives `(neg of &3)` are another node; they meet canonical numbers only through comparison.

### Arithmetic terms inside atoms

A term is stored **only by its value**. Every subterm of a stored fact is a claim, and a claimed
arithmetic term is a request: `john likes (&7 / &2)` derives the truncated `(&7 / &2) = &3`, plus the
intermediate states of the math stack (E10). A stored term would litter the graph and, with native
`/`, lie.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace-math
    zelph+> ace John likes 7 / 2.

    Error in line "ace John likes 7 / 2.": Correct ACE, but zelph cannot use it [ace/unsupported]: sentence 1, line 1, column 6: a term without a whole-number value used as data ('7 / 2'), construct expr/as-data.
      John <>likes 7 / 2.
      zelph stores a term by its value, and this term has no whole-number value.
    ```

The graph is unchanged afterwards; in particular no `exquo` fact is left (the value was sought in a
dropped scratch cluster). `John's age is 29 + 1.`, whose term has a value, is accepted and writes
`John age &30`.

- **Atoms of statements** (facts and refutations): an `(:math MT)` argument whose `MT` holds no math
  variable, rule variable or constant is evaluated **once, in the check step** (the math checks of the
  resolution step, before the dry run): one `ace/math-value!` call takes every such term of the block's
  write actions, in one `ace:math` scratch cluster and one run (`:simplify` and the shape test of
  [the scratch protocol](#the-scratch-protocol)), and each term is replaced by `(:math (:num V))`; the
  dry run and the real run both use the rewritten plan. So `John's age is 29 + 1.` writes `John age
  &30`, which meets a native `John age &30` typed under `.lang ace`. A ground term without a value
  (`John likes 7 / 2.`, `A = 7 / 0. John likes A.`) is refused in the check step with
  `[ace/unsupported]`, construct `:expr/as-data`; a term that holds a name, a variable or a math variable
  (`John likes Sue + 2.`, the shape of 3527) is refused by the same id from `ace/plan`, since it can
  have no value. The message points at the condition that holds the term, the verb `likes`
  ([Positions](constructs.md#positions)), and names the term.
- **Atoms of rules**: a ground term is evaluated in the check step as above. A term T that contains a
  rule variable is replaced by a fresh rule variable V, bound by the **guarded value binding**: the
  conditions `((:simplify T') = V), ((V compare &0) = W)` with the seeds `(A) => (:simplify T')` and
  `(A, ((:simplify T') = V)) => (V compare &0)`, A being the rule's other conditions. The guard makes V
  an integer: where nothing reduces, `:simplify` answers the unreduced term, and without the guard the
  head would store it (E17: `kid halfage (&9 exquo &2)`, and for an age that is a string,
  `bob nextage (<c b a> + &1)`). `compare` has a verdict only for a constant difference, so W exists iff
  V is a number (E16).

**Open for M5:** whether the `:show-zelph` lines of a block with a ground term render the term as
written (`John age (&29 + &1)`, through the `:native` hook, which is what `ace/zelph-of` gives for the
plan `ace/plan` returns) or the value the check step put into the plan (`John age &30`, the fact the
block writes). The specification fixes neither, and RT-3 ([Native Equivalence of the Shown
zelph](tests.md#rt-3-native-equivalence-of-the-shown-zelph)) checks lines that name the math stack by
the same-graph oracle only, so it does not decide either.

## Math Variables and the Ring Order

A math variable such as the `X` of `Is it true that X \= 0?` is not a rule variable: it is an
*indeterminate*, a symbol the polynomial layer computes with ([Polynomial Normal
Forms](../math/polynomial.md)).

**`(ace/math-indet-node! name)`** ([DEC-MATH-INDET](decisions.md#dec-math-indet) (a)). The node of an
ACE math variable is the node named as written, in `zelph`: `(zelph/resolve name "zelph")`, the only place where this is decided.
After `.import math`, resolving `"X"` twice gives one node, which displays as `"X"`; the native variable
`X` stays a variable; and the island `$( "X" + 1 )` reads back as the same node (E19). So
`ace/zelph-of` renders an ACE math variable as `"X"`, and its lines re-enter.

**`(ace/math-order! inds consts)`.** Two opposite `pouter` declarations would give one term two normal
forms, and they are a contradiction of the polynomial layer, `(V pouter W, W pouter V, V != W) => !`
([Variable order](../math/polynomial.md#variable-order)). Inside the request's scratch cluster:

1. S = the request's math-variable and constant nodes.
2. Read the existing order with exact probes, `(zelph/exists a "pouter" b :exact)` for each ordered pair
   of S (host item P-exact), with `pouter` resolved in `"zelph"`. The polynomial layer closes `pouter`
   transitively, so direct probes see the whole order; S is small, so this costs |S|² constant-time
   probes and no query.
3. Sort S topologically by that order, ties by name. If the existing order holds a cycle (the user's own
   opposite declarations), the decision is unknown, with `[ace/w-not-checkable]`.
4. For each consecutive pair a, b with neither `a pouter b` nor `b pouter a`: assert `a pouter b`.
   Declare `~ symvar` (math variables) or `~ symconst` (constants). Direct facts work without a
   `polyring` list, beside a native `<y x> ~ polyring` too.

Consecutive pairs of a linear extension never create a cycle. Nothing persists: the cluster is dropped.

The run shows the math stack doing, natively, what a decision over a math variable rests on: `"X"` is a
node and the variable `X` still a variable; the normal form of `(X + 1)^2`; the difference
`(X + 1)^2 - (X^2 + 1)`, which is `2*X`, the evidence in the universal answer of the opening session;
and the polynomial identity check of [topoly](../math/topoly.md#three-outcomes), which proves
`(1 + X) * (1 - X) = 1 - X^2` and disproves `(X + 1)^2 = X^2 + 1`. A normal form `("X" poly <c0 c1 …>)`
lists its coefficients, least significant first.

```
zelph+> "X" ~ symvar
zelph> X ~ symvar
Answer: "X" ~ symvar
zelph> ? :topoly (("X" + &1) ^ &2)
Answer: (:topoly (("X" + &1) ^ &2)) = ("X" poly <(pos zint &1) (pos zint &2) (pos zint &1)>)
zelph+> ? :topoly ((("X" + &1) ^ &2) - (("X" ^ &2) + &1))
Answer: (:topoly $( ("X" + &1) ^ &2 - ("X" ^ &2 + &1) )) = ("X" poly <(pos zint &0) (pos zint &2)>)
zelph+> ? ((&1 + "X") * (&1 - "X")) ≡ (&1 - ("X" ^ &2))
Answer: (((&1 + "X") * (&1 - "X")) ≡ $( &1 - "X" ^ &2 )) = proven
zelph+> ? (("X" + &1) ^ &2) ≡ (("X" ^ &2) + &1)
Answer: ((("X" + &1) ^ &2) ≡ $( "X" ^ &2 + &1 )) = disproven
```

## The Evaluation Module

`stdlib/evaluation.zph` is a general stdlib module ([DEC-MATH-MODULE](decisions.md#dec-math-module)
(b)): it carries no ACE knowledge, `math.zph` does not import it, `ace-math.zph` does, and anyone else
may. It has its own documentation page, `mkdocs/docs/math/evaluation.md` in the mathematics section,
and its own tests,
`src/test/test_evaluation.cpp`, over the three substrates. None of these files, and no stdlib node
named `exquo`, `difference` or `compare`, exists yet. The names follow the two request idioms of the
standard library ([How to read a request](../math/reference.md#how-to-read-a-request)): a binary
request answered under `=` (like `cmp`) and a self-fact request (like `:simplify`).

| Request | Answer | Silent when |
|---|---|---|
| `(A exquo B)` | `= Q`, the exact quotient, canonical, all sign cases (the name and meaning of `exquo` in Axiom/FriCAS and SymPy) | B does not divide A; B = 0; an operand is not an integer |
| `(:simplify T)` | the simplifier's normal form; for a ground T built from integers with `+ - * ^` and `exquo`: its value, `&n` or `(neg zint &n)` | a `exquo` with a zero divisor anywhere in T (the module withholds its reduced form) |
| `(L difference R)` | `= D`, the normal form of L − R: `(:topoly (L - R))`, and through `(:simplify L)` and `(:simplify R)` where `exquo` occurs | no normal form |
| `(L compare R)` | `= lt`, `eq` or `gt` when D is a constant (Haskell's `compare`, in the lower case of zelph's `cmp`) | D not constant, or absent |

- `compare` derives **only** the verdict fact, not the relational facts `<`, `>`, `==` that `cmp`
  derives, so ACE comparisons feed no meta-rule over `<`.
- The name `difference` avoids `diff`, which means differentiation in zelph (`diffby`, `diffalong`).
- `(zelph/no-selffact-sugar "exquo" "difference" "compare")` is required: without it
  `(&17 compare &17)` renders as `(:compare &17) = eq`.

**What the module adds, and why no more.** zelph's simplifier already folds ground `+ - * ^` across ℕ
and ℤ to one canonical value (`(:simplify $( &2 - &5 + &1 )) = (neg zint &2)`), so a value request of
the module's own would duplicate it. Native zelph still lacks what ACE needs: an exact division (natural
`/` truncates, and a `/` with a negative operand derives nothing, [Integers over ℤ](../math/integers.md)),
and a three-way verdict over the difference normal form. The module therefore holds `exquo`, its hook
into the simplifier, `difference` and `compare`: 23 rules.

**Checked against the simplifier.** The simplifier's rule SU, `(T simp A, T simp B, A != B) => !`, makes
every term single-valued ([The Simplification Core](../math/symbolic.md#the-simplification-core)). The
module gives `exquo` a decomposition and a congruence rule like the other operators; the simplifier's
folding rule then folds an exact quotient to its value, and an inexact one keeps its reduced form
through the simplifier's fallback. The rule set stays conflict-free: `exquo` answers at most once (one
rule per sign case, the natural case by numeral pattern), no rewrite of the simplifier matches an
`exquo` node, and the two routes of `difference` reach one canonical normal form, one node. The
congruence withholds the reduced form of a quotient whose divisor simplifies to `&0`, so the whole
term gets no normal form — partiality by absence, as the simplifier does for undeclared atoms — and a
formal identity (`X * &0 → &0`, `X - X → &0`) can never give a division by zero a value. An inexact
quotient can still be removed by such an identity (`(:simplify ((&7 exquo &2) * &0)) = &0`), which is
true over ℚ, the meaning of ACE's `/`. No request of the module triggers a contradiction: none in 51
requests on three substrates and 12 symbolic ones (E13, E14). Rule SU does fire when the user's graph
declares an equation that disagrees with the arithmetic; [trust](#trust) handles that (E18).

The module's starting text (the implementation adds documentation comments and doctests; a rule
written over two lines in the original is one line here):

```
.import math

# exquo -- exact quotient. Natural operands: exact iff the remainder is zero; silent for a zero divisor.
((A cons R) exquo (B cons S)) => ((A cons R) mod (B cons S))
((A cons R) exquo (B cons S)) => ((A cons R) / (B cons S))
((A cons R) exquo (B cons S), ((A cons R) mod (B cons S)) = &0, ((A cons R) / (B cons S)) = Q) => (((A cons R) exquo (B cons S)) = Q)
# a negative dividend
((neg zint A) exquo (B cons S)) => (A exquo (B cons S))
(((neg zint A) exquo (B cons S)), (A exquo (B cons S)) = Q, Q != &0) => (((neg zint A) exquo (B cons S)) = (neg zint Q))
(((neg zint A) exquo (B cons S)), (A exquo (B cons S)) = &0) => (((neg zint A) exquo (B cons S)) = &0)
# a negative divisor
((A cons R) exquo (neg zint B)) => ((A cons R) exquo B)
(((A cons R) exquo (neg zint B)), ((A cons R) exquo B) = Q, Q != &0) => (((A cons R) exquo (neg zint B)) = (neg zint Q))
(((A cons R) exquo (neg zint B)), ((A cons R) exquo B) = &0) => (((A cons R) exquo (neg zint B)) = &0)
# both negative
((neg zint A) exquo (neg zint B)) => (A exquo B)
(((neg zint A) exquo (neg zint B)), (A exquo B) = Q) => (((neg zint A) exquo (neg zint B)) = Q)

# exquo in the simplifier: decompose, congruence (no reduced form over a zero divisor), SN folds the value
((U exquo V) needssimp (U exquo V)) => (U needssimp U)
((U exquo V) needssimp (U exquo V)) => (V needssimp V)
((U exquo V) needssimp (U exquo V), U simp P, V simp Q, Q != &0) => ((U exquo V) red (P exquo Q))

# difference -- normal form of L - R, directly and through the simplified sides
(L difference R) => (:topoly (L - R))
(L difference R, (:topoly (L - R)) = D) => ((L difference R) = D)
(L difference R) => (:simplify L) (:simplify R)
(L difference R, (:simplify L) = P, (:simplify R) = Q) => (:topoly (P - Q))
(L difference R, (:simplify L) = P, (:simplify R) = Q, (:topoly (P - Q)) = D) => ((L difference R) = D)

# compare -- the verdict when the difference is constant
(L compare R) => (L difference R)
(L compare R, (L difference R) = (pos zint N), N != &0) => ((L compare R) = gt)
(L compare R, (L difference R) = (pos zint &0)) => ((L compare R) = eq)
(L compare R, (L difference R) = (neg zint N)) => ((L compare R) = lt)

%(zelph/no-selffact-sugar "exquo" "difference" "compare")
```

An earlier version of 35 rules computed values with a request of its own (`acevalue`) and used
placeholder names. The text above replaces it: the names are the final ones; values come from the
simplifier, which three rules teach `exquo`; the natural case of `exquo` matches numerals by pattern,
so integer and symbolic operands start no `mod` or `/` work, and each sign case is written out with an
explicit zero case; `difference` is a request of its own, and `compare` requests it. Three behaviours
differ from the earlier version, all intended: `X^(1+1)` now has a difference (the simplifier folds the
exponent first); `7 / 2 * 0` has the value 0 (a formal identity, true over ℚ) where the earlier
version was silent; and a quotient that does not reduce answers `:simplify` with itself, which `ace`
reads as "no value". A zero divisor stays without value everywhere.

The run types these 23 rules into a session on today's engine (they are not in the repository yet) and
then asks the requests `ace` makes: an exact quotient and the silence of an inexact one (`7 exquo 2`), a
value through the simplifier, the unreduced answer of an inexact quotient inside `:simplify`, a negative
dividend, silence for a zero divisor, verdicts over ground terms, and a verdict
over a math variable whose difference is constant (`X + 1` against `X`), while `2*X` against `X`, whose
difference is not constant, stays silent.

```
zelph+-> ? &12 exquo &4
Answer: (&12 exquo &4) = &3
zelph+-> ? &7 exquo &2
zelph+-> ? :simplify ((&2 * &3) + (&12 exquo &4))
Answer: (:simplify ((&2 * &3) + (&12 exquo &4))) = &9
zelph+-> ? :simplify (&7 exquo &2)
Answer: (:simplify (&7 exquo &2)) = (&7 exquo &2)
zelph+-> ? :simplify ((neg zint &12) exquo &4)
Answer: (:simplify ((neg zint &12) exquo &4)) = (neg zint &3)
zelph+-> ? :simplify (&7 exquo &0)
zelph+-> ? (&12 exquo &4) compare &3
Answer: ((&12 exquo &4) compare &3) = eq
zelph+-> ? (&2 - &5) compare &0
Answer: ((&2 - &5) compare &0) = lt
zelph+-> ? &7 compare (&12 exquo &4)
Answer: (&7 compare (&12 exquo &4)) = gt
zelph+-> "X" ~ symvar
zelph+-> ? ("X" + &1) compare "X"
Answer: (("X" + &1) compare "X") = gt
zelph+-> ? ("X" * &2) compare "X"
```

## Deciding a Formula

### The procedure and the verdict table

Every arithmetic yes/no answer and check on this page comes out of this procedure (identities are
decided by node identity instead). For `L op R`, `ace`
requests `(L' compare R')`, which requests `(L' difference R')`; runs; and reads the verdict and D, the
normal form of L − R. The math variables are of three kinds: U universal, E
existential, C constants. vars(D) come from the normal form decomposed with `zelph/parse` (host item
P-parts); the fallback vars(L) ∪ vars(R) is sound, only less complete.

| D | `=` | `\=` | `<` | `>` | `=<` | `>=` |
|---|---|---|---|---|---|---|
| none (silent) | ? | ? | ? | ? | ? | ? |
| `(pos zint &0)` (verdict `eq`) | T | F | F | F | T | T |
| `(pos zint N)`, N ≠ 0 (`gt`) | F | T | F | T | F | T |
| `(neg zint N)` (`lt`) | F | T | T | F | T | F |
| not constant, vars(D) ⊆ U | **F** | ? | ? | ? | ? | ? |
| not constant, vars(D) ⊆ E | ? | **T** | ? | ? | ? | ? |
| not constant, otherwise (any C, or U and E mixed) | ? | ? | ? | ? | ? | ? |

T is true, F false, ? unknown. The polynomial layer answers in the coefficient form `(pos zint N)`, so
the constant rows hold as written.

- A universal `=` with a non-constant D is false by the identity theorem: a non-zero polynomial does not
  vanish on all of an infinite integral domain. An existential `\=` with a non-constant D is true for
  the same reason.
- A constant C is a fixed unknown value, not one that can be chosen: `For every integer X, Pi * X = X`
  holds iff Pi = 1, so it is unknown (E14: the difference is `(Pi - 1)*X`).
- **A `/` in the formula.** A ground side is compared by its value through the second route of
  `difference` (`((&12 exquo &4) compare &3) = eq`); a zero divisor anywhere leaves the formula
  undecided; an inexact quotient that a formal identity removes is decided over ℚ (`7 / 2 * 0 = 0` is
  true); one that stays leaves it undecided (`7 / 2 * 2 = 7`, `3*(2-4/13) >= 1/3`). The route through
  `:simplify` also folds a ground exponent before the polynomial layer needs a numeral: `X^(1+1)` has a
  normal form, where `topoly` alone is silent.
- **An earlier verdict table was unsound in one cell, and must not be reused.** It marked a universal
  `\=` with non-constant D as true; `For every integer X, X \= 0` is false at X = 0. It also marked the
  existential case as unknown, which is sound but incomplete. The table above replaces it.

### Several formulas

Several formulas of one statement or question box combine three-valued: "and" is the minimum, "or" the
maximum (F < ? < T), and a negated formula swaps T and F. A top-level "or" stays refused
(`:or/in-head-or-statement`); in a question it is `:or/in-question` (M4).

### Math-universal implications

An implication `(:imp A C)` — asserted, negated at top level (`It is false that for every integer X
…`), or inside a question — is **math-universal** when A holds only type conditions (`object` `integer`
or `number`, optionally the property `natural`, as in 2396) on referents that occur elsewhere only in
formulas, and C holds only comparisons and negated formulas. It is decided by the table above with
those referents as U, and **never compiled into a rule over `integer` nodes**: the quantifier is the
ring declaration. `ace/math-plan` recognises it and returns a `:check` (statement) or an `:ask-value`
payload in `:decide` mode (question) in place of the rule that `ace/plan` would build (`ace/plan`
offers every implication that holds a formula to the hook first).

- Equality verdicts hold for every infinite domain (integer, number, natural number). Inequalities are
  decided only through a constant D: `For every natural number X, X >= 0` stays unknown.
- A negated math-universal takes the negated verdict; unknown stays unknown. This is what makes the
  universal "No" of [questions](#questions-and-answers) re-enter as a true statement.
- Beyond reach, and classified by their own ids anyway: existentials or cardinalities in C (2369, 2393:
  `:object/at-least`), formulas in A (`If X > 2 then X > 1.`: `:rule/unsafe`).

## The Scratch Protocol

Every evaluation runs inside a scratch cluster that is dropped afterwards: a cluster records the nodes
created while it is active, and dropping it removes exactly those ([Scoped work:
clusters](../janet.md#scoped-work-clusters)). So deciding `1 + 2 = 3` leaves the graph as it was.

`(ace/math-decide! mf kinds resolve)` returns `{:verdict V :d NF :why W}`, with V one of `:true`,
`:false`, `:unknown`, NF the normal form or nil, and W nil or `:conflict`; `(ace/math-value! mts resolve)`
returns a tuple with one decimal text or nil per term of `mts` (all ground terms of a block in one
call). Both run:

1. `(def prev (zelph/cluster))`, `(zelph/cluster "ace:math")`.
2. Build the terms (`ace/math-term!`), then `ace/math-order!` (decisions only).
3. Assert the requests, names in `"zelph"`: `(L' compare R')` for a formula; `(:simplify T')` for a
   ground value; `(T' difference &0)` for the normal form of a term with math variables.
4. `(ace/infer!)`, that is `(zelph/run-delta :quiet)` ([Running quietly](../janet.md#running-quietly)),
   which prints nothing and returns `{:contradictions C :naf-final N}`: the contradictions the run met
   and, whenever negation records are kept, the conclusions under "not provably" it saw defeated. The
   derivation echo of M9 never shows a scratch run.
5. Read the answers of each request as data, by a query `req = V` inside `ace:math`: a verdict
   `(zelph/name v "zelph")`, one of `gt`, `eq`, `lt`; a value `(string v)`, which is a value only in the
   two shapes `&<digits>` (giving `"162"`) and `neg zint &<digits>` (giving `"-30"`) — anything else,
   such as an unreduced `&7 exquo &2` or `&3 ^ (neg zint &1)`, is no value (E15); a normal form
   decomposed with `zelph/parse` into `NF = (:z Sign Digits)` or `(:poly Name (NF …))` (coefficients least
   significant first), with the math-variable and constant nodes mapped back to their ACE names
   through the table built in step 2.
6. `(zelph/cluster prev)`, `(zelph/cluster-drop "ace:math")`; return the data.

`(ace/math-numeral node)` reads a canonical number node from the graph in the same way as step 5's
value, without a run and without a write.

The run performs these steps from Janet on today's engine for the value of `2 - 5 + 1`: the quiet run
returns no contradiction, the value arrives as the text `neg zint &2` (that is −2), and `.stat` shows
the same number of nodes before and after; the drop returns how many nodes it removed.

```
zelph+> .auto-run
Auto-run is now disabled.
zelph+-> .stat
Network Statistics:
------------------------
Nodes: 5661
RAM Usage: 0.0 GiB
Name-of-Node Entries by language:
  zelph: 1498
Node-of-Name Entries by language:
  zelph: 124
Languages: 1
Rules: 418
------------------------
zelph+-> %(def prev (zelph/cluster))
zelph+-> %(zelph/cluster "ace:math")
"ace:math"
zelph+-> %(def t (zelph/fact (zelph/fact (zelph/int "2") "-" (zelph/int "5")) "+" (zelph/int "1")))
<zelph/node $( &2 - &5 + &1 )>
zelph+-> %(def req (zelph/fact t "simplify" t))
<zelph/node :simplify $( &2 - &5 + &1 )>
zelph+-> %(def found (zelph/run-delta :quiet))
{:contradictions () :naf-final ()}
zelph-> %(def values (map |(string (get $ 'V)) (zelph/query (zelph/fact req "=" 'V))))
@["neg zint &2"]
zelph-> %(zelph/cluster prev)
zelph-> %(zelph/cluster-drop "ace:math")
116
zelph-> %(zelph/out (string/format "values: %j  contradictions: %d" values (length (found :contradictions))))
values: @["neg zint &2"]  contradictions: 0
zelph-> .stat
Network Statistics:
------------------------
Nodes: 5661
RAM Usage: 0.0 GiB
Name-of-Node Entries by language:
  zelph: 1498
Node-of-Name Entries by language:
  zelph: 124
Languages: 1
Rules: 418
------------------------
```

### Trust

A result counts only if step 4 returned no contradiction and no defeat, and every request read has at
most one answer (invariant 16). Otherwise it is `:unknown` with `:why :conflict` (no value), and the
block gets `[ace/w-not-checkable]`, saying that the graph's own arithmetic disagrees with itself. The
contradictions and defeats of a scratch run are not reported as `[ace/contradiction-derived]` or
`[ace/w-naf-final]`: what the run derived goes with `ace:math`, and the next inference, a full pass
after the drop, meets again whatever of it concerns the user's graph.

The math stack reports contradictions of its own in every mode: rule SU (a second normal form), rule DU
(a second raw derivative) and a cycle in the variable order. `ace`'s requests trigger none of them;
they fire when the user's graph holds a declared equation that disagrees with the arithmetic, or
opposite variable orders. Then a request can answer twice: after a native `(&1 + &2) = &4`,
`(&1 + &2) compare &3` answers both `gt` and `eq`, and rule SU records the two normal forms (E18). The
record is made inside `ace:math` and goes with it, and nothing of it is printed in zelph syntax
([DEC-QUIET](decisions.md#dec-quiet)). The [first run on this page](#invariants) shows such a conflict
natively.

One evaluation of `12 / 4` creates and drops a few hundred nodes, and `.stat` is unchanged afterwards
(E7, E15). The counts move with every change of the standard library, so tests assert "unchanged",
never a count.

### The drop, and what it relies on

Three host items, all built, make the drop correct and cheap:

- **P-drop-delta** ([DEC-DROP-DELTA](decisions.md#dec-drop-delta) (a)). A run inside `ace:math`
  derives facts there, and the drop removes them. Without the fix, the next `run-delta` assumed a fixpoint it no longer had and missed
  consequences of facts that had been pending before (E5). Now, dropping a cluster in which a run
  derived facts invalidates the record of the incremental run, and the next `ace/infer!` runs a full
  pass: correct, at the cost of one full pass (an estimate of about 12 ms with math and primes loaded,
  from an old measurement) after every block that ran an `ace:math` cluster.
- **P-scratch-drop.** A cluster drop used to disarm the engine's fact-path stores for the rest of the
  session, so the first `ace:math` drop would have put every `ace-math` session into the slow state the
  Jacobian performance guard was written for. As built, the drop keeps them armed (test 21 below; the
  engine pins it with "performance guard: a dropped cluster leaves the math stack's stores armed").
- **P-drop-names.** Removing a node no longer scans the whole name map: a drop costs a constant per
  language and node, never a scan of a large graph's names.

### Where it runs

- **The check step** (the math checks of the resolution step, before the dry run; [Step 3: Resolution
  and Checks](block.md#step-3-resolution-and-checks)): each `:check` in its own `ace:math` cluster,
  each ground formula condition of a rule the same way, and the ground terms of atoms together in one
  cluster, by one `ace/math-value!` call. A check reads no fact the block writes (names in formulas are
  symbolic constants), so deciding it before the writes loses nothing under
  [DEC-QTIME](decisions.md#dec-qtime) (a); a box that mixes atoms and formulas is a question, never a
  check.
- **The question step** (after the real run and the block's one inference, DEC-QTIME (a); [Questions
  in a Block](block.md#questions-in-a-block)): each math question in its own `ace:math` cluster, which
  is that question's scratch cluster in place of `ace:q`; its atom query and its requests share the
  one run.

## Statements

The policy for statements is [The ace Block](block.md#what-a-block-does)'s: an assertion the graph
already contradicts rejects the block, and a false ground formula is that case. A `:check` is decided
in the check step, before the dry run, so a rejected block has written nothing.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace-math
    zelph+> ace 1 + 2 = 3. It is false that 2 = 5 - 4. A = 2*3^4. Moon is the Moon.

    zelph> ace John's age is 29 + 1. 3*(2-4/13) >= 1/3.

    Warning [ace/w-not-checkable]: sentence 2, line 1, column 34: zelph cannot decide whether 3 * (2 - 4 / 13) >= 1 / 3; nothing was stored for this sentence.
    zelph> ace 1 + 2 = 4.

    Error in line "ace 1 + 2 = 4.": Contradiction [ace/contradiction]: sentence 1, line 1, column 1: this cannot be true, because 1 + 2 - 4 = -1.
      <>1 + 2 = 4.
    ```

The first block stores nothing; the second stores `John age &30`. `4 / 13` has no whole-number value,
so the comparison stays undecided, although it holds over the rationals: `ace` says what it cannot
decide instead of guessing. The warning points at the formula's operator, the token of the formula
condition; a contradiction points at the start of its sentence.

| Text | Plan | Effect | Output |
|---|---|---|---|
| true ground formula `1 + 2 = 3.` (2385) | `:check` | nothing stored | nothing |
| false ground formula `1 + 2 = 4.`, `-0 = -1.` (2852) | `:check` | block rejected in the check step, graph unchanged | `[ace/contradiction]`, with the difference as evidence (`1 + 2 - 4 = -1`) |
| undecided `3*(2-4/13) >= 1/3.` (the shape of 2370); `There is a natural number N. -5 < N.` (the shape of 2396) | `:check` | nothing stored | `[ace/w-not-checkable]` |
| a formula whose run met a contradiction of the math stack (E18) | `:check` | nothing stored | `[ace/w-not-checkable]`, naming the conflict in the graph's arithmetic |
| definition `A = 2*3^4.` (3771); without a value 3773 `2*3^(-1)`, 3774 `2*3^(2+B)` | none (a definition) | nothing stored | nothing |
| math-universal: true, false, undecided | `:check` (U) | as the three rows above | as above |
| identity `John = Mary.`, `John is Mary.` | `:check` of kind `:identity`, decided in the resolution step | block rejected | `[ace/contradiction]` naming what each side resolved to |
| `John \= Mary.`, `Berlin is not Paris.` (the shape of 2810), `Moon is the Moon.` (the shape of 3751) | same | nothing stored | nothing |
| `Man-<16 hex> is John.` | same | block refused, graph unchanged | `[ace/unsupported]`, construct `copula/identity` |
| a ground term in an atom: `John's age is 29 + 1.` | `:fact`, value computed in the check step | `John age &30` | nothing |
| a term without a value: `John likes Sue + 2.` (the shape of 3527), `John likes 7 / 2.` | none | block refused, graph unchanged | `[ace/unsupported]`, construct `expr/as-data` |
| `It is false that 2 = 5 - 4.` (the shape of 2386) | `:check`, polarity flipped | nothing stored | nothing |

**The `:check` payload** (owned here): `{:kind K :f MF :kinds {Name Kind} :polarity P :verdict V :text
T}`, with K `:compare` or `:identity`, Kind one of `:E`, `:U`, `:C`, P `:pos` or `:neg`, V set by
`ace/plan` when it needs no graph and by the resolution step for an identity, and T the ACE text of the
formula; or `{:kind :all :parts (Check …) :op Op}` with Op `:and` or `:or` for several formulas.
`ace/zelph-of` renders a check as the comment line `# check: <text>`.

## Questions and Answers

All math questions — values, yes/no over formulas, and boxes that mix atoms with formulas — are one
plan action, `:ask-value`, with the payload `{:mode M :kind K :term MT :f MF :parts … :kinds {…}
:pattern (Pat …) :vars (Var …) :answer {…} :drs DRS :verdict V}`, M one of `:value`, `:decide`, `:vocab`
and K `:compare` or `:identity`. Questions are answered in the question step, after the block's writes
and its one inference (DEC-QTIME (a)); one that already carries `:verdict` is answered from it without
the hook, so identity questions need no `ace-math`.

- **A box that mixes atoms and formulas** (`Is John's age greater than 20?`) queries its atoms in its
  `ace:math` cluster, then decides the formulas once per result row with that row's values, all
  requests in one run: **Yes** if some row makes them all true; **No** if the atoms have no variable,
  hold, and a formula is false; otherwise not provable. A wh-question answers every row that makes
  them all true. A ground atom is checked as every yes/no question checks it: with
  `zelph/exists … :exact` first and, on a miss while the graph may hold facts with several objects,
  with the plain `zelph/exists` ([Yes/No Questions](answers.md#yesno-questions),
  [DEC-YESNO-WIDER](decisions.md#dec-yesno-wider)). The hook calls the function of `ace/ask` that
  makes this check, so the plain probe stays the one call of its kind in the front end's sources.
- **Value mode.** A ground term is requested as `(:simplify T')` and answered by its numeral; a term
  with math variables as `(T' difference &0)`, its normal form decomposed (step 5 of the scratch
  protocol) and printed by `ace/math-nf-text`; a term with a constant C prints its normal form the
  same way (`Pi` is a symbol in it). No value: `No answer.`

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace-math
    zelph+> ace What is 2 - 5 + 1? What is -2^2? A = 2*3^4. What is A?
    What is 12 / 4? What is 7 / 0? What is (X + 1)^2?

    Answer: 2 - 5 + 1 = -2.
    Answer: -2^2 = 4.
    Answer: 2 * 3^4 = 162.
    Answer: 12 / 4 = 3.
    No answer.
    Answer: (X + 1)^2 = X^2 + 2*X + 1.
    zelph> ace Is it true that 1 + 2 = 3? Is it false that 1 + 2 = 4? Is it true that 1 + 2 \= 3?

    Answer: Yes. 1 + 2 = 3.
    Answer: Yes. It is false that 1 + 2 = 4.
    Answer: No. It is false that 1 + 2 \= 3.
    zelph> ace Is it true that X \= 0?

    Answer: Yes. X \= 0.
    zelph> ace Is it true that (X + 1)^2 = X^2 + 1?

    Answer: It is not provable that (X + 1)^2 = X^2 + 1.
    zelph> ace For every integer X is it true that (1 + X) * (1 - X) = 1 - X^2?

    Answer: Yes. For every integer X (1 + X) * (1 - X) = 1 - X^2.
    ```

A variable is introduced once per paragraph, and one block is one paragraph, so questions over the
same variable name stand in blocks of their own.

| Question | Mode | Answer |
|---|---|---|
| `What is 2 + 3?` | value | `Answer: 2 + 3 = 5.` |
| `What is 2 - 5 + 1?` | value | `Answer: 2 - 5 + 1 = -2.` |
| `What is 2^3^2?` / `What is -2^2?` | value | `Answer: 2^3^2 = 64.` / `Answer: -2^2 = 4.` |
| `A = 2*3^4. What is A?` | value (a definition) | `Answer: 2 * 3^4 = 162.` |
| `What is (X + 1)^2?` | value, normal form | `Answer: (X + 1)^2 = X^2 + 2*X + 1.` |
| `What is 12 / 4?` / `What is 7 / 2?`, `What is 7 / 0?` | value | `Answer: 12 / 4 = 3.` / `No answer.` |
| `Is it true that 1 + 2 = 3?` | decide | `Answer: Yes. 1 + 2 = 3.` |
| `Is it true that 1 + 2 \= 3?` | decide | `Answer: No. It is false that 1 + 2 \= 3.` |
| `Is it true that X \= 0?` | decide, E | `Answer: Yes. X \= 0.` |
| `Is it true that X = X + 1?` | decide, E, D constant | `Answer: No. It is false that X = X + 1.` |
| `Is it true that (X + 1)^2 = X^2 + 1?` | decide, E | `Answer: It is not provable that (X + 1)^2 = X^2 + 1.` |
| `For every integer X is it true that (X + 1)^2 = X^2 + 1?` | decide, U | `Answer: No. It is false that for every integer X (X + 1)^2 = X^2 + 1. (X + 1)^2 - (X^2 + 1) = 2*X.` |
| `For every integer X is it true that (1 + X) * (1 - X) = 1 - X^2?` | decide, U | `Answer: Yes. For every integer X (1 + X) * (1 - X) = 1 - X^2.` |
| `Is it false that 1 + 2 = 4?` / `Is it false that 1 + 2 = 3?` | decide | `Answer: Yes. It is false that 1 + 2 = 4.` / `Answer: No. 1 + 2 = 3.` |
| `Is 5 greater than 3?` | decide ([vocabulary](#vocabulary)) | `Answer: Yes. 5 is greater than 3.` |
| `Is it true that John = Mary?` / `Is John Mary?` | decided in the resolution step | `Answer: No. It is false that John = Mary.` / `Answer: No. It is false that John is Mary.` |
| `Is Man-<16 hex> John?` | decided in the resolution step | `Answer: It is not provable that Man-<16 hex> is John.` |
| `Is 13 prime?` / `Is 91 prime?` / `Is 1 prime?` | vocabulary | `Answer: Yes. 13 is prime.` / `Answer: No. It is false that 91 is prime. 7 is a divisor of 91.` / `Answer: It is not provable that 1 is prime.` |
| `What is the derivative of X^3 - 2*X + 7?` | vocabulary | `Answer: The derivative of X^3 - 2*X + 7 is 3*X^2 - 2.` |

The arithmetic rows were produced by a research prototype and are identical on the engine before the
host work. The general rules of answer lines are [Questions, Answers and Explanations](answers.md)'s:
the prefix `Answer: `, three-valued yes/no, sorted lines, `No answer.` for a value question without a
value. A line may carry a second ACE sentence as evidence, and the No form of `Is it false that S?`
drops the double negation. Under `(ace/option :justify true)` (M9, [Why: Justified
Answers](answers.md#why-justified-answers)), math answers carry no support lines in v1: what decides
them is the scratch run, which is dropped with `ace:math`.

Rules for the answer lines:

1. **Round trip.** Every line, stripped of `Answer: ` and of `Yes. ` or `No. `, entered as an `ace`
   block in a fresh cluster of the same session, is accepted and creates no node — including the
   universal "No" line (a negated math-universal decided true, plus an existential formula whose
   difference is 0) and `X \= 0.` (existential, not constant, true). A line `It is not provable that …`
   is re-entered as the question it answers and gives the same line. The oracle RT-1 ([Answers
   Re-Enter](tests.md#rt-1-answers-re-enter)) exempts the answers to a universal question from the
   node count in general, because such a line re-enters as a rule; a math-universal line re-enters as
   a check, which stores nothing, so these lines meet the full oracle.
2. **The universal is echoed.** Without "for every integer X" the line would claim that no X exists,
   which is false for `(X + 1)^2 = X^2 + 1`.
3. **Evidence on the same line.** A false universal `=` appends the difference normal form; a composite
   number appends its smallest divisor. One line per answer keeps the sort order meaningful.
4. **Free variables stay existential.** `Is it true that (X + 1)^2 = X^2 + 1?` is not provable, since
   X = 0 satisfies it; the user page `ace.md` explains how to ask the universal question.

## Printing

- **`ace/math-text`** (pure, in `ace-verbalise.zph`) echoes the user's term from the DRS with ACE
  precedence and **left** associativity: it parenthesises a right operand of the same level, never a
  left one; `^` is written without spaces, `* / + -` with spaces (`(X + 1)^2`, `2^3^2`, `2 * 3^4`).
  Referents print as their ACE variable names. It needs no arithmetic, so identity answers and the
  verbaliser round trip over corpus DRSs run without `ace-math`.
- **`ace/math-nf-text`** (pure, in `ace-math.zph`) flattens a normal form `(:poly V (c0 c1 …))` into
  monomials, sorts them descending by exponent vector in ring order, prints `c*X^e*Y^f` with `1*` and
  `^1` omitted and ` + ` or ` - ` between monomials, a negative leading coefficient as a negative
  literal (`-3*X^2`, `-1*X^2`: ACE has no unary minus on a variable), and zero as `0`. Lines the
  prototype printed: `(X + Y + Z)^4 = X^4 + 4*X^3*Y + … + Z^4.` (15 monomials),
  `(X + Y)^2 - (X - Y)^2 = 4*X*Y.`, `2 - 5 + 1 = -2.`
- **The `:native` hook** renders an `MT` as zelph text for `ace/zelph-of`, without the graph: fully
  parenthesised infix (`(&2 * (&3 ^ &4))`), `/` as `exquo`, numbers as `&n` and `(neg zint &n)`, math
  variables quoted (`"X"`), requests in their self-fact or infix form (`(:simplify T)`,
  `(L compare R)`). Such text re-enters: quoted names read back, and left-nested powers print with
  parentheses. Like every line that names the math stack, it is exact only when typed under
  `.lang zelph`.
- ACE text never comes from `(string node)` of a term, which is zelph's display. Numerals in ACE answers
  come from `ace/math-numeral`, which the verbaliser reaches through the hook.

## Formulas in Rules

A comparison in a condition cannot simply be a condition of the rule, because zelph's mathematics
computes only what is requested (invariant 6). So `ace` writes two rules for one: a **seed** that
requests the verdict for every binding of the rule's other conditions, and the main rule, which tests
the verdict. A comparison in a consequence becomes a denial.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace-math
    zelph+> ace John is a person. John's age is 30. Mary is a person. Mary's age is 15.
    If a person's age is A and A > 17 then the person is an adult.
    If a person's age is A and it is not provable that A > 18 then the person is a minor.
    Who is an adult? Who is a minor?

    Answer: John is an adult.
    Answer: Mary is a minor.
    ```

The rules this block writes, as `ace/zelph-of` prints them (variables named in the order of
[Variables](api.md#variables); the comments are added here):

```
((X ~ person), (X age Y)) => (Y compare &17)                          # seed of "A > 17"
((X ~ person), (X age Y), ((Y compare &17) = gt)) => (X ~ adult)
((X ~ person), (X age Y)) => (Y compare &18)                          # seed of "A > 18"
((X ~ person), (X age Y), ¬((Y compare &18) = gt)) => (X ~ minor)
```

The user's `A` is not a zelph name: it is the age referent, whose variable is `Y`, and as a number it
gets no class pattern ([typing](#typing)). "Not provably" over a plain variable side needs no value
binding ([values before negation](#values-before-negation)).

### Conditions

Safety: every variable of a comparison is bound by a positive condition that is not a formula, or by a
value binding before it; otherwise `:rule/unsafe`. Below, A stands for the rule's conditions that are
not formulas, L', R', T' for the built terms, and V, W for fresh rule variables.

| ACE condition | Seed rules | Condition in the main rule |
|---|---|---|
| a term T with variables inside an atom | `(A) => (:simplify T')`; `(A, ((:simplify T') = V)) => (V compare &0)` | `((:simplify T') = V), ((V compare &0) = W)`; V replaces T in the atom |
| `L = R` / `L < R` / `L > R` | `(A) => (L' compare R')` | `((L' compare R') = eq)` / `= lt` / `= gt` |
| `L \= R` | the same | `((L' compare R') = W), (W != eq)` |
| `L =< R` / `L >= R` | the same | `((L' compare R') = W), (W != gt)` / `(W != lt)` |
| `it is false that L op R` | the same | the complementary operator, positive (the verdict is total once computed) |
| `it is not provable that L op R` | [values before negation](#values-before-negation) | `¬((L'' compare R'') = v)` for each verdict v that makes op true, over bound values |
| a comparative with an arithmetic reading and a numeric side (`X is greater than 20`) | as the formula | as the formula |
| a ground formula condition (`If 1 + 1 = 2 then …`) | none | decided in the check step: true, the condition is dropped; false, the rule is not stored, `[ace/w-vacuous]`; undecided, not stored, `[ace/w-not-checkable]` |

The seeds of later conditions include the value bindings before them.

### Consequences

| ACE consequence | Compiled to |
|---|---|
| `V = T`, V a referent of the then-box (the shape of 3593: a then-box such as "… then the field has an area S and S = W * L.") | the definition substitutes; the term in the head atom becomes a guarded value binding in the body, with its two seeds `(C) => (:simplify T')` and `(C, ((:simplify T') = V)) => (V compare &0)` |
| any other `L op R` (2367, 2360, and the shape of 3600: "If a pupil's age is X then X > 5 and X < 19.") | the seed `(C) => (L' compare R')` and one denial: `(C, ((L' compare R') = W), (W != v)) => !` when one verdict v makes op true (`=` eq, `<` lt, `>` gt); `(C, ((L' compare R') = v)) => !` when one verdict v makes it false (`\=` eq, `=<` gt, `>=` lt) |
| `it is false that L op R` | the denial of the complementary operator |

Here C stands for the rule's conditions. Construction follows [the rule idiom](api.md#the-rule-idiom)
of Modules, API and Data Shapes: every zelph rule in its own `zelph/dedup-rule` thunk, all heads of one
main rule in one `=>` fact, `zelph/var` inside the thunk; seeds and denials are separate rules, and a
rule with one condition is built as a one-element condition set
([DEC-RECORD-SINGLE](decisions.md#dec-record-single)), which `ace/zelph-of` prints as
`(*{(X ~ person)} ~ conjunction) => …`
([DEC-ONE-CONDITION-PRINT](decisions.md#dec-one-condition-print)). Identity formulas in rules follow [identity](#identity).

Verified with the module's starting text (E16; the variables renamed to this page's letters, the seeds
of the grownup, halfage, teenager and minor rules left out):

```
((X ~ person), (X age Y)) => (Y compare &17)                                        # seed
((X ~ person), (X age Y), ((Y compare &17) = gt)) => (X ~ adult)
        → ann (18), john (30)
((X ~ person), (X age Y), ((Y compare &18) = Z), (Z != lt)) => (X ~ grownup)
        → ann, john; carl (17) not
((X ~ person), (X age Y)) => (:simplify (Y + &1))
((X ~ person), (X age Y), ((:simplify (Y + &1)) = Z)) => (Z compare &0)
((X ~ person), (X age Y), ((:simplify (Y + &1)) = Z), ((Z compare &0) = A)) => (X nextage Z)
        → kid nextage &10, eve nextage (neg zint &2), john nextage &31; bob (string age) none
((X ~ person), (X age Y), ((:simplify (Y exquo &2)) = Z), ((Z compare &0) = A)) => (X halfage Z)
        → john halfage &15, ann halfage &9; kid (9), carl (17), eve (−3) none
((X ~ teenager), (X age Y), ((Y compare &20) = Z), (Z != lt)) => !
        → tim age &20 contradicts
((X ~ person), (X age Y), ¬((Y compare &18) = gt)) => (X ~ minor)
        → ann (18) minor, john (30) not
```

The first run below types the module text (its lines are not repeated in the recording) and the facts
and four rules of the session, exactly as listed above, into today's engine, with auto-run off, and runs
one quiet inference before asking, as an `ace` block does: John is an adult, Mary a minor. The minor
rule stands on a negation level of its own above the simplifier, in a stratifiable component, so it
passes the check of [DEC-STRATA](decisions.md#dec-strata) that a block's dry run makes.

```
zelph+-> John ~ person
zelph+-> John age &30
zelph+-> Mary ~ person
zelph+-> Mary age &15
zelph+-> ((X ~ person), (X age Y)) => (Y compare &17)
zelph+-> ((X ~ person), (X age Y), ((Y compare &17) = gt)) => (X ~ adult)
zelph+-> ((X ~ person), (X age Y)) => (Y compare &18)
zelph+-> ((X ~ person), (X age Y), ¬((Y compare &18) = gt)) => (X ~ minor)
zelph+-> .run quiet
zelph-> X ~ adult
Answer: John ~ adult
zelph-> X ~ minor
Answer: Mary ~ minor
```

The second run shows the two consequence shapes. "Half the age" is bound through `:simplify` and the
guard: John's half age is 15, while the kid, aged 9, gets none, because `9 / 2` does not reduce and its
unreduced term fails the guard. The two rules for "every pupil's age is less than 19" (a seed and a
denial) meet Tim, aged 19: the quiet run returns one contradiction, whose conditions are the verdict
`eq`, Tim's age and his class.

```
zelph+-> John ~ person
zelph+-> John age &30
zelph+-> Kid ~ person
zelph+-> Kid age &9
zelph+-> ((X ~ person), (X age Y)) => (:simplify (Y exquo &2))
zelph+-> ((X ~ person), (X age Y), ((:simplify (Y exquo &2)) = Z)) => (Z compare &0)
zelph+-> ((X ~ person), (X age Y), ((:simplify (Y exquo &2)) = Z), ((Z compare &0) = A)) => (X halfage Z)
zelph+-> Tim ~ pupil
zelph+-> Tim age &19
zelph+-> ((X ~ pupil), (X age Y)) => (Y compare &19)
zelph+-> ((X ~ pupil), (X age Y), ((Y compare &19) = Z), (Z != lt)) => !
zelph+-> %(each e ((zelph/run-delta :quiet) :contradictions) (zelph/out (string/format "contradiction: %q" (e :conditions))))
contradiction: (<zelph/node (&19 compare &19) = eq> <zelph/node Tim age &19> <zelph/node Tim ~ pupil>)
zelph-> X halfage Y
Answer: John halfage &15
```

In an `ace` block, a denial like this that meets its own block's facts ("If a pupil's age is X then
X < 19. Tim is a pupil. Tim's age is 19.") is reported as `[ace/contradiction-derived]` after the
block's inference, and the block stays ([The Report of a Derived
Contradiction](block.md#the-report-of-a-derived-contradiction)).

**Open for M5:** how that report names the verdict fact among the contradiction's conditions,
`((&19 compare &19) = eq)` in the run above. The report lists every condition fact except guards and
helper atoms, each by its sentences or, without provenance, by the ACE sentence the verbaliser makes of
it; a verdict fact has neither, so as written its line would read "a fact of the loaded graph that ace
cannot express". The specification leaves the choice between leaving it out like a guard (it is
vocabulary of the math stack) and rendering it as the formula it decides.

### Values before negation

A conclusion under "not provably" is final ([DEC-STRATA](decisions.md#dec-strata)). A verdict over a
term whose value needs the simplifier's fallback — itself a rule with a negated condition,
`(T red C, ¬(T rw S)) => (T simp C)` — can arrive after the negation over it has been decided. With
`((X age Y), ¬(((((Y exquo &2) * &0) + &20) compare &18) = gt)) => (X ~ minor)` and its seed,
`tim age &13` gave `tim ~ minor` on the engine before the host work, although
`((((&13 exquo &2) * &0) + &20) compare &18) = gt` is derived afterwards (E20). Since zelph keys its
negation levels by pattern, that rule stands on level 2 above the simplifier and the plain form is
right in every order; but as soon as any rule over predicates is in force (even a transitive meta-rule
about a predicate no arithmetic or ACE rule touches), every key is its predicate alone, and the plain
form gives `tim ~ minor` again (measured again on the current tree), while the bound shape below
stays right.

So in a negated formula condition every side that is not a variable or a number literal is first bound
to its value by the guarded value binding, and the negation tests the verdict over the bound values
(shown for `>` with a compound left side T and a literal right side R'; a compound right side is bound
the same way):

```
(A) => (:simplify T')
(A, ((:simplify T') = V)) => (V compare &0)
(A, ((:simplify T') = V), ((V compare &0) = W)) => (V compare R')
(A, ((:simplify T') = V), ((V compare &0) = W), ¬((V compare R') = gt)) => (head)
```

This shape is right in every rule and fact order tried (E20). Its cost is incompleteness, never a wrong
conclusion: a side without a value binds nothing, so "it is not provable that A / 2 > 18" does not fire
for an odd A, where the plain reading would. A plain variable side needs no binding: the plain shape
`¬((Y compare &18) = gt)` is right in every order.

**A rule over predicates and the dry run.** While a rule over predicates is in force, the engine puts
a `:naf/formula` rule into one component with the simplifier's fallback, and that component is not
stratifiable (measured with `(zelph/strata)` on the current tree, for the plain and the bound shape
alike; without such a rule both shapes stand alone on level 2). The negation-level check of a block's
dry run therefore refuses a block that writes such a rule while a rule over predicates is in force, as
`:naf/unstratified`, because the component also holds rules `ace` did not write ([Step 4: The Dry
Run](block.md#step-4-the-dry-run); the M4 test `ace constructs: "not provably" beside a rule over
predicates is refused` pins the refusal for a rule without arithmetic). The bound shape matters where
the rule over predicates arrives after the ACE rule, typed natively: the engine then evaluates the ACE
rule in that component, and the bound shape keeps it right; a later block that writes a rule meets
the same refusal, because the check also covers the rules of earlier blocks.

**A requirement on the Lloyd-Topor helpers** of [From Sentences to the
Graph](semantics.md#helper-rules-for-not-provably): a helper whose box compares a compound term gets
the same treatment. The main rule binds the values positively and passes them into the helper, so the
negation is never evaluated before the values exist. A side over a referent the box declares itself
cannot be bound outside it, and such a box is refused as `:naf/unstratified`.

## Vocabulary

A few English words have an arithmetic reading: "greater", "smaller", "prime", "divisor",
"derivative". The vocabulary is **shipped Janet data of `ace-math.zph`**, like `ace-core`'s: never
written to the graph, `ace-lex` facts included, so a `.load` that replaces the graph cannot take it
away, and it never appears in a query ([DEC-LEX-STORE](decisions.md#dec-lex-store) (d),
[DEC-OF](decisions.md#dec-of) (a)).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace-math
    zelph+> ace Is 5 greater than 3? Is 13 prime? Is 91 prime? Is 1 prime?
    What is the derivative of X^3 - 2*X + 7?

    Answer: Yes. 5 is greater than 3.
    Answer: Yes. 13 is prime.
    Answer: No. It is false that 91 is prime. 7 is a divisor of 91.
    Answer: It is not provable that 1 is prime.
    Answer: The derivative of X^3 - 2*X + 7 is 3*X^2 - 2.
    ```

At its import, `ace-math` registers the vocabulary with `(ace-lexicon/add-shipped "math" {:ulex
ace-math/ulex :math-words ace-math/math-words})`. `ace-math/ulex` holds the ordinary entries of the words
`ace-core` lacks (`prime`, `divisor`, `derivative`; source `"math"`); `ace-math/math-words` holds the
`math_word` entries `(:math_word Form Class {:kind K :node N})`, Class `:noun` or `:adjective`, K one of
`:comparative`, `:test`, `:of`, N a name in the math stack. Each entry adds the field
`:math {:kind K :node N}` to the shipped readings of Form in that class; `great`, `small` and `large` are
`ace-core`'s adjectives and only gain the field. A user entry of the same form and class replaces the
shipped reading and its field; an overlay hides both. `ace/plan` reads the field from the readings
(pure: the lexicon cache).

| Word | Class | `:math` | ACE use | Request |
|---|---|---|---|---|
| `great`, `large` | adjective, comparative | `{:kind :comparative :node "gt"}`, that is `>` | `5 is greater than 3.` (2297, 2360) | `compare` |
| `small` | adjective, comparative | `{:kind :comparative :node "lt"}`, that is `<` | `4 is smaller than 9.` (the shape of 2123) | `compare` |
| `prime` | adjective | `{:kind :test :node "testprime"}` | `Is 13 prime?`, `13 is prime.` | `(:testprime &13) = prime`, `(:testprime &91) = composite`; silent for 0 and 1 |
| `divisor` | relational noun (with "of") | `{:kind :of :node "hasdivisor"}` | `7 is a divisor of 91.`, `What is a divisor of 91?` | `&91 hasdivisor &7` |
| `derivative` | relational noun (with "of") | `{:kind :of :node "diffby"}` | `the derivative of T`, which maps to `diffby` iff T has exactly one math variable and is refused otherwise, with the list form as hint; the list form `the derivative of [T, X1, …]`, mapped to `diffalong`, arrives with `:data/list` at M7 | `diffby` |

- A word's arithmetic reading applies only with a numeric argument. A comparative maps to `compare`
  only with "than" and a numeric **data** side (an integer or an arithmetic term, after substitution);
  otherwise it is the general comparison predicate (`"is greater than"`), so `There is a number X and a
  number Y. X is greater than Y.` stays general. Likewise `prime`, `divisor` and `derivative` over a
  non-numeric argument are ordinary content words, resolved like any word (on a Wikidata graph the
  nouns may meet an item by label, which the `word-senses` note announces, because their shipped
  entries carry no id symbol; see [The `word-senses` Note](words.md#the-word-senses-note)).
- These are the "lexicon-marked function nouns" of DEC-OF (a). A statement using them is a `:check`
  (`7 is a divisor of 91.`, `13 is prime.`, the derivative line above), so answers round-trip. A definite
  `the derivative of …` denotes a value, so the resolution of definite noun phrases MUST NOT warn
  `[ace/w-definite-new]` for it.
- Only `great` and `small` (`greater`, `smaller`) are in APE's small lexicon. `equal-to`, `remainder`,
  `sum`, and function nouns such as `exponential` are not in v1; the next step of DEC-OF adds them as
  more `:math` readings, with no new mechanism.
- There is no warning for a stdlib name: `ace`'s content lives in language `ace` and the math stack's
  134 internal names (with primes and the evaluation module's starting text; 22 of them display names
  of variables) in `zelph`, so they never meet
  ([DEC-NAMES](decisions.md#dec-names)).

The run shows the requests behind these words on today's engine: the primality test of
[Primality](../math/primality.md#request-idiom) (`prime`, `composite`, silent for 1), the smallest
divisor of 91, and the derivative through `diffby` ([Differentiation](../math/symbolic.md#differentiation)).

```
zelph+> .import primes
zelph> ? :testprime &13
Answer: (:testprime &13) = prime
zelph+> ? :testprime &91
Answer: (:testprime &91) = composite
zelph+> &91 hasdivisor D
Answer: &91 hasdivisor &7
zelph> ? :testprime &1
zelph+> "X" ~ symvar
zelph> ? $( "X"^3 - 2*"X" + 7 ) diffby "X"
Answer: ($( "X" ^ &3 - &2 * "X" + &7 ) diffby "X") = $( &3 * "X" ^ &2 - &2 )
```

## Costs

| Measured | Value |
|---|---|
| `.import math` | 5661 nodes, 418 rules (the same on today's engine) |
| plus `.import primes` | 5854 nodes, 433 rules |
| plus the module's starting text, without primes | 5942 nodes, 441 rules |
| plus the module's starting text and primes, as `ace-math` imports them | 6135 nodes, 456 rules |
| the earlier 35-rule module with primes | 6328 nodes, 468 rules |
| a substrate alone | binary 1333 / 76, decimal 5657 / 76, integer 2986 / 195 (nodes / rules) |
| `.import math` again after a replacing load | 418 rules; the nodes of a first import plus those of the loaded graph that are new to it (5663 after loading a graph of 18 nodes) |
| wall time of `.import math` plus `primes` | 87-114 ms (an old measurement, not repeated) |
| inference after each later statement or block | about 0.05 ms without math; about 12 ms with math and primes (100 facts: 1284-1294 ms); about 15 ms with the 35-rule module (old measurements, not repeated) |
| `run-delta` per line with math loaded | about 2.1 ms (old; since 1.1.0 every seeded run with math loaded takes a classic pass of the simplifier's fallback over every reduced term, so the figure is a yardstick only) |
| corpus sweep of 3733 cases in clusters | 82 ms without math, about 93 s with math (old measurement) |

The counts of the first four rows and of the replacing load were measured again for this page on
today's engine; the other counts are from the research runs. The times are estimates from older
trees; the implementing session measures them again on an idle machine as budgets B8 (the import and
the inference per statement) and B9 (`ace/infer!` with math loaded) of the work plan ([The Budget
Table](plan.md#the-budget-table)).

## What This Builds On in zelph

- Numbers as digit lists and rule-based arithmetic: [Arithmetic Substrates](../math/arithmetic.md),
  [Semantic Math](../logic.md#semantic-math-computation-as-graph-rewriting),
  [Number Literals](../logic.md#number-literals).
- The integers and their canonical form: [Integers over ℤ](../math/integers.md).
- The simplifier that gives every ground term its value, and the rule that makes normal forms unique:
  [The Symbolic Layer](../math/symbolic.md#the-simplification-core).
- Polynomial normal forms, variable order and the identity check: [Polynomial Normal
  Forms](../math/polynomial.md), [Term-to-Polynomial Compiler](../math/topoly.md).
- The front end `math` and the island `$( … )` that ACE deliberately does not use:
  [The math Front End](../math/frontend.md).
- Primality and divisors: [Primality](../math/primality.md); derivatives:
  [Differentiation](../math/symbolic.md#differentiation).
- Every request idiom in one table: [Module and Predicate Index](../math/reference.md).
- `!=` guards for identity: [Inequality Constraints](../logic.md#inequality-constraints).
- Scratch clusters and quiet runs: [Scoped work: clusters](../janet.md#scoped-work-clusters),
  [Running quietly](../janet.md#running-quietly).
- Language-independent numbers and modules that keep their own `.lang`:
  [Multi-language Support](../modules.md#multi-language-support).
- The host items as a group, with their tests: [What zelph Already Provides](foundation.md#tests-that-pin-the-foundation).

## Prerequisites and Traps

**Host items the arithmetic of M5 relies on**, all built before any ACE-specific code
([DEC-WORKMODE](decisions.md#dec-workmode)) and part of zelph 1.1.0 ([What zelph Already
Provides](foundation.md#what-the-front-end-relies-on)):

- P-number-lang (DEC-MATH-LANG (a), extended to `zelph/list-chars`; [Numbers and characters independent
  of the language](foundation.md#numbers-and-characters-independent-of-the-language)): number literals
  and characters language-independent, the substrates resolving in `zelph`. It moved no counter of the Jacobian
  performance guard.
- P-drop-delta and P-scratch-drop, and P-drop-names ([the drop](#the-drop-and-what-it-relies-on);
  [Scratch clusters and the dry run](foundation.md#scratch-clusters-and-the-dry-run)).
- P-quiet-run (a): `:quiet` on `zelph/run-delta`, returning the contradictions met ([Quiet runs, the
  rules in force and negation levels](foundation.md#quiet-runs-the-rules-in-force-and-negation-levels)).
- P-parts (`zelph/parse`) for symbolic answers and vars(D). No interim decomposition by iteration is
  written (a deepening search took 166 ms for 15 monomials in an old measurement and would be thrown
  away); ground answers do not need it (`(string n)` suffices).
- P-lookup (`zelph/lookup`, the `zelph/lang` setter) for identity by node and the language handling;
  P-exact for the probes of the ring order ([Lookups that create
  nothing](foundation.md#lookups-that-create-nothing), which also covers P-parts).

Before the host work none of these bindings existed (`parse:false lookup:false lang:false`;
`run-delta :quiet` gave `arity mismatch, expected 0, got 1`). The defects found on the way:

| Defect | Status |
|---|---|
| `zelph/number`, `zelph/int` and `&` literals followed `.lang` (E3, E6) | fixed by P-number-lang |
| a cluster drop left the fixpoint record of `run-delta` stale (E5) | fixed by P-drop-delta |
| `(zelph/import …)` from a running fiber left it unrooted | fixed by the Janet update ([DEC-JANET](decisions.md#dec-janet)) |
| math-syntax could not parse back a quoted name shaped like a variable | gone |
| math-syntax displayed power towers it could not parse | gone: both towers re-enter |
| `zelph/int` of a non-negative number gave `pos zint` | gone |

**Traps**, each of which the design above avoids:

- After `.import math`, `of` is function application: `(age of john) is A` answers
  `$( age(john) ) is thirty`. [DEC-OF](decisions.md#dec-of) (a) never uses the node `of`.
- `exquo`, `difference` and `compare` need `zelph/no-selffact-sugar`.
- A `:simplify` answer is not always a value: where nothing reduces it is the term itself
  (`(&7 exquo &2)`, `(&3 ^ (neg zint &1))`). `ace` tests the shape and guards rule bindings.
- The simplifier's identities are formal: `X * 0 → 0` and `X - X → 0` also remove an inexact quotient
  (true over ℚ); the evaluation module keeps them away from a zero divisor.
- The math stack's own contradiction rules (SU, DU, the cycle in the variable order) fire on
  conflicting user arithmetic, also inside `ace:math`.
- A negation over a verdict whose value needs the simplifier's fallback can be decided too early, while
  a rule over predicates is in force ([values before negation](#values-before-negation)).
- Every stored arithmetic subterm is a request (E10): terms are stored by value only.
- `(zelph/list-chars "")` is nil (E9): the empty string is the internal node `ace:empty-string`.
- Inside a rule-building thunk, queries, cluster switches and imports are refused.
- Every cluster switch restores `prev`, never a literal nil.
- Never `.import math` in a shared fixture of non-math tests.

## Tests

The harness, the layers, the gates and the final names are [Tests and Quality Gates](tests.md)'s,
which counts the arithmetic tests and leaves their content to this page ([Arithmetic](tests.md#arithmetic));
this is what the arithmetic must pin. Each TEST_CASE carries a comment saying why it exists and which
corpus cases it mirrors (`// acetexts 2386`); a hand-written test uses an own sentence in the shape of
a corpus case, never the corpus text. The test binary
runs in check mode (`.semi-naive check`): a conclusion under "not provably" that a later fact defeats
raises "Negation check: …", so a test that needs such a negation across runs opts out with
`.semi-naive on` and says why.

**Layer A, the corpus.** The TEST_CASE `"ace math corpus: formula and term cases agree with APE"`:
`doctest::test_suite("slow")`, compiled only when the corpus directory is configured
(`ZELPH_APE_CORPUS_DIR`), `.import ace-math`, the corpus lexicon in strict mode, auto-run off, over the
math slice that the tests page defines ([Layer A: Text to DRS](tests.md#layer-a-text-to-drs): the 98
accepted cases holding a `formula/`, `expr/` or `:data/integer` id, 95 of them runnable):

- DRS equivalence and the expected status for every runnable case of the slice, among them the 45
  accepted formula cases (2358 2359 2360 2362 2367 2369 2370 2384 2385 2386 2387 2388 2393 2394 2396 2448
  2739 2839 2852 3254-3257 3259-3266 3319 3596 3598 3633 3656 3677 3712 3722 3771-3776) and the 14
  cases with a term and no formula (2363 2364 2406 2423 2624 2625 2873 2874 2875 3364 3525 3526 3527
  3669), less the two below; its own id lists for the rejections 2372 2373 2851 3592 3678 1720 1721, and
  2630 accepted.
- Two of these are no DRS-equivalence cases: they need APE's big lexicon (Clex), which is never shipped
  or fetched, and are expected to be rejected with `[ace/unknown-word]` in strict mode: 3364 (`makes`),
  and 3598, a three-paragraph text whose second paragraph needs `radius` and `area`, which is
  **rejected as a whole**, where APE returns the partial DRS of `Pi = 3.14.`. Their refusals in the
  [corpus table](#the-math-corpus-cases-at-m5) (`:expr/concat`, `:data/real`) are the guessing-mode
  expectation of the graph layer, not this TEST_CASE's.
- The cases 3258, 3593 and 3600, which APE itself gets wrong: asserted for robustness only, never
  through the list of known divergences.
- Classification: the ids of [the ids table](#the-ids) per case, `:neg/formula` included, matching the
  refused row of the corpus table.

**Layer A, our own sentences** (fast; `.import ace-drs` and a lexicon of their own declared with
`ace/declare!`, since APE's small lexicon needs the fetched tree): the grammar oracles of [surface
syntax](#surface-syntax) — left-associativity including `^`, precedence, the negative literal (atomic,
across a space), hyphenated words, exactly six comparison operators, typed chains and untyped
formulas, the question forms that follow from the grammar.

**Layer C** (`doctest::test_suite("slow")`, `.import ace-math`; `run_arithmetic_modules` where numbers
are computed, so that the case runs on each substrate, otherwise `run_both_modes`). Proposed names:

1. `ace math: numbers are canonical` — `-0` is `&0`, not `(pos zint &0)`; `-3` is `(neg zint &3)`; `007` is
   `&7`; ACE `2*3^4` is the node of `$( 2*3^4 )`.
2. `ace math: value questions` — `2 + 3 = 5.`, `2 - 5 + 1 = -2.`, `2^3^2 = 64.` beside the island's
   `$( 2^3^2 )` simplifying to 512, `-2^2 = 4.` beside the island's −4.
3. `ace math: division is exact or unknown, never truncated` — 12/4, 7/2, 7/0, −12/4, `7 / 0 * 0` without a
   value, beside the native `(&7 / &2) = &3`.
4. `ace math: free variables are existential, universal topics decide identities` — the question rows
   above; `2 ^ (2 ^ 2) = (2 ^ 2) ^ 2.` is true.
5. `ace math: \= is true for some X but undecided for every X` — its comment says that an earlier
   verdict table was unsound here.
6. `ace math: proper names are fixed constants` — `Pi * X = X` for every integer X is unknown.
7. `ace math: asserted formulas are checked, never stored as = facts` — after `1 + 2 = 4.` the block is
   rejected and `((&1 + &2) = &4)` does not exist (`zelph/exists`); `.stat` is unchanged after
   `1 + 2 = 3.`
8. `ace math: a definition substitutes within its block only` — `A = 2*3^4. What is A?`; the next block's
   `A` is new.
9. `ace math: ring order is extended, never contradicted` — a native order first; one normal form per
   term afterwards (node identity), no contradiction.
10. `ace math: formulas in rule conditions fire through seeds` — adult, the `>=` guard, `\=`, negated,
    "not provably"; the negation over a value that needs the simplifier's fallback gives no minor, in
    check mode, also when a native transitive meta-rule is typed after the block (the case in which the
    plain form is wrong; typed before the block, the meta-rule makes the dry run refuse the block, see
    [values before negation](#values-before-negation)).
11. `ace math: formula consequences bind values or constrain` — the next-age binding; no binding for a
    string age or an inexact quotient (the guard); the age denial's contradiction and its record.
12. `ace math: equations between names are decided by node identity` (fast, no math) — rules that
    always hold store nothing (3254-3257, 3264); contradictions reject (3262 3263 3265); vacuous rules
    store nothing, with `[ace/w-vacuous]` (3259-3261, 3266); the `\=` guard; an identity in a head as a
    denial; `Is John Mary?` gives `No.`; an individual and a name: "not provable" as a question; two
    spellings of one item on the Wikidata-style fixture (`src/test/data/wikidata/ace-fixture.json`) are
    identical. The copula statements (`John is Bill.`, 3751, an asserted identity between an
    individual and a name, a vacuous copula condition) are the `ace names: …` tests of the tests page.
13. `ace math: arithmetic without ace-math is refused naming the import` — fast, no math:
    `:formula/compare` and `:expr/arith` (the tests page's `ace errors: a refusal for a missing import
    names the import` pins the message with `:data/integer`).
14. `ace math: reals, units, &, terms without value and comparisons of non-numbers are refused by id` —
    3596, 2624, 2873, 3527, `John likes 7 / 2.` (no `exquo` fact afterwards), 3319 (from M7); the graph
    unchanged.
15. `ace math: comparatives, primality, divisors and derivatives` — the [vocabulary](#vocabulary) rows.
16. `ace math: every answer re-enters as an accepted statement` — a loop over the answers of 2-6 and 15.
17. `ace math: arithmetic works under another .lang` — `.lang en` before `.import ace-math` (the stack
    lands in `zelph`, `en` is restored), then 2, 3 and 10 again; a native `&30` typed under `.lang ace`
    is `ace`'s number.
18. `ace math: a math check does not lose pending derivations` — auto-run off, a pending `tim ~ man`, a
    check, then a question about `tim` answered through `run-delta` (the engine side is pinned by its
    own doctest of P-drop-delta).
19. Math imported earlier under another language is the tests page's slow test `ace math: math imported
    earlier under another language is the stack ace-math uses` (`.lang en`, `.import math`, `.lang zelph`,
    `.import ace-math`, then `1 + 2 = 3.` is accepted and checked, and the user's language is restored;
    it replaces the refusal test of the withdrawn `[ace/math-lang]`). It is counted among the tests
    page's slow cases, not here.
20. `ace math: a conflicting equation in the graph makes a check unknown` — a native `(&1 + &2) = &4`,
    then `1 + 2 = 3.` gives `[ace/w-not-checkable]`, no record is left, nothing is printed in zelph syntax.
21. `ace math: a block keeps the fact-path stores armed after .import ace-math` — the `ace-math` variant
    of the tests page's `ace work: a block's dry run leaves the fact-path stores armed`. After
    `.import ace-math`, the block `John's age is 29 + 1. 1 + 2 = 3.` (its check-step `ace:math` and its
    `ace:dry` dropped) and the block `What is 2 + 3?` (its question-step `ace:math` dropped):
    `.fact-stores` reports `on` and `.prof` reports `genuine: walks=0`. The comment: before P-scratch-drop
    a cluster drop disarmed the stores for the rest of the session, which put every `ace` plus `ace-math`
    session into the state the Jacobian guard exists to catch (genuine walks from 0 to 667,540, three
    times the run time on the math stack), and that guard cannot see it, because its workload drops no
    cluster. Check mode leaves the walk count unchanged, so no opt-out.

Three more tests that touch arithmetic stand in the tests page's own areas and are counted there:
`ace math: after a load arithmetic needs ace-math again` (M5: after `.import ace-math` and the `.load`
of an unrelated saved graph, `John's age is 30.` gives `[ace/needs-math]` although `(ace/state :math)`
survived; after a second `.import ace-math` the sentence is accepted and numbers print as numbers
again), `ace constructs: a "not provably" sentence beside the math stack keeps its own negation level`
(M5: the ACE rules stand in stratifiable components of their own beside the rules of `ace-math`), and
`ace constructs: "not provably" beside a rule over predicates is refused` (M4: the refusal that
[values before negation](#values-before-negation) relies on).

**Module tests** (`src/test/test_evaluation.cpp`, [DEC-MATH-MODULE](decisions.md#dec-math-module)): one TEST_CASE each for `exquo`,
`:simplify` over `exquo`, `difference` and `compare`, each over the three substrates
(`run_arithmetic_modules`), with the outputs of [the evaluation module](#the-evaluation-module) and no
contradiction; no ACE in them.

**Budget.** The slow TEST_CASEs of this page are the corpus TEST_CASE of layer A, layer C's 1-11, the
slow part of 14, 15-18, 20, 21 and the four module tests: 23, the reserve the tests page holds for them.
The tests page counts 19, its math-language test, and the three tests above among its own cases. 12
and 13 import no math and are fast; 14 splits into a fast part (the ids without a route, refused
without `ace-math`) and a slow part (`:expr/as-data` is reached only with `ace-math`). All of ACE
shares the slow allowance of the install tier, `s ≤ 1.25·F − N + 0.25·f` with F fast and N total cases
at the start of a milestone, s and f the new slow and fast ones ([The Install
Tier](tests.md#the-install-tier)); F and N are read again at the start of M0 and of every milestone,
so this page states the formula and no number.

## Build Steps inside M5

The order of work is the work plan's ([M5 — Arithmetic](plan.md#m5-arithmetic)); these are the steps,
in that order.

1. Typing, the detection of `formula/` and `expr/`, negated formulas, definitions, identity planning
   and the identity decisions of the resolution step; the own-sentence oracles of layer A and the
   identity tests. No math is needed (the corpus TEST_CASE imports `ace-math` and follows step 3).
2. `stdlib/evaluation.zph` from the starting text above, with `src/test/test_evaluation.cpp` over three
   substrates and `mkdocs/docs/math/evaluation.md`. A general module: its documentation and tests say
   nothing about ACE.
3. The skeleton of `ace-math.zph`: the language lines, the imports, the hook table, `ace/num!`,
   `ace/math-term!`, `ace/math-numeral`; `[ace/needs-math]` without it; node identity with islands.
4. The scratch protocol (trust, the shape test); ground checks, ground values in atoms, value
   questions; the round trip.
5. Math variables, the ring order, answers in normal form, math-universal implications (needs
   `zelph/parse`).
6. Formulas in rules, values before negation.
7. The vocabulary.
8. Documentation: the "Numbers and formulas" part of the user page `mkdocs/docs/ace.md` — `.import
   ace-math` and that it imports math under `.lang zelph`; the differences from `$( … )` (`2^3^2` is 64 in
   ACE and 512 in the island, `-2^2` is 4 and −4, `X-1` is a word, `/` is exact, free variables are
   existential, integers only); the closed-world `\=`; the answer forms; refusals by id; every example a
   real run, the prose through the project's prose filter, ACE sentences in backticks. Also the
   quickstart line `.import ace-math             # ACE with numbers and formulas (imports math)` and
   the page of the evaluation module with its entry in the navigation.

## Not in v1

Each of these is a route that the design leaves open; see [Math, User Experience and
Host](routes.md#math-user-experience-and-host) on Design Routes Taken and Not Taken.

| Later route | What it opens | Cost |
|---|---|---|
| exact rationals | ACE's `/` with its real meaning, decimals in data (`50.5 kg`), `3.14 = 157/50` | large: a greatest common divisor in rules, sign normalisation, three substrates |
| storing a term without value as a term (`:expr/as-data`) | `John likes 7 / 2.` | harmless since `/` is `exquo` (silent where native `/` lied), but equal values become different nodes (`14 / 4` against `7 / 2`), and every stored term is a request |
| units as `~ symconst` factors | dimensional algebra by `topoly` (`2cm * 30mm * 1m` gives `60*cm*m*mm`) | small for the algebra, conversions extra |
| string concatenation over character lists | `:expr/concat`, serialisation rules in the style of 3669 | medium, and not arithmetic |
| positivity over the naturals | `For every natural number X, X^2 + X + 1 > 0` decidable (all coefficients ≥ 0) | one more rule |
| ACE as the language of proofs | math answers justified in ACE (`1 + 2 = 3 because …`), which the justified answers of M9 leave without support lines | medium: the proof of the scratch run (P-explain) read before `ace:math` is dropped, and ACE sentences for the math stack's steps |
| function nouns beyond the vocabulary above | `the exponential of X` as `(exp of X)` | the next step of DEC-OF |

Rejected: an ACE prelude with a demand transformation; `f(X)` inside `ace` blocks (it mixes zelph syntax
into ACE, which invariant 11 rules out); a value request of the evaluation module's own (an
`:evaluate` request would be a second implementation of `:simplify`, DEC-MATH-MODULE).

## Evidence

The claims of this page rest on these runs. E1-E12 were first made on an older tree and repeated on the
engine before the host work; E13-E20 were made on that engine with the module's starting text; E16 and
E20 were checked again after the merge that keyed negation levels by pattern. The real runs on this
page repeat the central ones on today's engine.

| Id | What it shows |
|---|---|
| E1 | late binding: a function compiled before `ace/math-later` exists is a compile error; a call through the hook table works once the table is set |
| E2 | early binding: a function compiled before `.import binary-arithmetic` keeps the fallback `zelph/number` |
| E3 | before P-number-lang the math stack was bound to its import language: under `.lang en` numbers and operators were other nodes and `? &1 + &2` answered nothing |
| E4 | decisions under `.lang en` failed for numbers built there and worked for numbers built under `zelph` |
| E5 | a run inside a dropped cluster left `run-delta` with a stale fixpoint (`tim mortal false` until a full `.run`); the reason for P-drop-delta |
| E6 | numbers from digit nodes resolved in `zelph` are the right node; `zelph/int` under `en` built other nodes on every substrate |
| E7 | the scratch protocol with the earlier 35-rule module: `12 / 4` gives `&3`, `.stat` unchanged |
| E8 | node and rule counts of the imports |
| E9 | `!=` guards and an identity denial; equal strings are one node; `(zelph/list-chars "")` is nil |
| E10 | a stored arithmetic subterm is a request: `john likes (&7 / &2)` derives `(&7 / &2) = &3` |
| E11 | the family counts of negated formulas over the corpus: N 509, F 19, seven N cases only through negated formulas |
| E12 | `2-X` under the tokenizer's rule; `X-1`, `X1-Y` are words; `3-5` is a subtraction |
| E13 | the evaluation module on three substrates: 51 requests, identical outputs, no contradiction |
| E14 | the symbolic rows: verdicts and differences over math variables and the constant `Pi`; 6135 nodes, 456 rules |
| E15 | the scratch protocol through `:simplify`: values and verdicts read as text, `.stat` unchanged |
| E16 | formulas in rules: seeds, verdict conditions, guarded value bindings, a denial, "not provably" |
| E17 | without the guard a value binding stores unreduced terms (`kid halfage (&9 exquo &2)`) |
| E18 | a declared conflicting equation gives a request two answers and two normal forms; inside `ace:math` the record goes with the drop |
| E19 | the node of a math variable: `"X"` is one node, the variable `X` stays a variable, the island reads `"X"` back |
| E20 | values before negation: the plain negated formula over a compound term concludes too early; the bound shape is right in every order |
