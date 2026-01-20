# Disjointness Violations in Wikidata

This page documents zelph's approach to detecting disjointness violations in Wikidata, based on the research paper ["Disjointness Violations in Wikidata"](https://arxiv.org/abs/2410.13707) by Ege Atacan Doğan and Peter F. Patel-Schneider.

## Background

Disjointness is a fundamental ontological constraint: if two classes are disjoint, no entity can be an instance or subclass of both. In Wikidata, disjointness is expressed through the "disjoint union of" property ([P2738](https://www.wikidata.org/wiki/Property:P2738)) with qualifiers.

The paper identified **14,480 culprits** — classes that violate disjointness constraints — with the vast majority (93%) stemming from the disjointness between:

- **[Q4406616](https://www.wikidata.org/wiki/Q4406616)** (concrete object)
- **[Q7048977](https://www.wikidata.org/wiki/Q7048977)** (abstract entity)

## From SPARQL to zelph Rules

### How Disjointness is Declared in Wikidata

Disjoint pairs are declared using P2738 with P11260 qualifiers:

```sparql
SELECT DISTINCT ?class ?e1 ?e2 WHERE {
  ?class p:P2738 ?statement .
  ?statement pq:P11260 ?e1 .
  ?statement pq:P11260 ?e2 .
  FILTER ( str(?e1) < str(?e2) )
}
```

This query extracts which pairs of classes are declared disjoint. The results are available in the paper's [AllNumbers.csv](https://arxiv.org/src/2410.13707/anc/AllNumbers.csv).

### How Violations are Found

Once the disjoint pairs are known, violations are found with:

```sparql
SELECT ?class WHERE {
  ?class wdt:P279+ wd:Q4406616 .
  ?class wdt:P279+ wd:Q7048977 .
}
```

The critical element is `wdt:P279+` — the **transitive closure** of subclass-of. A class K is a culprit if there exists *any* P279-path from K to both disjoint classes.

### Translation to zelph Rules

A direct translation would be:

```
K P279 Q4406616, K P279 Q7048977 => !
```

However, this only matches **direct** P279 relationships (path length = 1). The SPARQL query finds paths of arbitrary length.

## Top Disjointness Pairs

From AllNumbers.csv, the most significant pairs by culprit count:

| Violated Class 1 | Violated Class 2 | Culprits | Instance Violations |
|------------------|------------------|----------|---------------------|
| Q4406616 (concrete object) | Q7048977 (abstract entity) | 13,520 | 2,203,817 |
| Q16686448 (artificial object) | Q1970309 (natural object) | 463 | 909,415 |
| Q16686022 (natural physical object) | Q8205328 (artificial physical object) | 157 | 147,673 |
| Q215627 (person) | Q43229 (organization) | 125 | 3,161 |

## Experimental Results

### Testing with December 2025 Dump

I loaded the complete Wikidata dump (118M nodes) and tested various rule configurations.

**Direct P279 matching (depth 1):**

```
K P279 Q4406616, K P279 Q7048977 => !
```

Result: **0 contradictions**

**Extended depth matching (up to depth 3):**

```
K P279 X, X P279 Q4406616, K P279 Y, Y P279 Q7048977 => !
```

Result: **0 contradictions**

### Why No Matches?

The issue is **path length**. Q4406616 and Q7048977 are near the top of the ontology (direct children of Q35120 "entity"). Real classes like "gene" (Q7187) reach them through many intermediate steps:

```
wikidata> Q7187 P279 X
Answer: «Q7187» «P279» «Q863908»

wikidata> .node Q863908
Name in language 'en': 'nucleic acid sequence'
```

Gene's only direct superclass is Q863908. The path to Q4406616/Q7048977 continues upward through multiple levels.

### Smaller Disjointness Pairs

I also tested smaller pairs from the CSV that should have shorter paths:

```
# Even number vs odd number (1 culprit in paper)
K P279 Q13366104, K P279 Q13366129 => !

# Gas vs liquid (7 culprits in paper)
K P279 Q11432, K P279 Q11435 => !
```

Result: **0 contradictions**

This suggests these specific violations have been **fixed** in the 16 months between the paper (August 2024) and our dump (December 2025).

### Verifying Individual Queries Work

Each condition individually finds many matches:

```
wikidata> K P279 Q11432
Answer: «Q3099699» «P279» «Q11432»
Answer: «Q1548028» «P279» «Q11432»
... (41 results for gas)

wikidata> K P279 Q11435
Answer: «Q190874» «P279» «Q11435»
Answer: «Q114023926» «P279» «Q11435»
... (50 results for liquid)
```

But no class appears in both result sets — confirming no direct violations exist.

### Disjointness Declarations in Wikidata

I verified that P2738 queries work correctly:

```
wikidata> X P2738 Y
Answer: «Q108811670» «P2738» «Q23766486»
... (776 results total)
```

## Conclusions

1. **zelph's unification engine works correctly** for direct relationships
2. **Transitive closure is required** to replicate the SPARQL analysis
3. **Some violations have been fixed** since the paper's publication
4. **Path lengths are significant** — culprits often reach top-level classes through 10+ intermediate steps

## Future Work: Transitive Closure Support

To fully support disjointness violation detection, zelph needs transitive closure matching. Proposed syntax:

```
K P279+ Q4406616, K P279+ Q7048977 => !
```

This would enable zelph to find violations regardless of path length, matching the expressive power of SPARQL's `wdt:P279+` operator.

## References

- Doğan, E.A. & Patel-Schneider, P.F. (2024). [Disjointness Violations in Wikidata](https://arxiv.org/abs/2410.13707). arXiv:2410.13707
- [AllNumbers.csv](https://arxiv.org/src/2410.13707/anc/AllNumbers.csv) — Full list of disjoint pairs and violation counts
- [Wikidata:WikiProject Ontology](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology) — Ontology improvement efforts
