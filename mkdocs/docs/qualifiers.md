Wikidata statements can carry **qualifiers** — additional property-value pairs that refine a statement, such as _start time_ (P580) on a _position held_ (P39) statement, or _list item_ (P11260) on a _disjoint union of_ (P2738) statement. Qualifiers are essential for two things zelph cares about: many [property constraints](https://www.wikidata.org/wiki/Help:Property_constraints_portal) are defined via qualifiers, and structural definitions such as class disjointness live entirely in qualifiers.

The standard zelph Wikidata import (`.load`) deliberately imports only direct triples (entity–property–entity) for memory efficiency. Since version 0.9.6, the command `.wikidata-qualifiers` adds the statement layer on top of an already loaded network.

## Data Model

zelph does not introduce any special metadata for qualifiers. Statements are _reified_: each qualified statement becomes an ordinary node, connected by ordinary facts, all in the `wikidata` language — mirroring the RDF statement layer that SPARQL users know from the Wikidata Query Service:

```
<subject>   p:<P>          <statement>    # links the entity to its statement node
<statement> ps:<P>         <main value>   # the statement's main value
<statement> pq:<Pq>        <value>        # one fact per qualifier
<statement> wikibase:rank  wikibase:{Normal|Preferred|Deprecated}Rank
```

Statement nodes are named by their Wikidata statement ID (which always contains `$`, so it can never collide with Q/P IDs). The RDF prefix is kept as part of the predicate node name (`p:P2738`, `pq:P11260`) so that the direct-triple predicates (bare `P279` etc.) and the statement layer never share a node — otherwise transitive closures like `wdt:P279+` would leak into statement nodes.

Because statement nodes are ordinary named nodes, everything that works on zelph networks works on them too: native queries, rules and inference, [SPARQL](sparql.md), and [partial loading / sharding](sharding.md).

Value handling: entity values attach to the existing Q/P nodes via their `wikidata` names; time, quantity, string, and monolingual text values become nodes named by their raw value (e.g. `+2020-01-01T00:00:00Z`, `+42`). `novalue`/`somevalue` snaks and coordinates are skipped.

## The `.wikidata-qualifiers` Command

```zelph
.wikidata-qualifiers <wikidata-dump.json[.bz2]> [P-id1 P-id2 ...]
```

Without property IDs, **all** qualifiers are imported. With property IDs, only qualifiers whose property is listed are imported, and a statement is only materialized if it contributes at least one matching qualifier — this keeps selective imports small.

The intended workflow is:

```zelph
.load wikidata-20260309-all.bin                            # 1. load the base network
.wikidata-qualifiers wikidata-20260309-all.json.bz2 P11260 # 2. add the statement layer
.save wikidata-20260309-all-P11260.bin                     # 3. persist the combined network
```

Loading the base network first is important: it ensures that subjects and entity values attach to the existing nodes via their `wikidata` names instead of creating disconnected duplicates.

The import is **idempotent and incremental**: facts are content-addressed, so re-running the command — for example with additional qualifier properties on a network that already contains qualifiers — simply extends the loaded network.

Be aware of scale: a selective import like `P11260` adds only a few thousand facts, but a _full_ qualifier import materializes hundreds of millions of statement nodes and grows memory usage substantially beyond the base network's requirements. For most use cases, selective per-purpose databases are the better choice.

## Prebuilt Databases

You do not need to run the import yourself. Qualifier-extended `.bin` files are published alongside the regular ones on [Hugging Face](https://huggingface.co/datasets/acrion/zelph) — currently `wikidata-20260309-all-P11260.bin`, which contains the full network plus the _list item_ qualifiers needed for disjointness analysis (see below).

If you need a database with a different set of qualifiers for your work, open an issue on [GitHub](https://github.com/acrion/zelph/issues) — I am happy to run the import and publish additional variants on request.

## Use Case: Disjointness Violations

The motivating use case comes from [_Disjointness Violations in Wikidata_ (Doğan & Patel-Schneider, 2024)](https://arxiv.org/abs/2410.13707). Wikidata expresses class disjointness through _disjoint union of_ (P2738) statements whose members are listed in _list item_ (P11260) **qualifiers** — so finding disjointness violations requires reading qualifiers _and_ traversing the subclass hierarchy transitively. The paper identified over 14,000 "culprit" classes this way; several of the required queries time out on public SPARQL endpoints.

With the qualifier-extended database and zelph's [SPARQL support](sparql.md), the paper's central query runs verbatim:

```sparql
SELECT DISTINCT ?i ?class ?disj1 ?disj2 WHERE {
  ?class p:P2738 ?l .
  MINUS { ?l wikibase:rank wikibase:DeprecatedRank . }
  ?l pq:P11260 ?disj1 . ?l pq:P11260 ?disj2 .
  FILTER ( ( str(?disj1) < str(?disj2) ) )
  ?i wdt:P279* ?disj1 . ?i wdt:P279* ?disj2 .
}
```

This reads the disjointness _definitions_ dynamically from the graph (no external CSV files), filters deprecated statements by rank, and finds every class that violates any of the resulting pairwise disjointnesses — end to end, in one query, on your own machine.

## Outlook: Property Constraints

Most standard Wikidata property constraints are defined via qualifiers on P2302 statements of the property entities (e.g. P2305, P2306, P5314). The qualifier import processes property entities as well, so importing these qualifier properties turns constraint definitions into queryable graph structure — the foundation for qualifier-dependent constraint checking with zelph rules. This is an active development direction.
