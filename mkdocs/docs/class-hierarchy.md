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

- **`below`** — how many of the 18,757 affected classes sit at or below this one. Removing the wrong `P279` statement here takes that many entries out of the report in one edit.
- **the list** — only the _topmost_ classes in violation. A class whose parent is already in violation is not listed: the mistake is not there, and it disappears by itself once the parent is fixed.
- **the last line** — the raw count next to the number of places you actually have to look. The gap between 18,757 and 81 is the point of the report.
- **a `note:` line under it**, when the report cannot speak for every affected class — see [What the report cannot see](#what-the-report-cannot-see). There is none here, which is itself information: all 18,757 are covered by the 81.

The third argument is how many rows to print (default 25). Leave it out to see more.

## What the report cannot see

A class is omitted from the list when a class higher up is already in violation – that is what _topmost_ means, and it is what reduces 18,757 entries to 81. The definition has one gap. If the classes in violation above a class form a `P279` cycle, then every one of them has a superclass in violation, namely its own predecessor in the cycle, so none of them is topmost. Neither the cycle nor anything beneath it shows up in the list, however many rows you print.

The report counts those classes instead of passing over them. Here is the pair Doğan and Patel-Schneider work through in their section 4 – _concrete object_ ([Q4406616](https://www.wikidata.org/wiki/Q4406616)) against _abstract entity_ ([Q7048977](https://www.wikidata.org/wiki/Q7048977)):

```
zelph-> %(culprits "Q4406616" "Q7048977" 3)
below	class
287263	Q27096213 (geographic entity)
114093	Q46344 (quantum)
56677	Q135899982
-- 1147 topmost culprit(s) of 467701 affected class(es); showing 3 --
   note: 27297 of the 467701 affected class(es) (5.8%) sit below no topmost culprit, so nothing above stands for them
         -- every affected class above them has an affected superclass of its own, which is what a P279 cycle produces
-- 3.071 s --
```

The 1,147 rows cover 440,404 of the 467,701 affected classes, or 94.2 %. The other 27,297 sit under a cycle such as this one:

```
zelph-> %(class-report "Q340169")
Class: Q340169 (communications media)
Subclasses (transitive): 70014
Direct superclasses: 2
  P279 -> Q12774177 (means)
  P279 -> Q104450446 (data carrier)
```

_communications media_ is a subclass of _data carrier_, which is a subclass of _manifestation_, which is a subclass of _communications media_ again. All three are in violation and all three report the same 70,014 transitive subclasses, because each of them is below the other two.

A cycle defeats the ranking, not the explanation. `culprit-path` answers for such a class exactly as it does for a ranked one:

```
zelph-> %(culprit-path "Q340169" "Q4406616")
P279 chain, 5 class(es):
  Q340169 (communications media)
  -> Q104450446 (data carrier)
  -> Q17538423 (manifestation)
  -> Q223557 (physical object)
  -> Q4406616 (concrete object)
```

Breaking one edge of a cycle is therefore worth more than its `below` column would ever have said: it is what puts the classes underneath it within reach of a work list at all.

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

Each arrow is one `P279` statement in Wikidata. _employee_ reaches _organization_ through three of them, and _profession_ through three others. Both chains are what makes it a violation; breaking either one resolves it. Which statement is the wrong one is an editorial judgement — the tool narrows 18,757 classes down to a handful of statements and leaves that judgement to you.

The shortest chain is reported, because that is the one that can be checked by hand. If a class is reachable along several paths, fixing one may leave the others; re-run `culprits` after an edit to see what is left.

Two inputs that look like a disjoint pair and are not: the same class twice is refused outright, and a pair where one class sits below the other is answered with a note, because everything under the lower one is then reported for that reason alone rather than because anything is wrong.

A single class, if you want its immediate surroundings:

```
zelph-> %(class-report "Q703534")
Class: Q703534 (employee)
Subclasses (transitive): 14497
Direct superclasses: 2
  P279 -> Q327055
  P279 -> Q852998
```

## Asking for what is not there

The report above answers one fixed question. The next question an editor has is usually the negative one – which classes under here are _not_ under there – and that is a condition you can write. Two rules over the direct subclasses of _organization_ ([Q43229](https://www.wikidata.org/wiki/Q43229)), splitting them by whether they also reach _profession_ ([Q215627](https://www.wikidata.org/wiki/Q215627)):

```
zelph-> .lang wikidata
wikidata-> .auto-run
Auto-run is now enabled.
wikidata> .deductions off
Deduction printing mode: off
wikidata> (C P279 Q43229, C P279⁺ Q215627) => (C in-violation Q215627)
(((C P279 Q215627) closure one-or-more), (C P279 Q43229)) => (C in-violation Q215627)
 (skipped 9 deductions)
wikidata> (C P279 Q43229, ¬(C P279⁺ Q215627)) => (C sound-under Q43229)
((C P279 Q43229), ¬((C P279 Q215627) closure one-or-more)) => (C sound-under Q43229)
 (skipped 815 deductions)
```

The `.auto-run` is not optional here: `.load` switches inference off, because
running it after every line of a bulk import would be ruinous, and it says so
when it does. The `-` in the `zelph->` prompt is the reminder. A rule entered
while inference is off is stored and does nothing until it is switched back on
(or `.run` is typed).

The echo shows what the `⁺` stands for as sugar: the condition is the ordinary
fact `(C P279 Q215627) closure one-or-more`.

_organization_ has 824 direct subclasses in this file. **Nine of them reach _profession_ as well** – among them _militia_ ([Q153936](https://www.wikidata.org/wiki/Q153936)) and _credit bureau_ ([Q1187145](https://www.wikidata.org/wiki/Q1187145)) – and 815 do not. An organization is not a profession, so those nine are where to look, and the 815 are what says the other question was asked too. `.deductions off` keeps each run to one line; without it every derived fact prints its own.

Two points arise from these being _rules_ rather than a query.

Every answer carries its derivation:

```
wikidata> .explain (Q153936 in-violation Q215627)
Q153936 in-violation Q215627
   ├─ Q153936 P279 Q43229  [axiom]
   └─ (Q153936 P279 Q215627) closure one-or-more  [closure]
```

`[axiom]` is a statement that stands in the dump, and it is the one to open on Wikidata. `[closure]` is the chain the engine walked; `%(culprit-path "Q153936" "Q215627")` prints it step by step.

And `in-violation` is an ordinary predicate now, so the next rule can read it – to raise a [contradiction](logic.md#contradiction-detection), to feed a further report, or to be negated in its own turn. A work list is a fact in the graph here, not the end of the pipeline.

One thing to keep in mind at scale: a negated path is tested once per candidate, so anchor the rule on something small, as the `P279` condition does above. Where the question is really the difference of two whole subtrees, the [SPARQL layer](sparql.md)'s `MINUS` computes both closures once and subtracts them.

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
