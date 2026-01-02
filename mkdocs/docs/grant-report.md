# Wikimedia Rapid Fund Report: Wikidata Contradiction Detection and Constraint Integration

**Project:** [zelph: Wikidata Contradiction Detection and Constraint Integration](https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Wikidata_Contradiction_Detection_and_Constraint_Integration_(ID:_23553409))  
**Grant Period:** November 1, 2025 – December 31, 2025  
**Version:** 0.9 (Beta)

## Executive Summary

This report details the outcomes of the development sprint funded by the Wikimedia Rapid Fund. The primary objectives were to develop a reporting system for prioritized contradiction detection and to integrate Wikidata Property Constraints into the zelph reasoning engine.

During the project, significant insights gained from collaboration with the [Wikidata Ontology Cleaning Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Cleaning_Task_Force) led to a strategic pivot in how zelph processes data. Instead of indiscriminate deduction, the focus shifted to targeted contradiction detection rules. This required a fundamental re-engineering of the import and memory management subsystems to allow the **entire** Wikidata graph (113+ million items) to be held simultaneously in RAM, a feat previously impossible with the alpha architecture.

The project has successfully moved zelph from Alpha to **Version 0.9 (Beta)**. All stability issues were resolved during the grant period.

## 1. Architectural Overhaul & Performance

To meet the requirement of "completeness"—analyzing the entire graph at once rather than iteratively—I completely refactored the data ingestion and reasoning core.

### Memory Architecture
The previous documentation claimed a memory footprint of 9.2 GB. **This was incorrect.** It was based on an iterative processing model that never held the full state.

The new architecture successfully processes the complete 1.4 TB Wikidata JSON dump. The resulting in-memory semantic network is serialized to disk for fast reloading.

*   **Raw Input:** ~1.4 TB (JSON)  
*   **Serialized State:** ~100 GB (Binary)  
*   **Runtime Memory Requirement:** ~256 GB RAM

