This page demonstrates how to use Janet's file I/O and JSON capabilities to import external data into zelph's semantic network and export inferred knowledge back to files.

For general Janet integration, see [Scripting with Janet](janet.md). For installing external Janet packages (required for JSON support), see [Installing External Packages](janet.md#installing-external-packages).

## Importing JSON Data

### Example: Biological Taxonomy

Taxonomic classifications are a natural fit for semantic networks — they form hierarchies that benefit directly from transitive reasoning. Suppose you have collected species data in a JSON file and want to use zelph to infer ancestor relationships, detect classification conflicts, or cross-reference with other knowledge.

Create a file [taxonomy.json](https://github.com/acrion/zelph/blob/main/sample_scripts/taxonomy.json):

```json
[
  {
    "species": "Wolf",
    "family": "Canidae",
    "order": "Carnivora",
    "class": "Mammalia"
  },
  {
    "species": "Red Fox",
    "family": "Canidae",
    "order": "Carnivora",
    "class": "Mammalia"
  },
  {
    "species": "Brown Bear",
    "family": "Ursidae",
    "order": "Carnivora",
    "class": "Mammalia"
  },
  {
    "species": "House Cat",
    "family": "Felidae",
    "order": "Carnivora",
    "class": "Mammalia"
  },
  {
    "species": "Cobra",
    "family": "Elapidae",
    "order": "Squamata",
    "class": "Reptilia"
  },
  {
    "species": "Iguana",
    "family": "Iguanidae",
    "order": "Squamata",
    "class": "Reptilia"
  }
]
```

### Reading and Parsing the File

Janet provides `slurp` (read entire file as string) and `spit` (write string to file) as built-in functions — no external packages needed for file I/O. For JSON parsing and encoding, zelph uses the `spork/json` package (see [Installing External Packages](janet.md#installing-external-packages)).

Create a zelph script [import_taxonomy.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/import_taxonomy.zph):

```
%(use spork/json)

%
(def raw (slurp "taxonomy.json"))
(def records (decode raw))

(each record records
  (def species (get record "species"))
  (def family  (get record "family"))
  (def order   (get record "order"))
  (def class   (get record "class"))

  # Create taxonomic relationships
  (zelph/fact species  "member of" family)
  (zelph/fact family   "member of" order)
  (zelph/fact order    "member of" class))

(printf "Imported %d species." (length records))
%
```

Run it:

```
zelph> .import import_taxonomy.zph
Imported 6 species.
```

At this point, the graph contains facts like `Wolf "member of" Canidae`, `Canidae "member of" Carnivora`, and so on. Note that `zelph/fact` is idempotent — importing the same family-to-order relationship multiple times (once per species) does not create duplicate facts.

### Applying Rules to Imported Data

Now that the data is in the graph, zelph's inference engine can reason over it. Add a transitive rule to derive indirect membership:

```
(R is transitive, A R B, B R C) => (A R C)
"member of" is transitive
```

The first line defines a general transitivity rule using variables — it fires for any relation `R` that has been declared transitive. The second line declares that `"member of"` is such a relation. Note that these are two separate statements: the rule contains only variable-based conditions, and the declaration is a plain fact.

zelph will automatically deduce facts like:

```
( Wolf member of Carnivora )   ⇐ {( Canidae member of Carnivora ) ( member of is transitive ) ( Wolf member of Canidae )}
( Wolf member of Mammalia )    ⇐ {( Carnivora member of Mammalia ) ( member of is transitive ) ( Wolf member of Carnivora )}
( Cobra member of Reptilia )   ⇐ {( Squamata member of Reptilia ) ( member of is transitive ) ( Cobra member of Squamata )}
...
```

You can also add contradiction rules to validate the data. For example, to detect if a species is mistakenly assigned to two different families:

```
(A "member of" X, A "member of" Y, X ~ family, Y ~ family, X != Y) => !
```

### A Complete Import Script

Here is a self-contained script [taxonomy.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/taxonomy.zph) that imports the JSON, defines rules, and runs a query:

```
%(use spork/json)

# --- Import ---
%
(def raw (slurp "taxonomy.json"))
(def records (decode raw))

(each record records
  (def species (get record "species"))
  (def family  (get record "family"))
  (def order   (get record "order"))
  (def class   (get record "class"))

  (zelph/fact species "member of" family)
  (zelph/fact family  "member of" order)
  (zelph/fact order   "member of" class)

  # Tag each rank for use in validation rules
  (zelph/fact species "~" "species")
  (zelph/fact family  "~" "family")
  (zelph/fact order   "~" "order"))

(printf "Imported %d species." (length records))
%

# --- Rules ---
(R is transitive, A R B, B R C) => (A R C)
"member of" is transitive

# --- Query: all mammals ---
X "member of" Mammalia
```

## Exporting Knowledge to JSON

After reasoning, you can extract knowledge from zelph's graph and write it to a JSON file. This is useful for feeding inferred facts into other systems, generating reports, or creating datasets for further processing.

### Querying and Collecting Results

Use `zelph/query` to extract bindings, then build a Janet data structure and encode it as JSON ([export_taxonomy.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/export_taxonomy.zph)):

```
%(use spork/json)

%
# Find all species that are (directly or transitively) members of Mammalia
(def results (zelph/query (zelph/fact 'X "member of" "Mammalia")))

# Build an array of result objects
(def output @[])
(each r results
  (def name (zelph/name (get r 'X)))
  (when name
    (array/push output {"entity" name "relation" "member of" "target" "Mammalia"})))

# Write to file
(def json-str (encode output))
(spit "mammals.json" json-str)
(printf "Exported %d results to mammals.json" (length output))
%
```

The resulting `mammals.json` will contain both the directly stated and the inferred memberships:

```json
[
  { "entity": "Wolf", "relation": "member of", "target": "Mammalia" },
  { "entity": "Red Fox", "relation": "member of", "target": "Mammalia" },
  { "entity": "Brown Bear", "relation": "member of", "target": "Mammalia" },
  { "entity": "House Cat", "relation": "member of", "target": "Mammalia" },
  { "entity": "Canidae", "relation": "member of", "target": "Mammalia" },
  { "entity": "Ursidae", "relation": "member of", "target": "Mammalia" },
  { "entity": "Felidae", "relation": "member of", "target": "Mammalia" },
  { "entity": "Carnivora", "relation": "member of", "target": "Mammalia" }
]
```

Note that families and orders appear as well — they were inferred as transitive members of Mammalia.

### Exporting a Filtered Subset

You can combine `zelph/query` with Janet's filtering to export only specific results. For instance, [exporting only species](https://github.com/acrion/zelph/blob/main/sample_scripts/export_taxonomy_filtered.zph) (not families or orders):

```
%(use spork/json)

%
(def results (zelph/query (zelph/fact 'X "member of" "Mammalia")))

(def species-only
  (filter (fn [r] (zelph/exists (get r 'X) "~" "species")) results))

(def output
  (map (fn [r] {"species" (zelph/name (get r 'X))}) species-only))

(spit "mammal_species.json" (encode output))
(printf "Exported %d species." (length output))
%
```

### Exporting All Deduced Facts

For a broader export, you can iterate over all predicates of interest and collect their facts ([export_taxonomy_all.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/export_taxonomy_all.zph)):

```
%(use spork/json)

%
(def all-members (zelph/query (zelph/fact 'X "member of" 'Y)))

(def output
  (map (fn [r]
    {"subject"   (zelph/name (get r 'X))
     "predicate" "member of"
     "object"    (zelph/name (get r 'Y))})
  all-members))

(spit "all_membership.json" (encode output))
(printf "Exported %d membership facts." (length output))
%
```

This captures both the original facts and everything zelph inferred through transitive reasoning — a complete picture of the `"member of"` relation in the graph.

## Working with CSV Data

For CSV files, Janet's built-in string functions are sufficient — no external package is needed. Here is a minimal pattern for importing tab-separated or [comma-separated data](https://github.com/acrion/zelph/blob/main/sample_scripts/data.csv) ([import_csv.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/import_csv.zph)):

```
%
(def lines (string/split "\n" (slurp "data.csv")))

(each line (slice lines 1)  # skip header row
  (def fields (string/split "," line))
  (when (>= (length fields) 3)
    (def subj (string/trim (get fields 0)))
    (def pred (string/trim (get fields 1)))
    (def obj  (string/trim (get fields 2)))
    (zelph/fact subj pred obj)))
%
```

For CSV files with quoted fields or embedded commas, consider using the `spork/csv` module (installed alongside `spork/json` via `jpm install spork`).

## Summary

| Task                        | Key Functions                                                                              |
| :-------------------------- | :----------------------------------------------------------------------------------------- |
| Read a file                 | `(slurp "path")` — built-in Janet                                                          |
| Write a file                | `(spit "path" content)` — built-in Janet                                                   |
| Parse JSON                  | `(decode str)` — from `spork/json` ([installation](janet.md#installing-external-packages)) |
| Encode JSON                 | `(encode value)` — from `spork/json`                                                       |
| Create facts from data      | `(zelph/fact subject predicate object)`                                                    |
| Query the graph             | `(zelph/query (zelph/fact 'X pred 'Y))`                                                    |
| Check existence (read-only) | `(zelph/exists subj pred obj)`                                                             |
| Get node name as string     | `(zelph/name node)`                                                                        |
