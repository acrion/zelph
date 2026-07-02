zelph includes a SPARQL query interface: you can query any zelph network — not just Wikidata imports — using a substantial subset of the standard SPARQL query language. This page describes the feature from a user's perspective: how to enable it, what is supported, and what to expect in terms of performance.

## Why an Import Is Required

The SPARQL support is not built into the zelph core. It is implemented entirely in [Janet](janet.md), zelph's embedded scripting language, in the script [sample_scripts/sparql.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/sparql.zph). The script uses Janet's Parsing Expression Grammars to parse SPARQL, translates the query into zelph's native query and closure primitives, and registers the `sparql` keyword in the REPL.

To enable SPARQL in a session, import the script once:

```zelph
.import sample_scripts/sparql.zph
```

This design is deliberate. The zelph core knows nothing about SPARQL — the entire query language is layered on top of the public Janet API (`zelph/query`, `zelph/closure`, `zelph/register-keyword`, and friends). This demonstrates how far zelph can be extended programmatically without touching the C++ engine, and it means you can read, modify, or extend the SPARQL implementation yourself: it is an ordinary script.

## Not Wikidata-Specific

Although the script contains convenience handling for Wikidata prefixes (`wd:`, `wdt:`, `p:`, `pq:`, and so on — see below), the SPARQL engine itself is generic. It queries whatever facts exist in the current zelph network, whether they came from a Wikidata dump, one of your own `.zph` scripts, or programmatic Janet code. Any IRI or prefixed name that is not a known Wikidata prefix is simply treated as a node name in the current language.

## Getting Started

Load a network, import the script, then type `sparql` and paste your query. An empty line executes it:

```
zelph> .load /path/to/wikidata-20260309-all-pruned.bin
zelph> .import sample_scripts/sparql.zph
"SPARQL subset loaded. Type 'sparql', paste your query, finish with an empty line."
zelph-> sparql
SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . }

```

Blank lines _inside_ a pasted query (for example between the `PREFIX` prologue and the `SELECT`) are harmless: the handler only executes once the query is structurally complete (a `SELECT` has been seen and all braces are balanced). Two consecutive blank lines force execution, which is useful to surface a syntax error in an incomplete paste.

Results are printed as tab-separated rows. For nodes that carry names, both the Wikidata ID and the English label are shown when available, e.g. `Q703534 (employee)`.

## Supported Subset

| Feature                         | Notes                                                                                             |
| ------------------------------- | ------------------------------------------------------------------------------------------------- |
| `SELECT` / `SELECT DISTINCT`    | Variable list or `*`                                                                              |
| Basic graph patterns            | Triple patterns, `;` property lists, `,` object lists                                             |
| Variables in predicate position | e.g. `wd:Q1 ?p ?o`                                                                                |
| Property paths                  | One-or-more `+`, zero-or-more `*`, and sequences with `/` (e.g. `wdt:P31/wdt:P279*`)              |
| `OPTIONAL`                      | Evaluated correlated with the outer pattern                                                       |
| `MINUS`                         | Evaluated correlated (i.e. `FILTER NOT EXISTS` semantics — identical for the typical query class) |
| `UNION`                         | Any number of branches                                                                            |
| `FILTER`                        | Single comparisons (`= != < > <= >=`) over variables, literals, numbers; `str()` and `lang()`     |
| Subqueries                      | Including nested subqueries                                                                       |
| `GROUP BY` + `COUNT`            | Also `COUNT(DISTINCT ?x)`                                                                         |
| `ORDER BY`                      | `ASC(?x)` / `DESC(?x)`                                                                            |
| `LIMIT`                         |                                                                                                   |
| `PREFIX`                        | Custom prefixes expand to full IRIs                                                               |

Not supported (rejected with an error): `BIND`, `VALUES`, `SERVICE`, `CONSTRUCT`, `DESCRIBE`, `GRAPH`, `HAVING`. A property path with `*` where **both** ends are unbound is also rejected, since it would trivially relate every node to itself.

