# Querying in zelph

zelph provides powerful querying capabilities directly in its scripting language and interactive CLI. Queries allow you to search the semantic network for matching patterns, supporting variables, multiple conditions, and integration with inference rules. This page covers general queries first (applicable to any domain), followed by Wikidata-specific examples.

Queries are statements that contain variables (single uppercase letters) but no `=>` (which would make them rules). They are evaluated immediately without needing `.run`, though inference can expand the graph beforehand to reveal more matches.

> **Prefer standard SPARQL?** Since version 0.9.6, zelph also supports a substantial subset of the SPARQL query language, including transitive property paths (`wdt:P279+`), `OPTIONAL`, `MINUS`, `UNION`, `FILTER`, subqueries, and aggregation. Because SPARQL is standardized, existing queries — for example from the Wikidata Query Service — often run unchanged. See [SPARQL Queries](sparql.md). This page covers zelph's native query syntax, which additionally integrates with rules and inference.

## Key Features

- **Variables**: Single uppercase letters (A-Z) or words starting with an underscore `_`, scoped to the query.
- **Multi-Conditions (Conjunctions)**: Write the conditions as a comma list, e.g. `cond1, cond2`, or use the explicit form `*{(cond1) (cond2)} ~ conjunction`. Note that neither carries outer parentheses: `(cond1, cond2)` on a line by itself is a *term*, i.e. the beginning of a statement, and awaits the rest of it – see [Basic Usage](quickstart.md#basic-usage).
- **Wildcards**: Use variables for subjects, relations, or objects (e.g., `X R Y` matches any triple).
- **Inference Integration**: Automatically performed after each command (also see command `.auto-run`).
- **Output**: Matches are printed with bound values. If no matches: only the query is echoed. The order of the answers is not specified – with parallel unification (the default, see `.parallel`) it can differ between two runs of the same query; see [Deduction Output Modes](rules.md#deduction-output-modes).
- **Limitations**: A query is a statement, so it may span several lines, but each answer is one line and there is no paging.

## General Queries

These examples use a simple geography graph. Load them in zelph (`.lang zelph` mode) for testing:

```
zelph> Berlin "is capital of" Germany
zelph> Germany "is located in" Europe
zelph> Europe "has part" Germany
zelph> (X "is capital of" Y, Y "is located in" Z) => (X "is located in" Z)
((Y "is located in" Z), (X "is capital of" Y)) => (X "is located in" Z)
(Berlin "is located in" Europe) ⇐ {(Germany "is located in" Europe) (Berlin "is capital of" Germany)}
zelph>
```

### Single-Condition Queries

Basic pattern matching.

- Find capitals: `X "is capital of" Y`  
  Output:

  ```
  X "is capital of" Y
  Answer: Berlin "is capital of" Germany
  ```

- Find locations in Europe: `A "is located in" Europe`  
  Output (post-inference):
  ```
  A "is located in" Europe
  Answer: Germany "is located in" Europe
  Answer: Berlin "is located in" Europe
  ```

### Multi-Condition Queries

Combine for intersections.

- Capitals in Europe: `X "is located in" Europe, X "is capital of" Y`  
  Output:

  ```
  {(X "is capital of" Y) (X "is located in" Europe)}
  Answer: {(Berlin "is capital of" Germany) (Berlin "is located in" Europe)}
  ```

  > Note: In this example we use the comma `,` [syntax sugar for conjunctions](rules.md#syntax-sugar-for-conditions). The fully explicit form is `*{(X "is located in" Europe) (X "is capital of" Y)} ~ conjunction`, which answers identically.

## Wikidata-Specific Queries

For Wikidata, switch to `.lang wikidata` after loading a dump (`.load path/to/dump.json` or `.load cached.bin`). Queries use Q/P IDs or names (if set). Examples from paleontology (e.g., Brontosaurus Q3222766).

### Single-Condition Queries on Wikidata

- Instances of fossil taxon: `X P31 Q23038290`  
  Output: Many answers, e.g., `Answer: Q3222766 P31 Q23038290` (Brontosaurus).

- Parent taxa: `X P171 Q3222766`  
  Output: Taxa with Brontosaurus as parent (if any).

### Multi-Condition Queries on Wikidata

Combine for targeted searches.

- Fossil taxa in genus rank: `X P31 Q23038290, X P105 Q34740`  
  Output: Matches like Brontosaurus/Apatosaurus.

- Synonyms with parent taxon: `X P460 Q14326, X P171 Q2544161` (Apatosaurus synonyms in Diplodocidae)  
  Output:
  ```
  {(X P171 Q2544161) (X P460 Q14326)}
  Answer: {(Q3222766 P171 Q2544161) (Q3222766 P460 Q14326)}
  ```
  Since `Q3222766` is [Brontosaurus](https://www.wikidata.org/wiki/Q3222766), this answer means "The [parent taxon](https://www.wikidata.org/wiki/Property:P171) (P171) of [Brontosaurus](https://www.wikidata.org/wiki/Q3222766) is [Apatosaurinae](https://www.wikidata.org/wiki/Q2544161) (Q2544161), which is [said to be the same as](https://www.wikidata.org/wiki/Property:P460) [Apatosaurus](https://www.wikidata.org/wiki/Q14326) (Q14326).

## Tips and Advanced Usage

- **Debugging**: Use `.node`, `.out`, `.in` to inspect before querying.
- **Patterns**: Fixed parts in quotes if spaces; variables anywhere.
- For complex logic, define rules first, then query the inferred graph.

See [Rules and Inference](rules.md#rules-and-inference) for synergy with queries.
