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

To download the compressed JSON file, browse to [https://dumps.wikimedia.org/wikidatawiki/entities/](https://dumps.wikimedia.org/wikidatawiki/entities/). You may need to search the subdirectories to find the download link for `wikidata-*-all.json.bz2`. Note that the mirror [https://dumps.wikimedia.your.org/wikidatawiki/entities/](https://dumps.wikimedia.your.org/wikidatawiki/entities/) is often faster.

After decompression, you can start zelph with the provided `wikidata.zph` script:

```bash
zelph sample_scripts/wikidata.zph
```

### Basic Import

To import a Wikidata JSON dump, use the `.load` command:

```zelph
.load download/wikidata-20250127-all.json
```

This imports the data and automatically creates a `.bin` cache file in the same directory for faster future loads.

With the same command, you can load a `.bin` file directly

### Advanced Commands

zelph provides several additional commands for working with Wikidata:

- **Export constraints:** extract constraints from the dump and generate zelph scripts for them:

  ```zelph
  .wikidata-constraints download/wikidata-20250127-all.json constraints_output_dir
  ```

Please note that after executing a '.load' command, '.auto-run' is disabled. This means that any rules added will only be applied when inference is performed explicitly via the `.run`, `.run-once`, `.run-md` or `.run-file` commands (see the Performing Inference section above).