A note on `lang()`: zelph models labels through its language system rather than as `rdfs:label` triples, so `lang()` always yields the empty string. The common label idiom `OPTIONAL { ?x rdfs:label ?l . FILTER (lang(?l) = "en") }` is accepted and the rows survive; labels appear in the output automatically via zelph's name system instead.

## Prefix Handling and Languages

The well-known Wikidata prefixes resolve directly to nodes in zelph's `wikidata` language, **independent of the current `.lang` setting** — you do not need to switch languages before querying:

| SPARQL prefix      | Resolves to                                                         |
| ------------------ | ------------------------------------------------------------------- |
| `wd:`, `wdt:`      | Bare IDs (`Q5`, `P279`) — the direct-triple layer                   |
| `p:`, `ps:`, `pq:` | Prefixed statement-layer names (`p:P2738`, `ps:P2738`, `pq:P11260`) |
| `wikibase:`        | e.g. `wikibase:rank`, `wikibase:DeprecatedRank`                     |

The statement-layer names exist in a network once qualifiers have been imported — see [Wikidata Qualifiers](qualifiers.md). Keeping the RDF prefix as part of the node name ensures that transitive closures like `wdt:P279+` can never leak into statement nodes.

`PREFIX` declarations in the query expand to full IRIs; the well-known Wikidata IRI prefixes (`http://www.wikidata.org/entity/` etc.) are then mapped the same way. Everything else is treated as a plain node name in the current language.

## Performance and the Adjacency Index

Transitive property paths (`+`, `*`) are evaluated by a native closure engine in the zelph core. The first time a closure runs over a given predicate, zelph builds an adjacency index over all relation nodes of that predicate and saves it next to the loaded `.bin` file as `<file>.bin.pidx.<id>` (where `<id>` is the internal node ID of the predicate):

```
Building adjacency index over 1193229 relation nodes (24 thread(s))...
Adjacency index ready: 1117131 edges.
Saved adjacency index to /path/to/file.bin.pidx.322 (1117131 edges).
```

This is a one-time cost per predicate and file. All subsequent queries that traverse the same predicate — in the same session or a later one — reuse the index and skip the build step entirely. On the full Wikidata network, the effect is substantial.

Beyond the index, the evaluator applies several optimizations transparently, most notably _closure intersection_: when several transitive steps constrain the same variable (the typical shape of disjointness queries), only the smallest closure is enumerated and the others act as membership filters.

