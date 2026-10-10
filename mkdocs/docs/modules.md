# Scripts and Modules

## Multi-language Support

zelph allows nodes to have names in multiple languages. This feature is particularly useful when integrating with external knowledge bases. The preferred language can be set in scripts using the `.lang` command:

```
.lang zelph
```

This capability is fully utilized in the Wikidata integration, where node names include both human-readable labels and Wikidata identifiers. An item in zelph can be assigned names in any number of languages, with Wikidata IDs being handled as a specific language ("wikidata").

## Two ways to run a script

A script file reaches zelph on one of two paths, and they differ in what the
file IS to the engine.

`.import <script>` loads a **module**: a script whose purpose is to define
things for the session around it. Its own lines are not echoed, its statements
are not anchors for the deduction filter (see
[Deduction Output Modes](rules.md#deduction-output-modes)), and inference runs
once when the whole file has been read rather than after every line. It
resolves first against the working directory and then the standard library
(the `.zph` extension is optional), and it accepts `.zph` and `.janet` only.

`zelph <script> [args...]` from the shell runs a **session**: the file behaves
exactly as if its lines had been typed, which is what the shebang
`#!/usr/bin/env zelph` promises. Statements are echoed, they anchor the
deduction filter, and inference runs after each of them — so a line can read
what the line before it derived. Everything about finding and preparing the
file is shared with `.import`: the same resolution order, the same `.janet`
runner, the same script arguments, the same module registry below. The one
difference is the file NAME: a session script that exists is run whatever it
is called, because a shebang script is normally called `report` rather than
`report.zph`.

```bash
zelph stdlib/examples/english.zph      # a session: echoes, derives, prints
zelph < stdlib/examples/english.zph    # the same session, read from a pipe
```

Inside the REPL the same file is a module:

    .import examples/english

`.quit` ends a session script where it stands. A module ignores it — a library
that stopped the session it is being loaded into would be a surprising thing.

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

Facts are hash-consed: the node *is* its own structure, meaning that asserting the same fact twice results in no change. (Two different facts only share a node when their 62-bit hashes collide; zelph then keeps the first one and records nothing from the second, as detailed in [The Identity Foundation](internals/performance.md#the-identity-foundation).) A rule requires one additional step. It includes variables, which are allocated fresh for every statement, and a node built from newly allocated variables constitutes a fresh node – hence, entering the same rule twice would yield two rules deriving identical consequences, doubling the unification cost.

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

One boundary: the check operates on `=>` statements that have been parsed, encompassing `.import` and the REPL interface, yet excludes [`zelph/rule`](janet.md), `zelph/rule*`, or `zelph/build-rule` – a Janet program building rules dynamically determines its own output, and zelph does not second-guess it. A rule whose text holds a collection literal (`@{...}`, or `{...}` in a term position) is recognised like any other: the collections within a rule’s text are compared by their members, not by their nodes.

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
specific implementation, but an alternative is already active. This is one of
the cases that make zelph **exit with a non-zero status**: your request was not
carried out, so the session that follows is not the one you asked for. (The
partial-view warning of [Sharding](sharding.md) is not such a case; it
accompanies an operation that is legitimate.)
