# The Generated Knowledge Tree

## What Is This Tree?

The `/tree` directory on this website contains 4,580 automatically generated pages that represent a portion of the Wikidata knowledge graph after being processed through zelph’s semantic inference engine.
Each page corresponds to a specific Wikidata entity (Q-item) or property (P-item) and displays:

1. **Deductions**: New facts inferred by zelph’s rule system that aren't explicitly stated in Wikidata
2. **Contradictions**: Logical inconsistencies detected when applying zelph’s semantic rules to the entity

Each page title links directly to the corresponding entry in Wikidata for easy cross-reference.

## How Was It Generated?

This knowledge tree was created through the following process:

1. Cloning and building the zelph repository
2. Running the semantic analysis with wikidata-var.zph script:
   ```bash
   ./build/bin/zelph_app sample_scripts/wikidata-var.zph
   ```
3. Within the zelph application, importing the [Wikidata JSON dump](https://dumps.wikimedia.org/wikidatawiki/entities) and generating the Markdown pages:
   ```
   .wikidata download/wikidata-20250127-all.json
   .run-md
   ```
   The `.run-md` command was executed multiple times to build up the semantic network iteratively, as each subsequent execution leverages facts and relations established during previous runs.

4. Post-processing to fix broken links:
   ```bash
   cd mkdocs
   ./replace_invalid_mkdocs_links.raku
   ```
   This Raku script (installable via `pacman -S rakudo-bin` on [Ditana](https://ditana.org)) replaces links to non-existent pages with direct links to Wikidata.

## Why Only 4,580 Pages?

Although over 1.3 million Wikidata entities were imported into zelph, the resulting tree contains only 4,580 pages due to two factors:

1. Selective page generation: Pages were only created for entities where zelph could make meaningful deductions beyond what's explicitly stated in Wikidata, or where it detected logical contradictions when applying its rule system.
2. Non-exhaustive processing: The generation process was not exhaustive. Each run of `.run-md` builds upon previous runs by leveraging newly established facts and relations. While additional runs could potentially discover more deductions and contradictions, the current tree represents a substantial but non-exhaustive analysis of the knowledge graph.

This selective approach focuses on entities where zelph adds analytical value through its inference capabilities, rather than simply mirroring Wikidata’s content.

Particularly noteworthy is the [Logical Contradictions](tree/Q363948.md) page, which highlights inconsistencies in Wikidata’s knowledge structure that could be valuable for Wikidata curators.

The content and scope of generated pages depend significantly on the specific rules defined in [the script](https://github.com/acrion/zelph/blob/main/sample_scripts/wikidata.zph) and the number of inference iterations performed.
