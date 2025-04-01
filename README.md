# zelph: A Sophisticated Semantic Network System

## Quick Start Guide

### Prerequisites

zelph can be used on Linux, macOS, and Windows platforms.

### Building zelph

You need:

- C++ compiler (supporting at least C++17)
- CMake 3.25.2+
- Git 

```bash
# Clone the repository with all submodules
git clone --recurse-submodules https://github.com/acrion/zelph
cd zelph

# Configure the build (Release mode)
cmake -D CMAKE_BUILD_TYPE=Release -B build src

# Build the project
cmake --build build
```

### Basic Usage

Once built, you can run zelph in interactive mode:

```bash
./build/bin/zelph_app
```

Let’s try a basic example:

```
Berlin "is capital of" Germany
Germany "is located in" Europe
X is capital of Y, Y is located in Z => X is located in Z
```

After entering these statements, zelph will automatically infer that Berlin is located in Europe:

```
«Berlin» «is located in» «Europe» ⇐ («Germany» «is located in» «Europe»), («Berlin» «is capital of» «Germany»)
```

Note that none of the items used in the above statements are predefined, i.e. all are made known to zelph by these statements.
In section [Semantic Network Structure](#semantic-network-structure) you’ll find details about the core concepts, including syntactic details.  

### Using Sample Scripts

zelph comes with sample scripts to demonstrate its capabilities:

```bash
# Run with the English examples script
./build/bin/zelph_app sample_scripts/english.zph

# Or try the Wikidata integration script
./build/bin/zelph_app sample_scripts/wikidata.zph
```

### Importing Wikidata

zelph can import and process data from Wikidata:

```
# Within the zelph CLI
.wikidata path/to/wikidata-dump.json
```

For more details on Wikidata integration, see the [Working with Wikidata](#zelph-and-wikidata-finding-logical-connections-and-contradictions) section below.

### What’s Next?

- Explore the [Core Concepts](#core-concepts) to understand how zelph represents knowledge
- Learn about [Rules and Inference](#rules-and-inference) to leverage zelph’s reasoning capabilities
- Check out the [Example Script](#example-script) for a comprehensive demonstration
 
## Introduction

zelph is an innovative semantic network system that allows inference rules to be defined within the network itself.
This project provides a powerful foundation for knowledge representation and automated reasoning, with a special focus on efficient memory usage and logical inference capabilities.
With dedicated import functions and specialized semantic scripts (like [wikidata.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/wikidata.zph)),
zelph offers powerful analysis capabilities for the complete Wikidata knowledge graph while remaining adaptable for any semantic domain.

The zelph ecosystem includes:

- A core C++ library providing both C++ and C interfaces
- A single command-line binary that offers both interactive usage (CLI) and batch processing capabilities
- API functions beyond what’s available in the command-line interface
- Integration options for languages like Go and Lua through the C interface

The key features of zelph include:

- Representation of knowledge in a semantic network structure
- Rules encoded within the same semantic network as facts
- Support for multi-language node naming
- Contradiction detection and resolution
- Memory-efficient data structures optimized at bit level
- A flexible scripting language for knowledge definition and querying
- Built-in import functionality for Wikidata JSON datasets

## Core Concepts

### Semantic Network Structure

In zelph, knowledge is represented as a network of nodes connected by relations.
Unlike traditional semantic networks where relations are labeled edges,
zelph treats relation types as first-class nodes themselves.
This unique approach enables powerful meta-reasoning about relations.

### Facts and Relations

Facts in zelph are represented as triples consisting of a subject, relation type, and object.
The standard relation type is `~`, which represents a categorical relation (similar to "is a" or "instance of").
For example:

```
X ~ Y
```

This means "X is an instance of category Y" or "X is a Y".

#### Working with Custom Relations

zelph can work with any type of relation, not just the standard `~` relation.
Here’s how custom relations work:

```
is opposite of ~ ->
> «is opposite of» «~» «->»
white is opposite of black
> «white» «is opposite of» «black»
```

In this example, using the interactive CLI, the first line declares "is opposite of" as a relation type (a member of the `->` category).
After the `>` symbol, we see zelph’s responses.

zelph creates new relation types automatically as needed. The explicit declaration of "is opposite of" can actually be omitted:

```
white "is opposite of" black
> «white» «is opposite of» «black»
```

Here, zelph automatically recognizes that "is opposite of" must be a relation type.
Note that when a relation contains spaces and is being used for the first time, it must be enclosed in quotation marks.
Once the relation is known to zelph, the quotation marks are no longer necessary in subsequent usage.

### Internal Representation of facts

In a conventional semantic network, relations between nodes are labeled, e.g.

```mermaid
graph LR
    white -->|is opposite of| black
```

zelph’s representation of relation types works fundamentally differently.
As mentioned in the introduction, one of zelph’s distinguishing features is that it treats relation types as first-class nodes rather than as mere edge labels.

At the network level, there is only a single primitive relation type: `~`, which represents a general category relation.
Far from being a limitation, this is actually one of zelph’s most powerful characteristics.
Relations that differ from the basic `~` type are not represented as arrow labels in zelph, but as regular nodes with the same status as any other node in the network.

Internally, zelph creates special nodes to represent relations. For example, when defining:

```
"is opposite of" ~ ->
```

This tells zelph that "is opposite of" is a relation (represented by `->`, which is the category of all relations).
zelph creates a special node to represent this fact.

This can be visualized as follows:

```mermaid
graph TD
    A("is opposite of ~ ->") <--> B("is opposite of")
    C("->") --> A
    A --> D("~")
```

The nodes `->` and `~` are predefined zelph nodes. `->` represents the category of all relations, while `~` represents a subset of this category, namely the category of categorical relations. Every relation that differs from the standard relation `~` (like "is opposite of") is linked to `->` via a `~` relation.

The node `is opposite of ~ ->` represents this specific relation (hence its name).
The relations to other nodes encode its meaning.

This approach provides several advantages:

1. It enables meta-reasoning about relations themselves
2. It simplifies the underlying data structures
3. It allows relations to participate in other relations (higher-order relations)
4. It provides a unified representation mechanism for both facts and rules

This architecture is particularly valuable when working with knowledge bases like Wikidata, where relations (called "properties" in Wikidata terminology) are themselves first-class entities with their own attributes, constraints, and relationships. zelph’s approach naturally aligns with Wikidata’s conceptual model, allowing for seamless representation and inference across the entire knowledge graph.

Similarly, when stating:

```
white "is opposite of" black
```

zelph creates a special relation node that connects the subject "white" bidirectionally, the object "black" in reverse direction, and the relation type "is opposite of" in the forward direction.

```mermaid
graph TD
    A("white is opposite of black") <--> B("white")
    C("black") --> A
    A --> D("is opposite of")
```

The directions of the relations are as follows:

| Element        | Example        | Relation Direction |
|----------------|----------------|--------------------|
| Subject        | white          | bidirectional      |
| Object         | black          | backward           |
| Relation Type  | is opposite of | forward            |

This semantics is used by zelph in several contexts, such as rule unification. It’s required because zelph doesn’t encode relation types as labels on arrows but rather as equal nodes. This has the advantage of facilitating statements about statements, for example, the statement that a relation is transitive.

This design prevents subject and object from being identical in a relation. There are examples of this in Wikidata, e.g., "South Africa (Q258)" "country (P17)" "South Africa (Q258)". "South Africa" is thus linked to itself in Wikidata via the relation (property) "Country". These examples are extremely rare in Wikidata and are ignored during import, with a warning.

## Creating a node graph

You can generate a node graph yourself using zelph’s `.dot` command, which outputs a GraphViz DOT format file. For example:

```
.dot name 2
```

In this example, `name` refers to the node identifier (in the currently active language specified via the `.lang` command) whose connections you want to visualize. The following number represents the depth of connections to include in the graph.

To convert the DOT file into an actual image, use the GraphViz command-line tool as follows:

```bash
dot -Tpng -oname.svg name.dot
```

## Rules and Inference

One of zelph’s most powerful features is the ability to define inference rules within the same network as facts. Rules are statements containing `=>` with conditions before it and a consequence after it.

Example rule:

```
R ~ "transitive relation", X R Y, Y R Z => X R Z
```

This rule states that if R is a transitive relation, and X is related to Y by R, and Y is related to Z by R, then X is also related to Z by R.
Variables in the zelph syntax are currently single uppercase letters. This restricts the number of variables in a rule to 26.
Note that the scope of variables always is a single rule. Internally, more complex rules are possible, but this is currently
only supported via zelph’s API, not via the scripting interface. See [main.cpp](https://github.com/acrion/zelph/blob/main/src/app/main.cpp)
for an example on how to use the API.

Here is a practical example of how this rule works in zelph (which you can also try out in interactive mode):

```
R ~ transitive relation, X R Y, Y R Z => X R Z
> ((Y R Z), (X R Y), (R «~» «transitive relation»)) «=>» (X R Z)
```

After the `>` symbol, we see zelph’s output, which in this case simply confirms the input of the rule.
The brackets `()` indicate that their content is represented as a separate node - each condition is a separate node in the semantic network.

Now, let’s declare that the relation `>` (greater than) belongs to the category (`~`) of transitive relations:

```
> ~ transitive relation
> «>» «~» «transitive relation»
```

Next, we provide three elements ("4", "5" and "6") for which the `>` relation applies:

```
6 > 5
> «6» «>» «5»
5 > 4
> «5» «>» «4»
«6» «>» «4» ⇐ («5» «>» «4»), («6» «>» «5»), («>» «~» «transitive relation»)
```

After entering `5 > 4`, zelph’s unification mechanism takes effect and automatically adds a new fact: `6 > 4`. This demonstrates the power of the transitive relation rule in action.

Rules can also define contradictions using `!`:

```
X "is opposite of" Y, A ~ X, A ~ Y => !
```

This rule states that if X is opposite of Y, then an entity A cannot be both an X and a Y, as this would be a contradiction.

If a contradiction is detected when a fact is entered (via the scripting language or during import of Wikidata data), the corresponding relation (the fact) is not entered into the semantic network. Instead, a fact is entered that describes this contradiction (making it visible in the Markdown export of the facts).

### Internal representation of rules

Let’s explain the internal representation of rules based on the example rule above.
The complete rule graph looks like this:

```mermaid
graph TD
    n1["((Y R Z), (X R Y), (R «~» «transitive relation»)) «=>» (X R Z)"] --> n2["=>"]
    n3["->"] --> n4["R «~» «->»"]
    n10["transitive relation"] --> n11["R «~» «transitive relation»"]
    n12["(Y R Z), (X R Y), (R «~» «transitive relation»)"] <--> n1
    n12 --> n13[","]
    n4 --> n8["~"]
    n4 <--> n14["R"]
    n11 --> n8
    n11 --> n12
    n11 <--> n14
    n15["X R Y"] --> n12
    n15 --> n14
    n16["X R Z"] --> n1
    n16 --> n14
    n17["X"] <--> n15
    n17 <--> n16
    n18["Y R Z"] --> n12
    n18 --> n14
    n19["Y"] --> n15
    n19 <--> n18
    n20["Z"] --> n16
    n20 --> n18
    
    style n12 fill:#87CEFA
    style n14 fill:#EEE8AA
    style n16 fill:#B3EE3A
```

This graph may seem somewhat overwhelming at first glance, but it follows a clear structure.
Let’s break it down:

1. The three conditions of the rule are connected to the blue condition node, which itself points to the logical operation of the condition: `,` (which represents the logical AND operation):
    ```mermaid
    graph TD
        n12["(Y R Z), (X R Y), (R «~» «transitive relation»)"] --> n13[","]
        n11["R «~» «transitive relation»"] --> n12
        n15["X R Y"] --> n12
        n18["Y R Z"] --> n12
        
        style n12 fill:#87CEFA
    ```
2. The blue condition node serves as the subject of the rule clause S => O (which is assigned the complete rule statement as a name). The green conclusion node functions as the object of the rule clause:
    ```mermaid
    graph TD
        n1["((Y R Z), (X R Y), (R «~» «transitive relation»)) «=>» (X R Z)"] --> n2["=>"]
        n12["(Y R Z), (X R Y), (R «~» «transitive relation»)"] <--> n1
        n16["X R Z"] --> n1
       
        style n12 fill:#87CEFA
        style n16 fill:#B3EE3A
    ```

3. Each condition, as well as the conclusion, is represented exactly like a fact (see the previous section "Internal Representation of facts").

This summarizes the complete diagram shown above. As mentioned earlier, the elegant aspect of this representation method is that the inference system can be applied not only to facts but also to rules.
Consequently, it becomes possible to formulate rules that generate other rules.

## Example Script

Here’s a comprehensive example demonstrating zelph’s capabilities:

```
X "is a" Y  => X ~ Y
X "is an" Y => X "is a" Y

is               "is a" ->
"has part"       "is a" ->
"is opposite of" "is a" ->

"is attribute of" "is opposite of" is
"is part of"      "is opposite of" "has part"
"is for example"  "is opposite of" "is a"

"has part"      "is transitive"
"has attribute" "is transitive"
~               "is transitive"

R "is transitive", X R Y, Y R Z => X R Z
X is E, E "is a" K  => X is K
X "has part" P, P "is a" K  => X "has part" K
K is E, X "is a" K  => X is E
K "has part" P, X "is a" K  => X "has part" P
X "is opposite of" Y, X "is a" K => Y "is a" K
X "is opposite of" Y => Y "is opposite of" X
R "is opposite of" S, X R Y => Y S X

X "is opposite of" Y, A is X, A is Y => !
X "is opposite of" Y, A "has part" X, A "has part" Y => !
X "is opposite of" Y, A "is a" X, A "is a" Y => !
X is E, X "is a" E => !
X is E, E "is a" X => !
X is E, E "has part" X => !

generates "is a" ->
needs "is a" ->

"is needed by" "is opposite of" needs
"is generated by" "is opposite of" generates

X generates energy => X "is an" "energy source"
A is hot => A generates heat
A generates oxygen => A is alive

chimpanzee "is an" ape
ape is alive

chimpanzee "has part" hand
hand "has part" finger

"green mint" "is an" mint
"water mint" "is a" mint
peppermint "is an" mint
mint "is a" lamiacea
catnip "is a" lamiacea
"green mint" is sweet

"is ancestor of" "is transitive"
peter "is ancestor of" paul
paul "is ancestor of" pius
A "is ancestor of" pius
```

When executed, the last line is interpreted as a query, because it contains a variable (single uppercase letter) and is no rule. Here are the results:

```
Answer: «paul» «is ancestor of» «pius»
«catnip» «~» «lamiacea» ⇐ «catnip» «is a» «lamiacea»
«needs» «~» «->» ⇐ «needs» «is a» «->»
«water mint» «~» «mint» ⇐ «water mint» «is a» «mint»
«mint» «~» «lamiacea» ⇐ «mint» «is a» «lamiacea»
«chimpanzee» «has part» «finger» ⇐ («hand» «has part» «finger»), («chimpanzee» «has part» «hand»), («has part» «is» «transitive»)
«peter» «is ancestor of» «pius» ⇐ («paul» «is ancestor of» «pius»), («peter» «is ancestor of» «paul»), («is ancestor of» «is» «transitive»)
«water mint» «~» «lamiacea» ⇐ («mint» «~» «lamiacea»), («water mint» «~» «mint»), («~» «is» «transitive»)
«peppermint» «is a» «mint» ⇐ «peppermint» «is an» «mint»
«chimpanzee» «is a» «ape» ⇐ «chimpanzee» «is an» «ape»
«green mint» «is a» «mint» ⇐ «green mint» «is an» «mint»
«chimpanzee» «is» «alive» ⇐ («chimpanzee» «is a» «ape»), («ape» «is» «alive»)
«generates» «is opposite of» «is generated by» ⇐ «is generated by» «is opposite of» «generates»
«has part» «is opposite of» «is part of» ⇐ «is part of» «is opposite of» «has part»
«is a» «is opposite of» «is for example» ⇐ «is for example» «is opposite of» «is a»
«is» «is opposite of» «is attribute of» ⇐ «is attribute of» «is opposite of» «is»
«needs» «is opposite of» «is needed by» ⇐ «is needed by» «is opposite of» «needs»
«finger» «is part of» «hand» ⇐ («hand» «has part» «finger»), («has part» «is opposite of» «is part of»)
«hand» «is part of» «chimpanzee» ⇐ («chimpanzee» «has part» «hand»), («has part» «is opposite of» «is part of»)
«finger» «is part of» «chimpanzee» ⇐ («chimpanzee» «has part» «finger»), («has part» «is opposite of» «is part of»)
«sweet» «is attribute of» «green mint» ⇐ («green mint» «is» «sweet»), («is» «is opposite of» «is attribute of»)
«alive» «is attribute of» «ape» ⇐ («ape» «is» «alive»), («is» «is opposite of» «is attribute of»)
«transitive» «is attribute of» «is ancestor of» ⇐ («is ancestor of» «is» «transitive»), («is» «is opposite of» «is attribute of»)
«alive» «is attribute of» «chimpanzee» ⇐ («chimpanzee» «is» «alive»), («is» «is opposite of» «is attribute of»)
«transitive» «is attribute of» «has part» ⇐ («has part» «is» «transitive»), («is» «is opposite of» «is attribute of»)
«transitive» «is attribute of» «~» ⇐ («~» «is» «transitive»), («is» «is opposite of» «is attribute of»)
«transitive» «is attribute of» «has attribute» ⇐ («has attribute» «is» «transitive»), («is» «is opposite of» «is attribute of»)
«mint» «is for example» «green mint» ⇐ («green mint» «is a» «mint»), («is a» «is opposite of» «is for example»)
«lamiacea» «is for example» «catnip» ⇐ («catnip» «is a» «lamiacea»), («is a» «is opposite of» «is for example»)
«->» «is for example» «needs» ⇐ («needs» «is a» «->»), («is a» «is opposite of» «is for example»)
«mint» «is for example» «water mint» ⇐ («water mint» «is a» «mint»), («is a» «is opposite of» «is for example»)
«->» «is for example» «is» ⇐ («is» «is a» «->»), («is a» «is opposite of» «is for example»)
«->» «is for example» «has part» ⇐ («has part» «is a» «->»), («is a» «is opposite of» «is for example»)
«ape» «is for example» «chimpanzee» ⇐ («chimpanzee» «is a» «ape»), («is a» «is opposite of» «is for example»)
«lamiacea» «is for example» «mint» ⇐ («mint» «is a» «lamiacea»), («is a» «is opposite of» «is for example»)
«->» «is for example» «is opposite of» ⇐ («is opposite of» «is a» «->»), («is a» «is opposite of» «is for example»)
«->» «is for example» «generates» ⇐ («generates» «is a» «->»), («is a» «is opposite of» «is for example»)
«mint» «is for example» «peppermint» ⇐ («peppermint» «is a» «mint»), («is a» «is opposite of» «is for example»)
«green mint» «~» «mint» ⇐ «green mint» «is a» «mint»
«chimpanzee» «~» «ape» ⇐ «chimpanzee» «is a» «ape»
«peppermint» «~» «mint» ⇐ «peppermint» «is a» «mint»
«peppermint» «~» «lamiacea» ⇐ («mint» «~» «lamiacea»), («peppermint» «~» «mint»), («~» «is» «transitive»)
«green mint» «~» «lamiacea» ⇐ («mint» «~» «lamiacea»), («green mint» «~» «mint»), («~» «is» «transitive»)
«is needed by» «is a» «->» ⇐ («needs» «is a» «->»), («needs» «is opposite of» «is needed by»)
«is attribute of» «is a» «->» ⇐ («is» «is a» «->»), («is» «is opposite of» «is attribute of»)
«is part of» «is a» «->» ⇐ («has part» «is a» «->»), («has part» «is opposite of» «is part of»)
«is generated by» «is a» «->» ⇐ («generates» «is a» «->»), («generates» «is opposite of» «is generated by»)
«->» «is for example» «is generated by» ⇐ («is generated by» «is a» «->»), («is a» «is opposite of» «is for example»)
«->» «is for example» «is attribute of» ⇐ («is attribute of» «is a» «->»), («is a» «is opposite of» «is for example»)
«->» «is for example» «is needed by» ⇐ («is needed by» «is a» «->»), («is a» «is opposite of» «is for example»)
«->» «is for example» «is part of» ⇐ («is part of» «is a» «->»), («is a» «is opposite of» «is for example»)
«is generated by» «~» «->» ⇐ «is generated by» «is a» «->»
«is needed by» «~» «->» ⇐ «is needed by» «is a» «->»
Ready.
```

The results demonstrate zelph’s powerful inference capabilities. It not only answers the specific query about who is an ancestor of pius, but also derives numerous other facts based on the rules and base facts provided in the script.

## Multi-language Support

zelph allows nodes to have names in multiple languages. This feature is particularly useful when integrating with external knowledge bases. The preferred language can be set in scripts using the `.lang` command:

```
.lang zelph
```

This capability is fully utilized in the Wikidata integration, where node names include both human-readable labels and Wikidata identifiers. An item in zelph can be assigned names in any number of languages, with Wikidata IDs being handled as a specific language ("wikidata").

## Project Status

The project is currently in alpha status. Core functionality is operational, but several key areas remain to be addressed:

- **Stability improvements**: While the system has successfully processed Wikidata’s 1.4 TB database multiple times, specific scenarios still trigger crashes (see code sections marked with TODO)
- **Dependency modernization**: The project currently relies on an outdated version of the Boost library. Rather than a direct update, replacing Boost with [Cbeam](https://github.com/acrion/cbeam) represents a more strategic approach. This would also facilitate robust implementation of multithreading support, critical for performance optimization.
- **REPL and parser refinement**: The REPL interface and the zelph language parser require architectural improvements.
- **Enhancement of semantic rules**: The [wikidata.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/wikidata.zph) script offers significant room for enhancement. Note that the node tree published on this site was generated using an earlier version of this script, [wikidata-var.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/wikidata-var.zph).
- **Potential Wikidata integration**: Exploring pathways for integration with the Wikidata ecosystem, e.g. the [WikiProject Ontology](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology).

Regarding potential Wikidata integration and the enhancement of `wikidata.zph`, collaboration with domain experts would be particularly valuable. Expert input on conceptual alignment and implementation of best practices would significantly accelerate development and ensure optimal compatibility with existing Wikidata infrastructure and standards.

## Licensing

zelph is dual-licensed:

1. **AGPL v3 or later** for open-source use,
2. **Commercial licensing** for closed-source integration or special requirements.

We would like to emphasize that offering a dual license does not restrict users of the normal open-source license (including commercial users).
The dual licensing model is designed to support both open-source collaboration and commercial integration needs.
For commercial licensing inquiries, please contact us at [https://acrion.ch/sales](https://acrion.ch/sales).

# zelph and Wikidata: Finding Logical Connections and Contradictions

## Wikidata as an Ideal Use Case for zelph

Wikidata represents an excellent application case for zelph’s capabilities.
It contains over 113 million entries interconnected by relations, all subject to logical constraints.
This complex web of knowledge presents two key opportunities for zelph:

1. **Finding contradictions**: Identifying logical inconsistencies in the data
2. **Making deductions**: Deriving new facts through logical inference

For example, if class `A` is the opposite of class `B` (such as [successor](https://zelph.org/tree/Q106110771) and [predecessor](https://zelph.org/tree/Q106110777)), then no entity `X` can belong to both classes (like [replacing entity](https://zelph.org/tree/Q45025415)).

Similarly, inferences can be made. Example: If X is related to Y and Y is related to Z through the same relation (e.g., X=[Canada](https://zelph.org/tree/Q16), Y=[American continent](https://zelph.org/tree/Q828), Z=[Earth's surface](https://zelph.org/tree/Q1349417), relation=[is part of](https://zelph.org/tree/P361)), and the relation is [transitive](https://zelph.org/tree/Q64861), then X must also be related to Z in the same way.

### Architectural Synergy with Wikidata

zelph’s architecture of treating relations as first-class nodes creates a perfect alignment with Wikidata’s data model.
In Wikidata, properties (P-entities) are not merely labels on edges but are themselves entities with their own attributes, constraints, and relationships to other entities.
This fundamental similarity enables zelph to:

1. **Naturally represent Wikidata’s property hierarchy**: Properties in Wikidata can have subproperties, domains, ranges, and other metadata - all of which are directly representable in zelph’s relation-as-node approach.

2. **Reason about properties themselves**: zelph can apply inference rules to properties just as it does to regular entities, enabling powerful meta-reasoning capabilities essential for working with Wikidata’s complex property structure.

3. **Enforce property constraints**: Wikidata’s property constraints (symmetry, transitivity, inverse relationships) map directly to zelph’s rule system, allowing automatic validation and inference.

This structural compatibility makes zelph well-suited for analyzing and enriching Wikidata’s knowledge graph while maintaining its semantic integrity.

### Navigating the Generated Knowledge Tree

zelph has processed Wikidata’s vast knowledge graph and generated a [tree of 4,580 pages](https://zelph.org/tree-explanation) representing entities and properties where meaningful deductions or contradictions were found.
This represents the practical application of zelph’s semantic inference capabilities to real-world data.

The navigation menu provides entry points to explore various branches of this knowledge tree, organized by knowledge domains and semantic relation types.
Particularly noteworthy is the [Logical Contradictions](https://zelph.org/tree/Q363948) page, which highlights inconsistencies in Wikidata’s knowledge structure that could be valuable for Wikidata curators.

For a detailed explanation of how this tree was generated and what it represents, see the [Tree Explanation](https://zelph.org/tree-explanation) page.

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

1. [instance of (P31)](https://zelph.org/tree/P31)
2. [subclass of (P279)](https://zelph.org/tree/P279)
3. [facet of (P1269)](https://zelph.org/tree/P1269)

zelph’s flexible design accommodates these distinctions.
The idea of the script is to follow the [Wikidata usage guidelines](https://www.wikidata.org/wiki/Property:P2559).
It can be easily adapted or extended for further improvements.

Notably, Wikidata only marks "subclass of" as transitive, not the other two relations.
This makes sense for "instance of" (since an instance is not a class), but the script adds transitivity for "facet of" along with additional rules that reflect its documented meaning:
if X is a "facet of" Y, then X inherits all properties of Y.

For this case, the following rules are included in the script:

- If `Y` is an [instance of](https://zelph.org/tree/P31) `C`, then `X` must also be an [instance of](https://zelph.org/tree/P31) `C`.
- If `Y` is a [subclass of](https://zelph.org/tree/P279) `C`, then `X` must also be a [subclass of](https://zelph.org/tree/P279) `C`.
- If `Y` [has part](https://zelph.org/tree/P527) `P`, then `X` must also [have part](https://zelph.org/tree/P527) `P`.
- If `Y` is [part of](https://zelph.org/tree/P361) `P`, then `X` must also be [part of](https://zelph.org/tree/P361) `P`.
- If `Y` has a [characteristic](https://zelph.org/tree/P1552) `Q`, then `X` must also have a [characteristic](https://zelph.org/tree/P1552) `Q`.

### Example Inference Process

Here’s a step-by-step example of zelph’s inference process when working with Wikidata:

1. According to Wikidata, the property [greater than (P5135)](https://zelph.org/tree/P5135) is an instance of [transitive Wikidata property (Q18647515)](https://zelph.org/tree/Q18647515).
2. Wikidata also states that [transitive Wikidata property (Q18647515)](https://zelph.org/tree/Q18647515) is a [facet of (P1269)](https://zelph.org/tree/P1269) [transitive relation (Q64861)](https://zelph.org/tree/Q64861).
3. The script contains the rule: `X is facet of Y, Y ~ C => X ~ C`
4. Therefore, zelph infers that [greater than (P5135)](https://zelph.org/tree/P5135) is also an instance of [transitive relation (Q64861)](https://zelph.org/tree/Q64861).

## Rules in the Semantic Network

Rules in zelph are encoded in the same semantic network as facts, using the special relation `=>` (which corresponds to [logical consequence (Q374182)](https://www.wikidata.org/wiki/Q374182) in Wikidata).

This innovative approach enables tight integration between the fact base and the rules, allowing rules to be reasoned about in the same way as facts.
This makes zelph particularly powerful for applications like Wikidata, where the knowledge base itself contains statements about relations, including properties like [transitivity](https://zelph.org/tree/Q18647515).

A rule is just a special case of a fact that uses the relation `=>`. In the case of the application of zelph to Wikidata data, this relation corresponds to [logical consequence](https://zelph.org/tree/Q374182).

## Publishing Results

The results of zelph’s analysis are published as a tree of pages, where each page corresponds to a Wikidata item. Pages can contain two sections:

1. **Deductions**: New facts derived about the item through logical inference
2. **Contradictions**: Logical inconsistencies involving the item

Example of a deduction:

- [natural physical object](https://zelph.org/tree/Q16686022) [is for example](https://zelph.org/tree/Q21514624) [Universe](https://zelph.org/tree/Q1) ⇐ ([Universe](https://zelph.org/tree/Q1) [*is a*](https://zelph.org/tree/P31) [natural physical object](https://zelph.org/tree/Q16686022)), ([*is a*](https://zelph.org/tree/P31) [*is inverse of*](https://zelph.org/tree/P1696) [is for example](https://zelph.org/tree/Q21514624))

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
