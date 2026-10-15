# Scripts and Modules

## Multi-language Support

zelph allows nodes to have names in multiple languages. This feature is particularly useful when integrating with external knowledge bases. The preferred language can be set in scripts using the `.lang` command:

```
.lang zelph
```

This capability is fully utilized in the Wikidata integration, where node names include both human-readable labels and Wikidata identifiers. An item in zelph can be assigned names in any number of languages, with Wikidata IDs being handled as a specific language ("wikidata").

A `.lang` placed within a module, a script brought in via `.import` or `zelph/import`, applies exclusively to the module’s own lines. Upon completion of the import, the importer’s language is restored, even if the module halted due to an error, and a nested import gives the surrounding module its own language back in the same way. A module can thus name its vocabulary in a fixed language without moving the session into it.

The arithmetic modules use this method to name everything in `zelph`. When imported under `.lang en`, they leave the session in `en`, and their digits and rules are the nodes they would be under `.lang zelph`. An `&`-literal refers to the identical node across every language; operators are found by name just like any other node, hence a computation is written under `.lang zelph`:

```
zelph> .lang en
en> .import decimal-arithmetic
en+> .lang
The current language is 'en'
en+> .lang zelph
zelph+> ? &3 + &4
Answer: (&3 + &4) = &7
```

When a script is executed as a session (`zelph script.zph`, as detailed below), it maintains its `.lang`: each line functions as though it had been manually typed.

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

In a session script, `.new` resets the network, and subsequent lines execute on
the reset state, just as they would when processed through a pipe. Within a
module, this is invalid: the library does not clear the session that loads it,
thus the import stops at that line.

### Import once

Every imported `.zph` script is registered under a **module ID** — by default
its lowercase file name without extension (`binary-arithmetic` for
`binary-arithmetic.zph`). A script whose ID is already registered is skipped,
much like `#pragma once` in C++. Scripts can therefore declare their
prerequisites with plain `.import` lines at the top; shared dependencies are
never loaded twice, and import cycles terminate. The command `.new` clears the
registry, as does any load operation that replaces the current network (refer
to the section below). Janet scripts are exempt: they are runnable programs
and may be executed repeatedly, e.g. with different arguments.

The registry is session state and is deliberately **not** part of a `.save`
file — see [Rules Say Themselves Only Once](#rules-say-themselves-only-once)
for why re-importing a module after `.load` is both necessary and free.

When a load supersedes a network, it also purges the registry: the network being replaced carries with it whatever a module had stored inside it – its rules and facts – and the display registrations of its Janet code, which refer to nodes within that network. Consequently, the subsequent `.import` of that module triggers its execution once again, causing the module to re-register its keywords, display schemes, and digit alphabet; its Janet definitions are merely re-established. A module that loads a network itself maintains its own entry, since its lines after the load execute on the loaded network.

### Rules Say Themselves Only Once

Facts are hash-consed: the node *is* its own structure, meaning that asserting the same fact twice results in no change. (Two different facts only share a node when their 62-bit hashes collide; zelph then keeps the first one and records nothing from the second, as detailed in [The Identity Foundation](internals/performance.md#the-identity-foundation).) A rule requires one additional step. It includes variables, which are allocated fresh for every statement, and a node built from newly allocated variables constitutes a fresh node – hence, entering the same rule twice would yield two rules deriving identical consequences, doubling the unification cost.

zelph therefore recognises a rule it already has. A `... => ...` statement
is compared against the existing rules **up to a renaming of its
variables** (alpha-equivalence, as in the lambda calculus); if one matches,
the newly built rule is rolled back and the statement evaluates to the rule
that was already there. What counts as "the same rule" is exactly what the
reasoner reads — set membership, the conjunction tag, and each condition's
subject, predicate and objects, including any negated condition — so two
rules that survive the check are guaranteed to behave differently.

This matters most in a place where it is easy to miss. Using `.load` swaps the current graph for the stored version, which retains a module’s rules but omits its Janet side (`zelph/number`, digit alphabets, display schemes); thus, resuming work on a saved arithmetic network in a new session means re-importing the module:

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