Queries that are infeasible on public SPARQL endpoints due to timeouts can succeed here, because the entire network resides in local memory and closures are computed natively. See the [example session](#complete-example-session) below for a real disjointness-culprit query.

## Profiling

To see where time is spent, enable phase profiling:

```zelph
%(set sparql-profile true)
```

Every evaluation phase that takes longer than 1 ms then prints a line with its duration and row counts (`closure bwd Q215627`, `minus-join 18933x20628`, …). Disable with `%(set sparql-profile false)`.

## Complete Example Session

The following is an unedited session running one of the disjointness-culprit queries from [Doğan & Patel-Schneider (2024)](https://arxiv.org/abs/2410.13707) — find all classes that are subclasses of both _profession_ (Q215627) and _organization_ (Q43229), reporting only the topmost culprits. It was run on the **pruned** database so the result list stays reasonably short for this page; on the full database, the same query completes in a time comparable to the same query on [QLever](https://qlever.dev/wikidata). Note the adjacency index being built on the first run and reused on the second.

```
❯ zelph
zelph 0.9.6-dev
-- REPL mode - type .help for commands, .quit to exit --

zelph> .load /home/stefan/zelph/wikidata-20260309-all-pruned.bin
Auto-run has been disabled due to loading a large dataset.
Loading network from generic file /home/stefan/zelph/wikidata-20260309-all-pruned.bin...
Loading: left chunks=75, right chunks=75, nameOfNode chunks=21, nodeOfName chunks=21
...........................................................................
...........................................................................
.....................
.....................
String pool size after load: 20389119
Network loaded.
 Time needed for loading/importing: 0h0m53.815s
-- 53.816 s --
zelph-> .import sample_scripts/sparql.zph
Importing file sample_scripts/sparql.zph...
"SPARQL subset loaded. Type 'sparql', paste your query, finish with an empty line."
-- 23 ms --
zelph-> %(set sparql-profile true)
true
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

Building adjacency index over 1193229 relation nodes (24 thread(s))...
Adjacency index ready: 1117131 edges.
Saved adjacency index to /home/stefan/zelph/wikidata-20260309-all-pruned.bin.pidx.322 (1117131 edges).
[sparql   0.176s] closure bwd Q215627
[sparql   0.017s] closure bwd Q43229
[sparql   0.215s] closure-intersection ?class (2 steps)
[sparql   0.039s] bgp 1 triple(s) over 18933 sol(s)
[sparql   0.010s] path-step over 24559 sol(s)
[sparql   0.010s] path-step over 23868 sol(s)
[sparql   0.059s] minus inner (18933 seeds)
[sparql   0.011s] minus-join 18933x20628
[sparql   0.286s] total eval
?class
Q10870991 (host)
Q135778122
Q11812495
Q135778356 (continuing agency)
Q2289279 (point of contact)
Q137826327 (musical group, band, or musician)
Q1097498
Q15954537 (Battle buddy)
Q7933191 (armed non-state actor)
Q15080920 (reseller)
Q244655 (indigo children)
Q113546431 (grand niece-in-law)
Q24204895 (art director)
Q2818964 (various authors)
Q118289795 (Indian independent music producer)
Q1759246 (theatrical producer)
Q713223 (artist-in-residence)
Q108475169 (hologram artist)
Q12773225 (slave)
Q86348613 (camp police)
Q20987265 (single mother)
Q66372681 (single father)
Q201559 (privateer)
Q131468512 (ultras)
Q136431541 (illicit massage industry)
Q963251 (Muladi)
Q217731 (hajduk)
Q137795660 (criminal duo)
Q130412841
Q104772439 (nephew-in-law or niece-in-law)
Q16658574 (sibling-in-law)
Q703534 (employee)
Q5656781 (Haredi burqa sect)
Q110291311 (niece-in-law)
Q15622260 (Opryshoks)
Q111597957 (anti-transgender activist)
Q84062072
Q2533745
Q63433867 (court photographer)
Q1304271 (one-man band)
Q2146905 (revival)
Q17078348 (revivalist artist)
Q24937040 (Italian Service Units)
Q56576187
Q1074665 (mounted archery)
Q250895 (eagle warrior)
Q1029421 (prostitution of children)
Q110291310 (nephew-in-law)
Q2166675 (tulpa)
Q110292096 (first cousin in-law)
Q138001889 (uncle-in-law or aunt-in-law)
Q3809586 (Wikimedian in residence)
Q20746742 (adoptive parent)
Q12868609 (sazanda)
Q134793183 (vocal contractor)
Q138304223 (VTuber Group)
Q10658390 (rescue diver)
Q327591
Q113264255 (politician before the emergence of political parties)
Q543219 (pharmacy technician)
Q11974939 (health professional)
Q12885585 (Native American tribe)
Q115791109 (treaty society)
Q30185
Q113958153 (Vocera de Salinas Priego)
Q1266169 (telephone company)
Q5747846 (vivandi\u00e8re)
Q21531924 (ironmonger)
Q2061864 (caravan dweller)
Q133744 (tour operator)
Q135272969
Q2563546 (investment firm)
Q122294913 (map data distributor)
Q76003093 (Collective contract shopping)
Q2538889 (weapons manufacturing company)
Q2516794 (publisher's bookshop)
Q2731013 (infrastructure fund)
Q3030771 (book distributor)
Q1187145 (credit bureau)
Q56304483 (magazine and newspaper publisher)
Q11569980
-- 81 result(s) --
-- 286 ms --
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

[sparql   0.027s] closure bwd Q215627
[sparql   0.017s] closure bwd Q43229
[sparql   0.066s] closure-intersection ?class (2 steps)
[sparql   0.036s] bgp 1 triple(s) over 18933 sol(s)
[sparql   0.010s] path-step over 24559 sol(s)
[sparql   0.010s] path-step over 23868 sol(s)
[sparql   0.056s] minus inner (18933 seeds)
[sparql   0.010s] minus-join 18933x20628
[sparql   0.148s] total eval
?class
Q10870991 (host)
Q135778122
Q11812495
Q135778356 (continuing agency)
Q2289279 (point of contact)
Q137826327 (musical group, band, or musician)
Q1097498
Q15954537 (Battle buddy)
Q7933191 (armed non-state actor)
Q15080920 (reseller)
Q244655 (indigo children)
Q113546431 (grand niece-in-law)
Q24204895 (art director)
Q2818964 (various authors)
Q118289795 (Indian independent music producer)
Q1759246 (theatrical producer)
Q713223 (artist-in-residence)
Q108475169 (hologram artist)
Q12773225 (slave)
Q86348613 (camp police)
Q20987265 (single mother)
Q66372681 (single father)
Q201559 (privateer)
Q131468512 (ultras)
Q136431541 (illicit massage industry)
Q963251 (Muladi)
Q217731 (hajduk)
Q137795660 (criminal duo)
Q130412841
Q104772439 (nephew-in-law or niece-in-law)
Q16658574 (sibling-in-law)
Q703534 (employee)
Q5656781 (Haredi burqa sect)
Q110291311 (niece-in-law)
Q15622260 (Opryshoks)
Q111597957 (anti-transgender activist)
Q84062072
Q2533745
Q63433867 (court photographer)
Q1304271 (one-man band)
Q2146905 (revival)
Q17078348 (revivalist artist)
Q24937040 (Italian Service Units)
Q56576187
Q1074665 (mounted archery)
Q250895 (eagle warrior)
Q1029421 (prostitution of children)
Q110291310 (nephew-in-law)
Q2166675 (tulpa)
Q110292096 (first cousin in-law)
Q138001889 (uncle-in-law or aunt-in-law)
Q3809586 (Wikimedian in residence)
Q20746742 (adoptive parent)
Q12868609 (sazanda)
Q134793183 (vocal contractor)
Q138304223 (VTuber Group)
Q10658390 (rescue diver)
Q327591
Q113264255 (politician before the emergence of political parties)
Q543219 (pharmacy technician)
Q11974939 (health professional)
Q12885585 (Native American tribe)
Q115791109 (treaty society)
Q30185
Q113958153 (Vocera de Salinas Priego)
Q1266169 (telephone company)
Q5747846 (vivandi\u00e8re)
Q21531924 (ironmonger)
Q2061864 (caravan dweller)
Q133744 (tour operator)
Q135272969
Q2563546 (investment firm)
Q122294913 (map data distributor)
Q76003093 (Collective contract shopping)
Q2538889 (weapons manufacturing company)
Q2516794 (publisher's bookshop)
Q2731013 (infrastructure fund)
Q3030771 (book distributor)
Q1187145 (credit bureau)
Q56304483 (magazine and newspaper publisher)
Q11569980
-- 81 result(s) --
-- 148 ms --
zelph-> .quit

❯ stat /home/stefan/zelph/wikidata-20260309-all-pruned.bin.pidx.322
  File: /home/stefan/zelph/wikidata-20260309-all-pruned.bin.pidx.322
  Size: 17874144  	Blocks: 10961      IO Block: 131072 regular file
Device: 0,51	Inode: 1479309     Links: 1
Access: (0644/-rw-r--r--)  Uid: ( 1001/  stefan)   Gid: ( 1001/  stefan)
Access: 2026-07-02 23:47:31.446379174 +0200
Modify: 2026-07-02 23:47:31.449712597 +0200
Change: 2026-07-02 23:47:31.449712597 +0200
 Birth: 2026-07-02 23:47:31.446379174 +0200

❯
```
