# Querying in zelph

zelph provides powerful querying capabilities directly in its scripting language and interactive CLI. Queries allow you to search the semantic network for matching patterns, supporting variables, multiple conditions, and integration with inference rules. This page covers general queries first (applicable to any domain), followed by Wikidata-specific examples.

Queries are statements that contain variables (single uppercase letters) but no `=>` (which would make them rules). They are evaluated immediately without needing `.run`, though inference can expand the graph beforehand to reveal more matches.

## Key Features
- **Variables**: Single uppercase letters (A-Z), scoped to the query. Limited to 26 per query.
- **Multi-Conditions**: Separate conditions with commas (logical AND). zelph unifies across all, binding variables consistently.
- **Wildcards**: Use variables for subjects, relations, or objects (e.g., `X R Y` matches any triple).
- **Inference Integration**: Run `.run` first to derive new facts, then query the expanded graph.
- **Output**: Matches are printed with bound values. No matches: Just the query echoed.
- **Limitations**: No OR/NOT in syntax (use rules for complex logic). No multi-line queries.

## General Queries 

These examples use a simple geography graph. Load them in zelph (`.lang zelph` mode) for testing:

```
Berlin "is capital of" Germany
Germany "is located in" Europe
Europe "has part" Germany
X is capital of Y, Y is located in Z => X is located in Z
"is located in" ~ transitive relation
R ~ transitive relation, X R Y, Y R Z => X R Z
.run  # Infer: Berlin "is located in" Europe
```

### Single-Condition Queries
Basic pattern matching.

- Find capitals: `X "is capital of" Y`  
  Output:  
  ```
  X «is capital of» Y
  Answer: «Berlin» «is capital of» «Germany»
  ```

- Find locations in Europe: `A "is located in" Europe`  
  Output (post-inference):  
  ```
  A «is located in» «Europe»
  Answer: «Berlin» «is located in» «Europe»
  Answer: «Germany» «is located in» «Europe»
  ```

### Multi-Condition Queries
Combine for intersections.

- Capitals in Europe: `X "is located in" Europe, X "is capital of" Germany`  
  Output:  
  ```
  (X «is capital of» «Germany»), (X «is located in» «Europe»)
  Answer: («Berlin» «is capital of» «Germany»), («Berlin» «is located in» «Europe»)
  ```

- Parts with opposites: Add `Europe "is opposite of" Asia`, then: `A "is opposite of" B, A "has part" Germany`  
  Output:  
  ```
  (A «is opposite of» B), (A «has part» «Germany»)
  Answer: («Europe» «is opposite of» «Asia»), («Europe» «has part» «Germany»)
  ```

- No match example: `X "is located in" Europe, X "has part" Germany`  
  Output: Just the query (no match, as nothing is both located in Europe and has Germany as part).

- Multi-Variable: `X "is located in" Y, Y "has part" Germany, X "is capital of" Z`  
  Output:  
  ```
  (X «is located in» Y), (X «is capital of» Z), (Y «has part» «Germany»)
  Answer: («Berlin» «is located in» «Europe»), («Berlin» «is capital of» «Germany»), («Europe» «has part» «Germany»)
  ```

Add symmetry for more: `"is opposite of" ~ symmetric relation`, `R ~ symmetric relation, A R B => B R A`, `.run`. Then: `A "is opposite of" B` shows bidirectional matches.

## Wikidata-Specific Queries

For Wikidata, switch to `.lang wikidata` after loading a dump (`.wikidata path/to/dump.json` or `.load cached.bin`). Queries use Q/P IDs or names (if set). Examples from paleontology (e.g., Brontosaurus Q3222766).

### Single-Condition Queries
- Instances of fossil taxon: `X P31 Q23038290`  
  Output: Many answers, e.g., `Answer: «Q3222766» «P31» «Q23038290»` (Brontosaurus).

- Parent taxa: `X P171 Q3222766`  
  Output: Taxa with Brontosaurus as parent (if any).

### Multi-Condition Queries
Combine for targeted searches.

- Fossil taxa in genus rank: `X P31 Q23038290, X P105 Q34740`  
  Output: Matches like Brontosaurus/Apatosaurus.

- Synonyms with parent taxon: `X P460 Q14326, X P171 Q2544161` (Apatosaurus synonyms in Diplodocidae)  
  Output:  
  ```
  (X «P171» «Q2544161»), (X «P460» «Q14326»)
  Answer: («Q3222766» «P171» «Q2544161»), («Q3222766» «P460» «Q14326»)
  ```

- No-match example: Musical works with taxon: `X P31 Q105543609, X P171 Q3222766`  
  Output: Just the query (no overlap between music and taxonomy).

## Tips and Advanced Usage
- **Pre-Inference**: Always `.run` for derived facts (e.g., transitivity).
- **Debugging**: Use `.node`, `.out`, `.in` to inspect before querying.
- **Patterns**: Fixed parts in quotes if spaces; variables anywhere.
- For complex logic, define rules first, then query the inferred graph.

See [Rules and Inference](index.md#rules-and-inference) for synergy with queries.
