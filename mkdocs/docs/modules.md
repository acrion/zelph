# Scripts and Modules

## Multi-language Support

zelph allows nodes to have names in multiple languages. This feature is particularly useful when integrating with external knowledge bases. The preferred language can be set in scripts using the `.lang` command:

```
.lang zelph
```

This capability is fully utilized in the Wikidata integration, where node names include both human-readable labels and Wikidata identifiers. An item in zelph can be assigned names in any number of languages, with Wikidata IDs being handled as a specific language ("wikidata").

## Importing Scripts: Module IDs and Interchangeable Implementations

`.import <script>` loads and executes a zelph (`.zph`) or Janet (`.janet`)
script, resolving first against the working directory and then the standard
library (the `.zph` extension is optional).

### Import once

Every imported `.zph` script is registered under a **module ID** — by default
its lowercase file name without extension (`binary-arithmetic` for
`binary-arithmetic.zph`). A script whose ID is already registered is skipped,
much like `#pragma once` in C++. Scripts can therefore declare their
prerequisites with plain `.import` lines at the top; shared dependencies are
never loaded twice, and import cycles terminate. `.new` clears the registry.
Janet scripts are exempt: they are runnable programs and may be executed
repeatedly, e.g. with different arguments.

The registry is session state and is deliberately **not** part of a `.save`
file — see [Rules Say Themselves Only Once](#rules-say-themselves-only-once)
for why re-importing a module after `.load` is both necessary and free.

### Rules Say Themselves Only Once

Facts are hash-consed: the node _is_ its structure, so asserting the same
fact twice does nothing. A rule requires one additional step. It includes
variables, variables are allocated fresh for each statement, and a node built
from fresh variables is a fresh node – thus, entering the same rule twice would
yield two rules deriving the same consequences at twice the unification cost.

zelph therefore recognises a rule it already has. A `... => ...` statement
is compared against the existing rules **up to a renaming of its
variables** (alpha-equivalence, as in the lambda calculus); if one matches,
the newly built rule is rolled back and the statement evaluates to the rule
that was already there. What counts as "the same rule" is exactly what the
reasoner reads — set membership, the conjunction and negation tags, and each
condition's subject, predicate and objects — so two rules that survive the
check are guaranteed to behave differently.

This matters most in a place where it is easy to miss. `.load` restores the
graph but not the Janet side of a module (`zelph/number`, digit alphabets,
display schemes), so working with a saved arithmetic network means
re-importing the module:

```
.load math.bin
.import math          # brings back &-literals -- and adds no second rule set
```

Without rule identity that sequence doubled every rule in the file.

Two boundaries: the check runs on parsed `=>` statements, which covers
`.import` and the REPL, but not [`zelph/rule`](janet.md) — a Janet program
building rules programmatically owns the nodes it passes in, and zelph does
not second-guess it. And a rule whose conditions mention a literal set
(`{...}` in a term position) is never recognised as a duplicate, because
each such set is a fresh node; the outcome is the old behaviour, never a
wrong one.

### Interchangeable implementations: `.provides`

A script can claim **additional** module IDs with

    .provides <id> [<id2> ...]

placed at the top of the file. Scripts claiming the same ID are
interchangeable — and mutually exclusive — implementations of one capability:
whichever is imported first wins, and later providers of the ID are skipped.

The three arithmetic substrates all declare `.provides arithmetic`. Dependent
modules simply import the default substrate (`.import binary-arithmetic`); to
compute on a different substrate, import it **before** anything that depends
on arithmetic:

    .import binary-nand-arithmetic   # claims "arithmetic" first
    .import symbolic-core            # its ".import binary-arithmetic" is skipped

If a directly requested script is skipped because a _different_ script
already provides one of its IDs, zelph prints a warning — you asked for a
specific implementation, but an alternative is already active.
