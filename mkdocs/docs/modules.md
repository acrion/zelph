# Scripts and Modules

## Multi-language Support

zelph allows nodes to have names in multiple languages. This feature is particularly useful when integrating with external knowledge bases. The preferred language can be set in scripts using the `.lang` command:

```
.lang zelph
```

This capability is fully utilized in the Wikidata integration, where node names include both human-readable labels and Wikidata identifiers. An item in zelph can be assigned names in any number of languages, with Wikidata IDs being handled as a specific language ("wikidata"). The way RDF terms connect to names and naming languages is specified in [Labels, Names and Languages](rdf-spec/names.md): IRIs serving as names within their own language, labels accompanied by a language tag as names in that language, and the fallback behaviour of a lookup when a language lacks a name.

A `.lang` placed within a module, a script brought in via `.import` or `zelph/import`, applies exclusively to the module’s own lines. Upon completion of the import, the importer’s language is restored, even if the module halted due to an error, and a nested import gives the surrounding module its own language back in the same way. A module can thus name its vocabulary in a fixed language without moving the session into it. An `.identity` within a module functions as the module’s own, in the same way: it sets the identity of the module’s file, within which the file’s [blank nodes](concepts.md#blank-nodes) are named, from its line onward to the file’s end, and leaves the identity of the importer unchanged.

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

When a script is executed as a session (`zelph script.zph`, as detailed below), it maintains its `.lang`: each line functions as though it had been manually typed. The sole exception is a blank node written as `_:label`, which belongs to the file rather than to the session (see below).

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
as though its lines were entered directly, fulfilling the promise made by the
shebang `#!/usr/bin/env zelph`, with one exception: a
[blank node](concepts.md#blank-nodes) written `_:label` belongs to the file
rather than to the session, meaning the file consistently names the same blank
nodes across every run, and an `.identity` within the file establishes the
file’s identity. Statements are echoed, they anchor the
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

`zelph --default '<setting line>' <script>` makes a setting a default of the process. The line executes prior to session start, without producing output, and also after every `.new`, which otherwise returns each engine setting to the default that is built in. The line must be one of the setting commands `.lang`, `.parallel`, `.threads`, `.anchors`, `.semi-naive`, `.fact-stores`, `.contradiction-records`, `.negation-records`, `.explain-budget`, `.node-map-load-factor`, `.log`, or `.log-janet`, or one of the three session settings, `.auto-run`, `.deductions`, and `.timer`, which `.new` keeps and thus are set only once, at startup. It carries the value it sets, as in `--default '.lang de'`; `.parallel`, `.log-janet`, and `.auto-run` are toggles and stand alone. The option may be specified multiple times, with its lines applying in the sequence provided. A line that is not one of these commands, or that its command rejects, terminates the process with status 1 before any execution occurs. The option comes before the script’s name: after the name, the identical two words are passed to the script as its arguments. The quotation marks are those used by the shell and keep the line as a single argument; in `cmd.exe`, double quotes perform this function.

```bash
zelph --default '.node-map-load-factor 0.9' --default '.anchors off'
```

```
zelph> .node-map-load-factor
Node map load factor from 2^31 buckets: 0.9 (default)
zelph> .anchors
Anchor-based lookups: off
zelph> .anchors on
Anchor-based lookups: on
zelph> .new
Cleared network and re-initialized core nodes.
zelph> .anchors
Anchor-based lookups: off
zelph> .node-map-load-factor 0.95
Node map load factor from 2^31 buckets: 0.95
zelph> .node-map-load-factor default
Node map load factor from 2^31 buckets: 0.9 (default)
```

The value of such a line is the session’s default in every sense. `.node-map-load-factor` and `.explain-budget` are the two settings that mark the default in their status line and take the word `default` as their argument, and for both of them the default is the value of the line: the status line marks it, and `default` restores it.

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

When a load supersedes a network, it also purges the registry: the network being replaced carries with it whatever a module had stored inside it – its rules and facts – and the display registrations of its Janet code, which refer to nodes within that network. Consequently, the subsequent `.import` of that module triggers its execution once again, causing the module to re-register its keywords, display schemes, digit alphabet and signed form; its Janet definitions are merely re-established. A module that loads a network itself maintains its own entry, since its lines after the load execute on the loaded network.

### Rules Say Themselves Only Once

Facts are hash-consed: the node *is* its own structure, meaning that asserting the same fact twice results in no change. (Two different facts only share a node when their 62-bit hashes collide; zelph then keeps the first one and records nothing from the second, as detailed in [The Identity Foundation](internals/performance.md#the-identity-foundation).) A rule requires one additional step. It includes variables, which are allocated fresh for every statement, and a node built from newly allocated variables constitutes a fresh node – hence, entering the same rule twice would yield two rules deriving identical consequences, doubling the unification cost.

zelph therefore recognises a rule it already has. A `... => ...` statement is compared against the existing rules **up to a renaming of its variables** (alpha-equivalence, as in the lambda calculus); if one matches, the newly built rule is rolled back and the statement evaluates to the rule that was already there. What constitutes "the same rule" is determined by what the reasoner reads – set membership, the conjunction tag, and each condition’s subject, predicate, and objects, including any negated condition. A set of conditions within a term, such as the one `(X q Y) => ((X p Y, X r Y) is noted)` refers to, is treated as a constant by the reasoner, just as is the set of a rule quoted in a consequence: a firing writes its facts regarding that exact set. A rule’s own set of conditions has more readers than the firing: a rule over rules pairs the members of a ground rule’s set by node, a variable bound at the subject of a ground rule binds the set itself, and the membership of a ground condition within it answers a query. The comparison therefore reads it as a term as well. A rule that a condition names or quotes is compared in the same way, as a term, even though a condition matches the sets of conditions of two such rules based on their members: two rules that name or quote rules over two named sets with identical members remain distinct. The set that a statement’s own text writes for its rule, and that only the rule itself holds, is compared by its members, a named set among them by its node, against any set that is not named; any other two such sets are regarded as the same term only if they are the same node, and each stands for one set of the other rule wherever it stands, the rule’s own conditions included. Two rules over two named sets of the same members are therefore two distinct rules, and so are two rules over two collections of the same conditions that a program made. A set of ground conditions expressed explicitly, `(*{(a s b) (c t d)} ~ conjunction)`, is a set constant, which every statement over those conditions builds again, so it is not the property of any single statement and is compared by its node: a rule over it is not the same as the rule over the comma list `(a s b, c t d)`, and `.list-rules` prints such a rule in that exact spelling, so the line printed and re-entered is the identical rule. When a firing writes a set of conditions as it stands, within a consequence outside a `=>` and in the consequences of a rule that the rule writes, two sets are also read as the firing writes them: a list the statement writes, whose members the firing’s claim asserts, is not the same term as a rule’s set or a set that a construction built, which the firing writes via its node. The rule `(X q k1) => ((a s b, c t d) noted X)` therefore stays beside the rule that `(C => (e f g)) => ((X q k1) => (C noted X))` writes over the set of `(a s b, c t d) => (e f g)`: only the typed rule’s firing transforms `a s b` and `c t d` into data, and with them `e f g`. A set that an inline keyword makes anew for each statement is not the statement’s own text, so a rule over it is written again by every statement. One set is read as the firing reads it: where a rule’s whole set of conditions has exactly one member, that member holds a variable, and the set does not appear elsewhere in the rule, the rule is not ground and the membership does not answer a query, so the set is compared as its member. The four spellings of a rule with one condition under [What the conjunction tag decides](logic.md#what-the-conjunction-tag-decides-and-what-it-does-not) are one rule for this reason, while `@{(a p b)} => (e f u)`, `{(a p b)} => (e f u)` and `(a p b) => (e f u)`, whose condition is ground, are three rules that fire alike. Thus, a statement is rolled back only for a rule that derives identical facts, excluding those about sets the statement would have made itself, with one exception: the conjunction tag in `(*{(X p Y)} ~ conjunction) => (X q Y)` answers a query, so when that spelling is entered after one of the other three and subsequently rolled back, the outcome that a rule over the tag, such as `(S ~ conjunction) => (k tagged yes)`, would have derived from it is lost.

The check might fail to identify a rule the graph already holds, in which case both are kept: once the path to a variable, to a set of conditions, or to a collection literal traverses approximately fourteen levels of nesting – counting each set of conditions within the rule’s own set, each collection literal within a term, each element of a list, and each fact within a condition or a consequence – or between rules that reach sets holding one another once their fingerprint or comparison exceeds its budget of 100,000 positions or pairs or the comparison exceeds the stack depth of its thread. Both may also be retained where a rule is typed before a generator writes its twin, and where two generators write the same rule: a generator’s rule refers to a set that the generator quotes or makes for it – its set of conditions or a set in a term – which other rules of the generator may also reference; that set is the generator’s own when it holds a variable the firing does not bind, and otherwise the first set of the same members the construction meets, which depends on the node ids. A rule typed after a generator stays beside its twin as well when that twin writes a set the construction made, since a firing writes such a set by its node and enters the list the typed rule writes. A query then gets a second answer about a set that looks alike.

This matters most in a place where it is easy to miss. Using `.load` swaps the current graph for the stored version, which retains a module’s rules but omits its Janet side (`zelph/number`, digit alphabets and signed forms, display schemes); thus, resuming work on a saved arithmetic network in a new session means re-importing the module:

```
.load math.bin
.import math          # brings back &-literals and numeric comparisons -- and adds no second rule set
```

Without rule identity, that sequence would double every rule in the file.

One boundary: the check operates on `=>` statements that have been parsed, encompassing `.import` and the REPL interface, yet excludes [`zelph/rule`](janet.md), `zelph/rule*`, or `zelph/build-rule` – a Janet program building rules dynamically determines its own output, and zelph does not second-guess it. A rule whose text holds a collection literal (`@{...}`, or `{...}` in a term position) is identified just like any other: the collections inside a rule’s text are compared by their members, not by their nodes, whereas an untagged collection that the rule only names, such as one bound by a generator or created by a program, is compared by its node; a collection bearing the conjunction tag is a set of conditions and is compared as one.

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
