# zelph and Wikidata: Finding Logical Connections and Contradictions

## Wikidata as an Ideal Use Case for zelph

Wikidata is an excellent use case for zelph.
It contains over 113 million entries interconnected by relations, all subject to logical constraints.
This complex web of knowledge presents two key opportunities for zelph:

1. **Finding contradictions**: identifying logical inconsistencies in the data
2. **Making deductions**: deriving new facts through logical inference

For example, if class `A` is the opposite of class `B` (such as [successor](https://www.wikidata.org/wiki/Q106110771) and [predecessor](https://www.wikidata.org/wiki/Q106110777)), then no entity `X` can belong to both classes (such as [replacing entity](https://www.wikidata.org/wiki/Q45025415)).

Similarly, inferences can be made. For example, if X is related to Y and Y is related to Z through the same relation (e.g. X = [Canada](https://www.wikidata.org/wiki/Q16), Y = [American continent](https://www.wikidata.org/wiki/Q828), Z = [Earth's surface](https://www.wikidata.org/wiki/Q1349417), relation = [is part of](https://www.wikidata.org/wiki/Property:P361)), and the relation is [transitive](https://www.wikidata.org/wiki/Q64861), then X must also be related to Z in the same way.

### Architectural Synergy with Wikidata

zelph’s architecture, which treats relations as first-class nodes, aligns very well with Wikidata’s data model.
In Wikidata, properties (P-entities) are not merely labels on edges but are themselves entities with their own attributes, constraints, and relationships to other entities.
This fundamental similarity enables zelph to:

1. **Naturally represent Wikidata’s property hierarchy**: properties in Wikidata can have subproperties, domains, ranges, and other metadata - all of which are directly representable in zelph’s relation-as-node approach.

2. **Reason about properties themselves**: zelph can apply inference rules to properties just as it does to regular entities, enabling powerful meta-reasoning capabilities essential for working with Wikidata’s complex property structure.

3. **Enforce property constraints**: Wikidata’s property constraints (symmetry, transitivity, inverse relationships) map directly to zelph’s rule system, allowing automatic validation and inference.

This structural compatibility makes zelph well suited to analysing and enriching Wikidata’s knowledge graph while maintaining its semantic integrity.

Since version 0.9.6, this extends to Wikidata's **statement layer**: qualifiers can be imported as reified statement structures and queried natively or via [SPARQL](sparql.md) — see [Wikidata Qualifiers](qualifiers.md).

If you came here for the class hierarchy specifically — disjointness violations, which `P279` statement to change, and doing it without a 210 GB machine — start at [Working on the Wikidata Class Hierarchy](class-hierarchy.md).

## Technical Implementation

### Memory Efficiency

Wikidata is large: the compressed JSON dump `wikidata-20260309-all.json.bz2` is about 100 GB in size, and the fully serialised zelph network is about 88 GB on disk.

zelph is capable of importing the **entire** Wikidata graph into memory, which enables non-iterative, whole-graph contradiction detection. After processing, the complete semantic network can be serialised to disk in a compact binary format for much faster future loading.

Loading the full graph for active reasoning still requires substantial memory. In practice, **256 GB of RAM is recommended** for smooth full-scale work with the complete Wikidata dump. Systems with **128 GB of RAM** may still process the graph by relying heavily on swap and ZRAM, but performance can degrade significantly.

The exact memory requirement depends on the Wikidata dump, the enabled rules, and the type of processing being performed.

### Processing Performance

Running inference on Wikidata data is computationally intensive but highly optimised:

- **Parallel processing:** both data import and the unification/reasoning engine are multi-threaded and can utilise all available CPU cores.
- **Performance:** a complete inference pass on the full dataset takes approximately 2.5 hours on high-end hardware (for example, an Intel Core i9 with 24 cores), although this depends strongly on available RAM and on the specific rules being applied.
- **Workflow:** users can run targeted scripts to find specific classes of contradictions (see additional Wikidata sections on [zelph.org](https://zelph.org/wikidata) for examples such as split-order violations).

## Wikidata Integration Script

The following script demonstrates how zelph connects with Wikidata data:

```zelph
.lang zelph

.name !                wikidata Q363948
.name ~                wikidata P31
.name "is subclass of" wikidata P279
.name "is facet of"    wikidata P1269
.name =>               wikidata Q374182
.name ->               wikidata Q130901
.name "is part of"     wikidata P361
.name "has part"       wikidata P527
.name "is opposite of" wikidata P461
.name "is inverse of"  wikidata P1696
.name "has quality"    wikidata P1552
.name "is for example" wikidata Q21514624
.name "transitive relation" wikidata Q18647515

# The following facts are part of wikidata:
#"is subclass of" ~ transitive relation
#"has part"       ~ transitive relation
#"is facet of"    ~ transitive relation
#"is part of"     ~ transitive relation
#"is part of"     is inverse of "has part"

# The following facts are not part of wikidata:
"has quality" ~ transitive relation

(X "is facet of" Y, Y ~ C) => (X ~ C)
(X "is facet of" Y, Y "is subclass of" C) => (X "is subclass of" C)
(X "is facet of" Y, Y "has part" P) => (X "has part" P)
(X "is facet of" Y, Y "is part of" P) => (X "is part of" P)
(X "is facet of" Y, Y "has quality" Q) => (X "has quality" Q)

# The following fact is not part of wikidata. Wikidata only includes the fact "is subclass of" "subject item of this property" "is for example"
"is for example"  is inverse of "~"

(R ~ "transitive relation", X R Y, Y R Z) => (X R Z)
(P ~ "transitive relation", P "is inverse of" Q) => (Q ~ "transitive relation")
(X ~ K, K "is subclass of" U) => (X ~ U)

(X "has quality" E, E ~ K) => (X "has quality" K)
(X "has quality" E, E "is subclass of" K) => (X "has quality" K)
(K "has quality" E, X ~ K) => (X "has quality" E)
(K "has quality" E, X "is subclass of" K) => (X "has quality" E)
(X "has part" P, P ~ K) => (X "has part" K)
(K "has part" P, X "is subclass of" K) => (X "has part" P)

(X "is opposite of" Y, X ~ K) => (Y ~ K)
(X "is opposite of" Y, X "is subclass of" K) => (Y "is subclass of" K)
(X "is inverse of" Y, X ~ K) => (Y ~ K)
(X "is inverse of" Y, X "is subclass of" K) => (Y "is subclass of" K)

# Single rules (no conjunction needed for 1 condition)
(X "is opposite of" Y) => (Y "is opposite of" X)
(X "is inverse of" Y)  => (Y "is inverse of" X)
(R "is opposite of" S, X R Y) => (Y S X)
(R "is inverse of" S, X R Y) => (Y S X)

(X "is opposite of" Y, A "has quality" X, A "has quality" Y) => !
(X "is inverse of" Y, A "has quality" X, A "has quality" Y) => !
(X "is opposite of" Y, A "has part" X, A "has part" Y) => !
(X "is inverse of" Y, A "has part" X, A "has part" Y) => !

(X "is opposite of" Y, A ~ X, A ~ Y) => !
(X "is opposite of" Y, A "is subclass of" X, A "is subclass of" Y) => !
(X "is inverse of" Y, A ~ X, A ~ Y) => !
(X "is inverse of" Y, A "is subclass of" X, A "is subclass of" Y) => !

(X "has quality" E, X ~ E) => !
(X "has quality" E, X "is subclass of" E) => !
(X "has quality" E, E ~ X) => !
(X "has quality" E, E "is subclass of" X) => !
(X "has quality" E, E "has part" X) => !

(X "has part" E, X ~ E) => !
(X "has part" E, X "is subclass of" E) => !
(X "has part" E, E ~ X) => !
(X "has part" E, E "is subclass of" X) => !

# The following contradiction requires that X cannot at the same time be both an instance and a subclass:
(X ~ A, X "is subclass of" B) => !

(A ~ B, B ~ A) => !
(A "is subclass of" B, B "is subclass of" A) => !
(A "is facet of" B, B "is facet of" A) => !
(A ~ B, B "is subclass of" A) => !
(A ~ B, B "is facet of" A) => !
(A "is subclass of" B, B "is facet of" A) => !
```

This script maps zelph’s relation types to Wikidata properties and items, defines inference rules, and establishes contradiction checks.

## Understanding the Script

### Relation Mapping

The script begins by mapping zelph’s internal names to Wikidata entities:

- `~` is mapped to Wikidata’s [instance of (P31)](https://www.wikidata.org/wiki/Property:P31)
- `is subclass of` is mapped to [subclass of (P279)](https://www.wikidata.org/wiki/Property:P279)
- `is facet of` is mapped to [facet of (P1269)](https://www.wikidata.org/wiki/Property:P1269)

This mapping ensures that zelph can interpret Wikidata’s relational structure correctly.

### Handling "is a" Relations

Wikidata makes a granular distinction between different types of category relations:

1. [instance of (P31)](https://www.wikidata.org/wiki/Property:P31)
2. [subclass of (P279)](https://www.wikidata.org/wiki/Property:P279)
3. [facet of (P1269)](https://www.wikidata.org/wiki/Property:P1269)

zelph’s flexible design accommodates these distinctions.
The idea behind the script is to follow the [Wikidata usage guidelines](https://www.wikidata.org/wiki/Property:P2559).
It can easily be adapted or extended further.

Notably, Wikidata marks `subclass of` as transitive, but not the other two relations.
This makes sense for `instance of` (since an instance is not a class), but the script adds rules for `facet of` that reflect its documented meaning:
if X is a `facet of` Y, then X inherits relevant properties of Y.

For this case, the following rules are included in the script:

- If `Y` is an [instance of](https://www.wikidata.org/wiki/Property:P31) `C`, then `X` must also be an [instance of](https://www.wikidata.org/wiki/Property:P31) `C`.
- If `Y` is a [subclass of](https://www.wikidata.org/wiki/Property:P279) `C`, then `X` must also be a [subclass of](https://www.wikidata.org/wiki/Property:P279) `C`.
- If `Y` [has part](https://www.wikidata.org/wiki/Property:P527) `P`, then `X` must also [have part](https://www.wikidata.org/wiki/Property:P527) `P`.
- If `Y` is [part of](https://www.wikidata.org/wiki/Property:P361) `P`, then `X` must also be [part of](https://www.wikidata.org/wiki/Property:P361) `P`.
- If `Y` has a [characteristic](https://www.wikidata.org/wiki/Property:P1552) `Q`, then `X` must also have a [characteristic](https://www.wikidata.org/wiki/Property:P1552) `Q`.

### Example Inference Process

Here is a step-by-step example of zelph’s inference process when working with Wikidata:

1. According to Wikidata, the property [greater than (P5135)](https://www.wikidata.org/wiki/Property:P5135) is an instance of [transitive Wikidata property (Q18647515)](https://www.wikidata.org/wiki/Q18647515).
2. Wikidata also states that [transitive Wikidata property (Q18647515)](https://www.wikidata.org/wiki/Q18647515) is a [facet of (P1269)](https://www.wikidata.org/wiki/Property:P1269) [transitive relation (Q64861)](https://www.wikidata.org/wiki/Q64861).
3. The script contains the rule: `(X "is facet of" Y, Y ~ C) => (X ~ C)`
4. Therefore, zelph infers that [greater than (P5135)](https://www.wikidata.org/wiki/Property:P5135) is also an instance of [transitive relation (Q64861)](https://www.wikidata.org/wiki/Q64861).

## Rules in the Semantic Network

Rules in zelph are encoded in the same semantic network as facts, using the special relation `=>` (which corresponds to [logical consequence (Q374182)](https://www.wikidata.org/wiki/Q374182) in Wikidata).

This approach enables tight integration between the fact base and the rules, allowing rules to be reasoned about in the same way as facts.
This makes zelph particularly powerful for applications such as Wikidata, where the knowledge base itself contains statements about relations, including properties such as [transitivity](https://www.wikidata.org/wiki/Q18647515).

A rule is simply a special case of a fact that uses the relation `=>`. In the application of zelph to Wikidata data, this relation corresponds to [logical consequence](https://www.wikidata.org/wiki/Q374182).

## Loading and Processing Wikidata

You may either download a zelph specific bin file from [Hugging Face](https://huggingface.co/datasets/acrion/zelph), or a compressed JSON file from [https://dumps.wikimedia.org/wikidatawiki/entities/](https://dumps.wikimedia.org/wikidatawiki/entities/). You may need to search the subdirectories to find the download link for `wikidata-*-all.json.bz2`. Note that the mirror [https://dumps.wikimedia.your.org/wikidatawiki/entities/](https://dumps.wikimedia.your.org/wikidatawiki/entities/) is often faster.

Please note that the automatic conversion from the `.json.bz2` format to zelph's general-purpose `.bin` format is computationally expensive.

To import a Wikidata JSON dump or a zelph bin file, use the `.load` command:

```zelph
.load download/wikidata-20250127-all.json.bz2
```

or directly the resuling bin file:

```zelph
.load download/wikidata-20250127-all.bin
```

You can download various zelph `.bin` files directly from [Hugging Face](https://huggingface.co/datasets/acrion/zelph).

Each imported entity is named twice: by its ID (`Q5`, `P279`) in the `wikidata`
language and by its English label in the `en` language. The dump writes labels
with JSON escapes (`B\u00fcdner`), which the import decodes, so the node is
named `Büdner` and can be found under that name. A `.bin` file written by a
zelph older than 1.0.0 stores the undecoded form instead; re-import the dump if
you need the labels themselves rather than only the IDs.

### Advanced Commands

zelph provides several additional commands for working with Wikidata:

- **Import qualifiers:** add Wikidata's statement/qualifier layer to an already loaded network, enabling qualifier-based queries and constraint checking (see [Wikidata Qualifiers](qualifiers.md)):

  ```zelph
  .wikidata-qualifiers download/wikidata-20250127-all.json P11260
  ```

- **Export constraints:** turn Wikidata's own property constraints into runnable zelph rules — see [Checking Wikidata's own constraints](#checking-wikidatas-own-constraints) below.

- **Extract single entities:** pull the exact JSON lines of named entities out of a dump, without importing anything:

  ```zelph
  .export-wikidata download/wikidata-20250127-all.json Q42 Q5
  ```

  Each line is written to `<id>.json` in the current directory, byte for byte as the dump held it — the usual way to get a realistic fixture to develop against. IDs are matched in full, so asking for `Q4` does not also give you `Q42`. Any ID that turns out not to be in the dump is listed at the end (`Not found in …: Q9999`); the scan itself reads the whole file, so on a full dump this takes as long as any other pass over it.

### Checking Wikidata's own constraints

Wikidata states most of its quality rules as data: the `property constraint`
([P2302](https://www.wikidata.org/wiki/Property:P2302)) statements that sit on
almost every property. `.wikidata-constraints` reads them out of a dump and
writes them out as zelph rules:

```zelph
.wikidata-constraints download/wikidata-20250127-all.json constraints_output_dir
```

What that buys you is a check you run **over the whole graph at once**, offline,
against a pinned dump — and one that hands back, for every item it flags, the
statements that made it flag. The result is a work-list you can act on, not a
count.

You get one `<property>.zph` per property from which at least one rule could be
derived. A property whose constraints are all of a type zelph cannot express
yet produces no file at all, so the output directory is the work-list itself
rather than a copy of the dump.

Two of Wikidata's constraint types are translated today:

| Constraint | Rule written |
| --- | --- |
| [conflicts-with](https://www.wikidata.org/wiki/Q21502838) (Q21502838) | `(I <prop> Y, I <other> <value>) => !` — one rule per forbidden value, or `(I <prop> Y, I <other> Z) => !` against the mere presence of the other property when the constraint names no value |
| [none-of](https://www.wikidata.org/wiki/Q52558054) (Q52558054) | `(I <prop> <value>) => !` — one rule per forbidden value |

`I`, `Y` and `Z` are variables: `I` is the item under test, `Y` and `Z` stand
for whatever values it happens to carry. `=> !` marks the combination as a
[contradiction](index.md#rules-and-inference) — importing such a rule asserts nothing, it
makes the violations of that constraint reportable.

So the whole check is three steps: load a dump, import the script, infer.

```zelph
.load download/wikidata-20250127-all.bin
.import constraints_output_dir/P569.zph
.run
```

Each violation is reported with the statements that produced it. Typed out
small, so you can see the shape — this is a real transcript of a rule of the
kind the exporter writes, for a constraint saying that
[date of birth](https://www.wikidata.org/wiki/Property:P569) conflicts with
[inception](https://www.wikidata.org/wiki/Property:P571):

```
zelph> .lang wikidata
wikidata> (I P569 Y, I P571 Z) => !
{(I P569 Y) (I P571 Z)} => !
wikidata> Q42 P569 Q1900
wikidata> Q42 P571 Q1900
! ⇐ {(Q42 P569 Q1900) (Q42 P571 Q1900)}
Found one or more contradictions!
```

The line after `⇐` is the justification: exactly the two statements on Q42 that
together break the constraint, which is what you need in order to decide which
of them is the mistake. (`.explain` does not apply here — a contradiction
materialises no fact to explain, so it carries its premises with it instead.)
For a large run, [`.run-export <file>`](index.md#exporting-derivations) writes every derivation
and every contradiction to a JSON Lines file, one record per line, with
`"kind":"contradiction"` and the premises.

Everything else Wikidata's constraint system defines — range, format,
single-value, subject type and some forty more types — is not translated yet.
Where a file is written, those constraints still appear in it as the raw JSON
of their P2302 statement, commented out and marked
`# (no existing zelph rule generator for this constraint type)`, which is the
material you need to write the rule yourself. A constraint whose qualifiers
carry no usable value says so (`# No P2306 (conflict property) found`) rather
than guessing at one. Which of the remaining types are worth generating next is
an [open roadmap question](index.md#where-the-logic-goes-next).

Please note that after executing a '.load' command, '.auto-run' is disabled. This means that any rules added will only be applied when inference is performed explicitly via the `.run`, `.run-once`, `.run-delta` or `.run-export` commands (see the Performing Inference section above).
