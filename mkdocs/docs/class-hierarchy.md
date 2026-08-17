# Working on the Wikidata Class Hierarchy

## What this gives you

Pick two classes that should be disjoint — say _profession_ ([Q215627](https://www.wikidata.org/wiki/Q215627)) and _organization_ ([Q43229](https://www.wikidata.org/wiki/Q43229)). zelph answers, on your own machine and in well under a second:

- **which classes are subclasses of both** — the violation set, which is usually large and by itself unusable as a work list;
- **which few classes actually carry the mistake** — the [_culprits_](#where-this-comes-from), those whose parents are not themselves in violation. Everything else inherits the problem;
- **how much each one is worth fixing** — how many entries of the violation set disappear with it;
- **why each entry is there** — the chain of `P279` statements that produced it. Every arrow in that chain is one statement in Wikidata, and one of them is the edit.

For the pair above, on the pruned 2026-03-09 dump: **18,757 classes are in violation, 81 of them carry the mistake, and a single edit at one class removes 14,498 of the 18,757.**

Nothing here is a query against a public endpoint. There is no timeout, no result cap, and no service that has to be up. You download one file and work offline.

## What you need

1. zelph — see [Quick Start](quickstart.md#installation).
2. One class-hierarchy file. It contains the `P279` ("subclass of") statements of a Wikidata dump and the names of the classes they connect, and nothing else:

```bash
hf download acrion/zelph wikidata-20260309-all-pruned-small-P279.bin --repo-type dataset --local-dir .
```

(Or download it from the [dataset page](https://huggingface.co/datasets/acrion/zelph) in a browser.) It is a few hundred megabytes and loads in about three seconds:

```
zelph> .load wikidata-20260309-all-pruned-small-P279.bin
Auto-run has been disabled due to loading a large dataset.
Loading network from generic file wikidata-20260309-all-pruned-small-P279.bin...
Loading: left chunks=3, right chunks=3, nameOfNode chunks=2, nodeOfName chunks=2
...
String pool size after load: 1666410
Network loaded.
 Time needed for loading/importing: 0h0m2.249s
-- 2.249 s --
zelph-> .stat
Network Statistics:
------------------------
Nodes: 2005552
RAM Usage: 0.6 GiB
Name-of-Node Entries by language:
  wikidata: 890779
  en: 775631
Node-of-Name Entries by language:
  wikidata: 890779
  en: 775631
Languages: 2
Rules: 0
------------------------
```

0.6 GiB of memory. The complete network the file was cut from needs 6.0 GiB, and the full dump behind that needs 223.7 GiB.

## The first result, in three commands

```
zelph-> .import wikidata-classes
Importing file /usr/share/zelph/wikidata-classes.zph...
Wikidata class tools loaded: (culprits A B), (culprit-path X A), (class-report X).
zelph-> %(culprits "Q215627" "Q43229" 10)
Building adjacency index over 1114758 relation nodes (24 thread(s))...
Adjacency index ready: 1114757 edges.
Saved adjacency index to wikidata-20260309-all-pruned-small-P279.bin.pidx.322 (1114757 edges).
below	class
14498	Q703534 (employee)
1761	Q30185
1689	Q1097498
812	Q11974939 (health professional)
46	Q12773225 (slave)
35	Q138001889 (uncle-in-law or aunt-in-law)
31	Q16658574 (sibling-in-law)
13	Q7933191 (armed non-state actor)
11	Q2538889 (weapons manufacturing company)
8	Q12885585 (Native American tribe)
-- 81 topmost culprit(s) of 18757 affected class(es); showing 10 --
-- 220 ms --
```

The adjacency index is built once and then written next to the file, so this happens on the first transitive question only — in this session the same call answered in 109 ms the second time, and a smaller pair (`Q5` against `Q43229`: 216 affected classes, 50 culprits) in 40 ms.

Reading the output:

- **`below`** — how many of the 18,933 affected classes sit at or below this one. Removing the wrong `P279` statement here takes that many entries out of the report in one edit.
- **the list** — only the _topmost_ classes in violation. A class whose parent is already in violation is not listed: the mistake is not there, and it disappears by itself once the parent is fixed.
- **the last line** — the raw count next to the number of places you actually have to look. The gap between 18,757 and 81 is the point of the report.

The third argument is how many rows to print (default 25). Leave it out to see more.

## Why is this class in violation?

The list says where to look. The chain says what to look at:

```
zelph-> %(culprit-path "Q703534" "Q43229")
P279 chain, 4 class(es):
  Q703534 (employee)
  -> Q852998
  -> Q431603 (advocacy group)
  -> Q43229 (organization)
zelph-> %(culprit-path "Q703534" "Q215627")
P279 chain, 4 class(es):
  Q703534 (employee)
  -> Q327055
  -> Q129586023 (person with an activity)
  -> Q215627
```

Each arrow is one `P279` statement in Wikidata. _employee_ reaches _organization_ through three of them, and _profession_ through three others. Both chains are what makes it a violation; breaking either one resolves it. Which statement is the wrong one is an editorial judgement — the tool narrows 18,933 classes down to a handful of statements and leaves that judgement to you.

The shortest chain is reported, because that is the one that can be checked by hand. If a class is reachable along several paths, fixing one may leave the others; re-run `culprits` after an edit to see what is left.

Two inputs that look like a disjoint pair and are not: the same class twice is refused outright, and a pair where one class sits below the other is answered with a note, because everything under the lower one is then reported for that reason alone rather than because anything is wrong.

A single class, if you want its immediate surroundings:

```
zelph-> %(class-report "Q703534")
Class: Q703534 (employee)
Subclasses (transitive): 14633
Direct superclasses: 2
  P279 -> Q327055
  P279 -> Q852998
```

## The same thing in SPARQL

If you would rather write the query yourself, the [SPARQL layer](sparql.md) works on this file too and gives the same 81 classes:

```
zelph-> .import sparql
zelph-> sparql
PREFIX wd: <http://www.wikidata.org/entity/>
PREFIX wdt: <http://www.wikidata.org/prop/direct/>
SELECT DISTINCT ?class WHERE {
  ?class wdt:P279+ wd:Q215627 .
  ?class wdt:P279+ wd:Q43229 .
  MINUS {
    ?class wdt:P279 ?parent .
    ?parent wdt:P279+ wd:Q215627 .
    ?parent wdt:P279+ wd:Q43229 .
  }
}

```

(A blank line runs the query.) What SPARQL cannot express is the ranking: `MINUS` gives you the 81 topmost classes as a set, in no particular order. The `below` column comes from counting, per candidate, how much of the violation set hangs underneath it — which is one closure per candidate, not one query.

## What is in the file, and what is not

The file is a **predicate slice**: every `P279` statement of the dump, the classes those statements connect, their Wikidata IDs and their English labels. That is all.

- Other properties are **not** in it. `?x wdt:P31 ?y` answers nothing; instance-of questions need a different slice or the full network.
- Classes that take part in no `P279` statement are not in it either. Asking about one says so instead of quietly answering "no violations":
  ```
  error: 'Q4711' takes part in no P279 fact in this network. Either the ID is
  wrong, or this network does not carry the class hierarchy.
  ```
- English labels are missing for some classes, because the dump does not carry one for them. They appear as bare IDs (`Q30185` above).
- The **pruned** dump has whole domains removed (biology, chemistry, astronomy and more) to keep the full network within 6.0 GiB. Its class hierarchy is correspondingly smaller: 1.11 million `P279` edges against 5.17 million in the complete dump. Numbers from a pruned file are not the numbers of Wikidata. If a slice of the complete dump is published, it is named without `-pruned` and used exactly the same way.

## Reproducibility

Everything above is computed from one file, and that file is one pinned dump. The same file gives the same answers tomorrow, on another machine, to another person — which is what makes a finding citable and a fix verifiable. The dump date is part of the file name (`wikidata-20260309-…`); state it when you report a number.

Wikidata itself moves on, of course: a class that is in violation in the file may already have been fixed on the Hub. The chain output gives you the statements to check, so verifying against the live item is one click per arrow.

### A worked example of a disagreement

That is not a footnote. Here is a query on the 2026-03-09 `-small` slice — classes below _class_ ([Q5127848](https://www.wikidata.org/wiki/Q5127848)) that do not go through _class_ in the metaclass sense ([Q16889133](https://www.wikidata.org/wiki/Q16889133)):

```
zelph-> .import sparql
zelph-> sparql
SELECT ?cls WHERE {
  ?cls wdt:P279* wd:Q5127848 .
  MINUS { ?cls wdt:P279* wd:Q16889133 }
}

-- 201 result(s) --
-- 1.050 s --
```

Against live Wikidata the same query answers a single-digit number. Neither side is miscounting. In the pinned dump, `Q5127848` has five direct subclasses, and two of them — _class of anatomical entity_ ([Q112826905](https://www.wikidata.org/wiki/Q112826905)) and _rank_ ([Q4120621](https://www.wikidata.org/wiki/Q4120621)) — have since been re-parented to `Q16889133`, which is exactly the kind of edit this page is meant to prompt. Their subtrees are the difference; subtract them and the file agrees:

```
zelph-> sparql
SELECT ?cls WHERE {
  ?cls wdt:P279* wd:Q5127848 .
  MINUS { ?cls wdt:P279* wd:Q16889133 }
  MINUS { ?cls wdt:P279* wd:Q112826905 }
  MINUS { ?cls wdt:P279* wd:Q4120621 }
}

?cls
Q5127848
Q64549097
Q3356722 (natural kind)
Q111973176 (PM20 ware category)
Q124883403 (Scope 3 category)
Q125121174 (OpenStreetMap tool category)
Q111282816 (Little brown mushrooms)
-- 7 result(s) --
```

Two statements moved, 194 results changed. Both numbers are right about the data they were computed from — but only one of them can still be checked, a year from now, by someone who was not there.

## How it works, in one paragraph

zelph is not a query engine over triples; it is an inference engine whose graph happens to answer queries. Two properties matter here. First, a network can be cut down to the statements of chosen predicates and still be a complete network — that is what `.save-predicates` produces, and why the class hierarchy can be shipped as a file that is three orders of magnitude smaller than the dump it comes from. Second, transitive questions are not answered by walking triples per query but by a closure engine over an adjacency index that is built once per predicate and cached next to the file (`.pidx.322` above). The 18,757 affected classes and the 81 culprits fall out of set operations on two closures. `stdlib/wikidata-classes.zph`, which `.import wikidata-classes` loads, is 200 lines of ordinary script on top of the public API — you can read it, change it, and ask different questions with it.

If you want to produce such a file yourself — for another dump, another property, or a combination — see [Publishing a Predicate Slice](publishing-slices.md).

## Where this comes from

The word _culprit_, and the way of counting used here, are from Ege Atacan Doğan and Peter F. Patel-Schneider, [_Disjointness Violations in Wikidata_](https://arxiv.org/abs/2410.13707) (2024). That paper analyses how disjointness is modelled in Wikidata, categorises the patterns that produce violations, and gives SPARQL queries that identify the culprits.

Two things in it say what this page is for.

The first is where the paper stops. Finding the culprits is mechanical; finding the **mistake behind** one is not, and the paper says so plainly — determining mistakes "requires manually examining the information in Wikidata", and "cannot be done with a simple formula and then retrieved using a simple query like determining culprits". Their own largest case is _gene_ ([Q7187](https://www.wikidata.org/wiki/Q7187)): 10,656 of the culprits they found are subclasses of it, and that was established by hand.

The second is what the paper asks for next. Its section on crowdsourcing fixes wants a tool that shows "disjointness culprits in context and pointed out changes that would eliminate the issue".

That is what `culprits` and `culprit-path` are: the culprits ranked by how much each fix is worth, and for each one the chain of `P279` statements that put it there, so the context is on the screen rather than in twelve browser tabs. What the tool deliberately does not do is decide which statement in the chain is the wrong one. That remains the editorial judgement the paper describes — the difference is that it is now a judgement about three or four statements instead of about a five-figure list.

Their case is in the file used here, so you can start from it:

```
zelph-> %(class-report "Q7187")
Class: Q7187
Subclasses (transitive): 23600
Direct superclasses: 3
  P279 -> Q15712714 (biomolecular structure)
  P279 -> Q50365914 (biological region)
  P279 -> Q863908 (nucleic acid sequence)
```

Three statements above a class that carries five figures of consequences — which is the shape of every entry in the report.

## Reporting problems

If a number here looks wrong, it is worth reporting: [github.com/acrion/zelph/issues](https://github.com/acrion/zelph/issues). Please include the file name (it carries the dump date), the exact command, and what you expected. A disagreement with a SPARQL endpoint is a useful report even when zelph turns out to be right — both sides have been wrong before.
