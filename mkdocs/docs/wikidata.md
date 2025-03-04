# zelph and Wikidata: Finding Logical Connections and Contradictions

## Wikidata as an Ideal Use Case for zelph

Wikidata represents an excellent application case for zelph’s capabilities.
It contains over 113 million entries interconnected by relations, all subject to logical constraints.
This complex web of knowledge presents two key opportunities for zelph:

1. **Finding contradictions**: Identifying logical inconsistencies in the data
2. **Making deductions**: Deriving new facts through logical inference

For example, if class `A` is the opposite of class `B` (such as [successor](tree/Q106110771.md) and [predecessor](tree/Q106110777.md)), then no entity `X` can belong to both classes (like [replacing entity](tree/Q45025415.md)).

Similarly, inferences can be made. Example: If X is related to Y and Y is related to Z through the same relation (e.g., X=[Canada](tree/Q16.md), Y=[American continent](tree/Q828.md), Z=[Earth's surface](tree/Q1349417.md), relation=[is part of](tree/P361.md)), and the relation is [transitive](tree/Q64861.md), then X must also be related to Z in the same way.

### Architectural Synergy with Wikidata

zelph’s architecture of treating relations as first-class nodes creates a perfect alignment with Wikidata’s data model.
In Wikidata, properties (P-entities) are not merely labels on edges but are themselves entities with their own attributes, constraints, and relationships to other entities.
This fundamental similarity enables zelph to:

1. **Naturally represent Wikidata’s property hierarchy**: Properties in Wikidata can have subproperties, domains, ranges, and other metadata - all of which are directly representable in zelph’s relation-as-node approach.

2. **Reason about properties themselves**: zelph can apply inference rules to properties just as it does to regular entities, enabling powerful meta-reasoning capabilities essential for working with Wikidata’s complex property structure.

3. **Enforce property constraints**: Wikidata’s property constraints (symmetry, transitivity, inverse relationships) map directly to zelph’s rule system, allowing automatic validation and inference.

This structural compatibility makes zelph well-suited for analyzing and enriching Wikidata’s knowledge graph while maintaining its semantic integrity.

### Navigating the Generated Knowledge Tree

zelph has processed Wikidata’s vast knowledge graph and generated a [tree of 4,580 pages](tree-explanation.md) representing entities and properties where meaningful deductions or contradictions were found.
This represents the practical application of zelph’s semantic inference capabilities to real-world data.

The navigation menu provides entry points to explore various branches of this knowledge tree, organized by knowledge domains and semantic relation types.
Particularly noteworthy is the [Logical Contradictions](tree/Q363948.md) page, which highlights inconsistencies in Wikidata’s knowledge structure that could be valuable for Wikidata curators.

For a detailed explanation of how this tree was generated and what it represents, see the [Tree Explanation](tree-explanation.md) page.

## Technical Implementation

### Memory Efficiency

The complete Wikidata in JSON format is approximately 1.4 TB, containing over 113 million entries.
After processing by zelph, the data occupies only 9.2 GB in memory.
This reduction is achieved through two factors: a [bit-optimized data structure](https://github.com/acrion/zelph/blob/main/src/lib/network.hpp) and selective data inclusion.

On average, zelph requires only 80 bytes per Wikidata item, which includes:

1. All relations to other items (i.e., connections via Wikidata properties, which define the relationships between items in the Wikidata structure)
2. The English names of the items
3. The Wikidata IDs of the items

In a semantic network like zelph, properties represent the labels that appear above the connection arrows between two items.
While Wikidata refers to these as "properties," zelph uses the term "relation names" for the same concept.

Items in zelph can be assigned names in any number of languages, with Wikidata IDs handled as a specific language ("wikidata").
There is also a language "zelph" that is used by default in zelph scripts, but this is configurable (via the `.lang` command).

### Processing Performance

Running the inference process on Wikidata data requires significant computational resources:

- A complete inference pass takes approximately 2.5 hours on an Intel Core i9-12900T
- Processing time varies depending on the number of applicable rules
- The current implementation does not yet utilize multi-threading
- Additional inference passes can be run using the `.run` or `.run-md` commands to discover more facts and contradictions

## Wikidata Integration Script

The following script demonstrates how zelph connects with Wikidata data:

```
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
.name "transitive relation" wikidata Q64861

# The following facts are part of wikidata:
#"is subclass of" ~ transitive relation
#"has part"       ~ transitive relation
#"is part of"     is inverse of "has part"

# The following facts are not part of wikidata:
"has quality" ~ transitive relation
"is facet of" ~ transitive relation

X is facet of Y, Y ~ C => X ~ C
X is facet of Y, Y is subclass of C => X is subclass of C
X is facet of Y, Y has part P => X has part P
X is facet of Y, Y is part of P => X is part of P
X is facet of Y, Y has quality Q => X has quality Q

# The following fact is not part of wikidata. Wikidata only includes the fact "is subclass of" "subject item of this property" "is for example"
"is for example"  is inverse of "~"

R ~ transitive relation, X R Y, Y R Z => X R Z
P ~ transitive relation, P is inverse of Q => Q ~ transitive relation
X ~ K, K is subclass of U => X ~ U

X has quality E,   E ~ K                => X has quality K
X has quality E,   E is subclass of K   => X has quality K
K has quality E,   X ~ K                => X has quality E
K has quality E,   X is subclass of K   => X has quality E
X has part P,      P ~ K                => X has part K
K has part P,      X is subclass of K   => X has part P

X is opposite of Y, X ~ K               => Y ~ K
X is opposite of Y, X is subclass of K  => Y is subclass of K
X is inverse of Y,  X ~ K               => Y ~ K
X is inverse of Y,  X is subclass of K  => Y is subclass of K

X is opposite of Y        => Y is opposite of X
X is inverse of Y         => Y is inverse of X
R is opposite of S, X R Y => Y S X
R is inverse of S,  X R Y => Y S X

X is opposite of Y, A has quality X, A has quality Y => !
X is inverse of Y,  A has quality X, A has quality Y => !
X is opposite of Y, A has part X,    A has part Y    => !
X is inverse of Y,  A has part X,    A has part Y    => !

X is opposite of Y, A ~ X,              A ~ Y              => !
X is opposite of Y, A is subclass of X, A is subclass of Y => !
X is inverse of Y,  A ~ X,              A ~ Y              => !
X is inverse of Y,  A is subclass of X, A is subclass of Y => !

X has quality E, X ~ E              => !
X has quality E, X is subclass of E => !
X has quality E, E ~ X              => !
X has quality E, E is subclass of X => !
X has quality E, E has part X       => !

X has part E, X ~ E              => !
X has part E, X is subclass of E => !
X has part E, E ~ X              => !
X has part E, E is subclass of X => !

# The following contradiction requires that X cannot be at the same time an instance and a subclass:
X ~ A, X is subclass of B => !

A ~ B, B ~ A                           => !
A is subclass of B, B is subclass of A => !
A is facet of B, B is facet of A       => !
A ~ B, B is subclass of A              => !
A ~ B, B is facet of A                 => !
A is subclass of B, B is facet of A    => !
```

This script maps zelph’s relation types to Wikidata properties and items, defines inference rules, and establishes contradiction checks.

## Understanding the Script

### Relation Mapping

The script begins by mapping zelph’s internal names to Wikidata entities:

- `~` is mapped to Wikidata’s [instance of (P31)](https://www.wikidata.org/wiki/Property:P31)
- `is subclass of` is mapped to [subclass of (P279)](https://www.wikidata.org/wiki/Property:P279)
- `is facet of` is mapped to [facet of (P1269)](https://www.wikidata.org/wiki/Property:P1269)

This careful mapping ensures that zelph can interpret Wikidata’s relational structure correctly.

### Handling "is a" Relations

Wikidata makes a granular distinction between different types of category relations:

1. [instance of (P31)](tree/P31.md)
2. [subclass of (P279)](tree/P279.md)
3. [facet of (P1269)](tree/P1269.md)

zelph’s flexible design accommodates these distinctions.
The idea of the script is to follow the [Wikidata usage guidelines](https://www.wikidata.org/wiki/Property:P2559).
It can be easily adapted or extended for further improvements.

Notably, Wikidata only marks "subclass of" as transitive, not the other two relations.
This makes sense for "instance of" (since an instance is not a class), but the script adds transitivity for "facet of" along with additional rules that reflect its documented meaning:
if X is a "facet of" Y, then X inherits all properties of Y.

For this case, the following rules are included in the script:

- If `Y` is an [instance of](tree/P31.md) `C`, then `X` must also be an [instance of](tree/P31.md) `C`.
- If `Y` is a [subclass of](tree/P279.md) `C`, then `X` must also be a [subclass of](tree/P279.md) `C`.
- If `Y` [has part](tree/P527.md) `P`, then `X` must also [have part](tree/P527.md) `P`.
- If `Y` is [part of](tree/P361.md) `P`, then `X` must also be [part of](tree/P361.md) `P`.
- If `Y` has a [characteristic](tree/P1552.md) `Q`, then `X` must also have a [characteristic](tree/P1552.md) `Q`.

### Example Inference Process

Here’s a step-by-step example of zelph’s inference process when working with Wikidata:

1. According to Wikidata, the property [greater than (P5135)](tree/P5135.md) is an instance of [transitive Wikidata property (Q18647515)](tree/Q18647515.md).
2. Wikidata also states that [transitive Wikidata property (Q18647515)](tree/Q18647515.md) is a [facet of (P1269)](tree/P1269.md) [transitive relation (Q64861)](tree/Q64861.md).
3. The script contains the rule: `X is facet of Y, Y ~ C => X ~ C`
4. Therefore, zelph infers that [greater than (P5135)](tree/P5135.md) is also an instance of [transitive relation (Q64861)](tree/Q64861.md).

## Rules in the Semantic Network

Rules in zelph are encoded in the same semantic network as facts, using the special relation `=>` (which corresponds to [logical consequence (Q374182)](https://www.wikidata.org/wiki/Q374182) in Wikidata).

This innovative approach enables tight integration between the fact base and the rules, allowing rules to be reasoned about in the same way as facts.
This makes zelph particularly powerful for applications like Wikidata, where the knowledge base itself contains statements about relations, including properties like [transitivity](tree/Q18647515.md).

A rule is just a special case of a fact that uses the relation `=>`. In the case of the application of zelph to Wikidata data, this relation corresponds to [logical consequence](tree/Q374182.md).

## Publishing Results

The results of zelph’s analysis are published as a tree of pages, where each page corresponds to a Wikidata item. Pages can contain two sections:

1. **Deductions**: New facts derived about the item through logical inference
2. **Contradictions**: Logical inconsistencies involving the item

Example of a deduction:

- [natural physical object](tree/Q16686022.md) [is for example](tree/Q21514624.md) [Universe](tree/Q1.md) ⇐ ([Universe](tree/Q1.md) [*is a*](tree/P31.md) [natural physical object](tree/Q16686022.md)), ([*is a*](tree/P31.md) [*is inverse of*](tree/P1696.md) [is for example](tree/Q21514624.md))

The part before `⇐` is the conclusion, derived from the facts listed after `⇐` using the applicable rules.

## Loading and Processing Wikidata

To download the compressed JSON file, browse to https://dumps.wikimedia.org/wikidatawiki/entities/. You may need to
search through the subdirectories to find a download link for `wikidata-*-all.json.bz2`.

After uncompression, you may start zelph with the provided `wikidata.zph` script:

```bash
zelph_app sample_scripts/wikidata.zph
```

To import Wikidata data, use the `.wikidata` command:

```
.wikidata download/wikidata-20250127-all.json
```

This creates an `.index` file in the same directory to accelerate future loading. Then, the inference mechanism can be started with either:

```
.run
```

or:

```
.run-md
```

The latter command generates Markdown files in the `mkdocs/docs/tree` directory, referencing Wikidata entities.
You can run these commands multiple times to perform additional inference passes, which can discover more facts and contradictions based on the knowledge already inferred.