Achieving this density required replacing standard STL containers with [`unordered_dense`](https://github.com/martinus/unordered_dense) maps and implementing bit-optimized data structures. While I run this on a 128 GB machine using aggressive ZRAM compression and swap, the system is designed for high-performance servers with 256 GB+ RAM.

### Reasoning Performance
I achieved a massive performance boost in the unification engine (approximately 1000x speedup), enabling efficient rule matching against the full dataset.

**Benchmark (Processing the first 1 GB of the dataset):**

*   **Before:** `Reasoning complete in 25626.9 ms` (2766 matches)  
*   **After:** `Reasoning complete in 34.5 ms` (2766 matches)

## 2. Methodology Pivot: Targeted Contradiction Detection

Initially, the project aimed to run generic deductions to find contradictions emerging from the consequences of facts. However, discussions with experts like Peter Patel-Schneider and Ege Atacan Doğan revealed that:

1.  Wikidata contains enough **immediate** contradictions that intermediate deductions are often unnecessary noise.  
2.  Unrestricted deduction rules on a dataset of this magnitude lead to an explosion of results and potential halting problems.

Consequently, the strategy shifted from generic scripts (like the old `wikidata.zph`) to specialized scripts that target specific classes of logical violations.

## 3. Case Study: Split Order Violations

I applied the new engine to detect "Split Order Classes" violations, a problem highlighted in [research by the Ontology Cleaning Task Force](https://arxiv.org/html/2411.15550v1). The research noted that standard SPARQL queries (via QLever) often run out of memory.

**zelph script used:**
```
.lang zelph

.name "is instance of" wikidata P31
.name "is subclass of" wikidata P279

I is subclass of C, I is instance of C => !
```

**Execution Results:**

The system successfully processed the entire 113.7 million node graph in a single pass.

*   **Matches processed:** 4,626,765  
*   **Contradictions found:** 744,517  
*   **Total Runtime:** ~2.5 hours (on i9-12900T with extensive swap usage; significantly faster on full RAM).

The resulting 744,517 contradictions were heavily concentrated around genes and proteins. I have categorized and published the full reports here:

*   **[Genes](../split-order-classes/gene)**
*   **[Proteins](../split-order-classes/protein)**
*   **[Other Entities](../split-order-classes/other)**

## 4. Case Study: Disjointness Violations

I also attempted to detect disjointness violations using the following script, derived from the definitions in the [corresponding paper](https://arxiv.org/abs/2410.13707):

```
.lang wikidata

# P2738: disjoint union of
# P11260: list item (qualifier)
# P279: subclass of
# P31: instance of

C P2738 S, S P11260 A, S P11260 B, K P279 A, K P279 B => !
C P2738 S, S P11260 A, S P11260 B, X P31 A, X P31 B => !
```

**Result:** Zero contradictions found.
It is suspected that the translation of the SPARQL logic into zelph rules needs refinement, specifically regarding how list items are modeled in the network. This is a subject for the next task force meeting.

## 5. Constraint Integration Framework

A major deliverable of this grant was the automated importation of Wikidata Property Constraints.

### Implementation
I implemented a robust parser in `wikidata.cpp` that scans the JSON dump for statements involving the property constraint property (`P2302`).

*   **Total Properties with Constraints:** 12,366 identified.  
*   **Output:** For every property with constraints, zelph generates a corresponding `.zph` script containing comments describing the constraints.

### Constraint-to-Rule Conversion
While the framework extracts all constraints, mapping them to logic rules requires specific C++ implementations for each constraint type (see function `get_supported_constraints` in [wikidata.cpp](https://github.com/acrion/zelph/blob/main/src/lib/wikidata.cpp)). I have implemented generators for two high-impact constraints:

1.  **Conflicts-with constraint ([Q21502838](https://www.wikidata.org/wiki/Q21502838))**: `I P Y, I P31 Q5 => !`
2.  **None-of constraint ([Q52558054](https://www.wikidata.org/wiki/Q52558054))**: `I P ForbiddenValue => !`

**Example generated script (P421.zph):**
```bash
# Constraint: Q21502838
# ... (corresponding raw JSON from the Wikidata dump) ...
I P421 Y, I P31 Q5 => !

# Constraint: Q52558054
# ... (corresponding raw JSON from the Wikidata dump) ...
I P421 Q36669 => !
```

### Preliminary Results
I ran a test on the first 5 GB of the Wikidata dataset (approx. 0.2% of total items) using the 8,709 rules generated from these two constraint types.

*   **Result:** 6,690 contradictions found in this small sample.
*   **Extrapolation:** This suggests approximately **3.3 million** existing violations of just these two constraint types across the full database.

The list of generated rules is available at **[constraint-list.md](constraint-list.md)**.
The detected violations for the sample run are published at **[constraints/!](../constraints/!)**.

## 6. Future Directions

Based on the success of this beta phase, I have some ideas for next steps:

1.  **Server Deployment:** Deploying zelph on a machine meeting the hardware requirements (256 GB RAM, 16+ Cores) to eliminate swap latency and allow for rapid, on-demand analysis via a web queue.
2.  **Lisp Embedding:** The current custom scripting language has limitations. I plan to embed a Lisp dialect (e.g., Fennel or Janet) to treat "code as data," perfectly mirroring the semantic network structure where rules and facts are identical nodes. This will allow for Turing-complete logic within the reasoning engine.
3.  **Qualifier Import:** To support complex constraints like "Property Scope" ([Q53869507](https://www.wikidata.org/wiki/Q53869507)), the import mechanism must be extended to treat Qualifiers as first-class semantic nodes.

Discussions with Task Force Members will refine these.

## Conclusion

The grant has enabled zelph to mature from an experimental alpha into a high-performance beta capable of ingesting the entirety of Wikidata. By validating the system against real-world problems like Split Order Classes and implementing a scalable constraint import framework, zelph is now positioned as a powerful tool for the Wikidata community to systematically improve data quality.
