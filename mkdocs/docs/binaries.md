# Binary Data Files

Here, I will regularly publish pre-compiled `.bin` files for zelph that you can load and use directly. These files contain prepared semantic networks, mainly based on Wikidata data, but also for other domains. The focus is on efficiency: Compared to JSON files (which can take hours to read), `.bin` files load in just a few minutes (depending on hardware).

I plan to regularly upload new `.bin` files based on the current Wikidata dumps (see [Wikidata Dumps](https://dumps.wikimedia.org/wikidatawiki/entities/) for transparency), but also for other data sources in the future.

## Available Files

All `.bin` files are available on [Hugging Face](https://huggingface.co/datasets/acrion/zelph).

Currently, I offer the following Wikidata variants:

- `wikidata-20251222-all.bin`: The full Wikidata dump, serialized for fast loading. Requires about 210 GB RAM.
- `wikidata-20251222-pruned.bin`: A reduced version of the full Wikidata dump, optimized for lower RAM requirements. This enables users with limited hardware to work with zelph and Wikidata – outside of the removed knowledge domains. Requires about 16 GB RAM.

## Using the Files

To load a `.bin` file in zelph, start zelph in interactive mode and use the command:

```
.load /path/to/file.bin
```

This loads the network directly into memory. Afterward, you can execute queries, define rules, or start inferences (e.g., with `.run`). For Wikidata-specific work, first load the script `wikidata.zph` (see [Wikidata Integration](wikidata.md)) and adjust the language:

```
.import sample_scripts/wikidata.zph
.lang wikidata
```

Tip: If you work with the full JSON file, zelph automatically creates a `.bin` cache file on the first import to speed up future runs.

## Generation of `wikidata-20251222-pruned.bin`

The file `wikidata-20251222-pruned.bin` was created by systematically pruning (removing) large knowledge domains from the full `wikidata-20251222-all.json` (approx. 1.7 TB). The goal was to reduce biological, chemical, astronomical, and geographical domains to lower the RAM requirement without losing the core data. The process involved loading the data, targeted removal of nodes and facts based on instance ([P31](https://www.wikidata.org/wiki/Property:P31)) and subclass relationships ([P279](https://www.wikidata.org/wiki/Property:P279)), and cleanup operations.

Here is a tabular overview of the steps (based on the protocol; I have consolidated redundant or test-like steps such as loading a 50 GB test and made educated guesses for transitions, e.g., that prunes were continued in sequential sessions):

| Step | Command / Action | Description / Removed Domain | Removed Nodes |
|------|------------------|------------------------------|---------------|
| 1 | `.load wikidata-20251222-all.json` (implicit via cache or direct) | Loading the full Wikidata data (113 million items) into zelph. Creates a `.bin` cache for quick reloading. | N/A |
| 2 | `.lang wikidata` | Switch to Wikidata language for correct ID handling. | N/A |
| 3 | `.prune-nodes A P31 Q8054` | Remove all instances of protein ([Q8054](https://www.wikidata.org/wiki/Q8054)). | 990416 |
| 4 | `.prune-nodes A P279 Q8054` | Remove all subclasses of protein ([Q8054](https://www.wikidata.org/wiki/Q8054)). | 17565 |
| 5 | `.prune-nodes A P31 Q7187` | Remove all instances of gene ([Q7187](https://www.wikidata.org/wiki/Q7187)). | 1074168 |
| 6 | `.prune-nodes A P279 Q7187` | Remove all subclasses of gene ([Q7187](https://www.wikidata.org/wiki/Q7187)). | 38756 |
| 7 | `.prune-nodes A P31 Q11173` | Remove all instances of chemical compound ([Q11173](https://www.wikidata.org/wiki/Q11173)). | 83 |
| 8 | `.prune-nodes A P279 Q11173` | Remove all subclasses of chemical compound ([Q11173](https://www.wikidata.org/wiki/Q11173)). | 1061177 |
| 9 | `.prune-nodes A P31 Q13442814` | Remove all instances of scholarly article ([Q13442814](https://www.wikidata.org/wiki/Q13442814)). | 45381672 |
| 10 | `.prune-nodes A P31 Q16521` | Remove all instances of taxon ([Q16521](https://www.wikidata.org/wiki/Q16521)). | 3799221 |
| 11 | `.prune-nodes A P31 Q5` | Remove all instances of human ([Q5](https://www.wikidata.org/wiki/Q5)). | 12930031 |
| 12 | `.prune-nodes A P131 B` | Remove all administrative location relationships ([P131](https://www.wikidata.org/wiki/Property:P131): located in administrative territorial entity). | 13608039 |
| 13 | `.cleanup` | Remove isolated nodes after pruning. | N/A |
| 14 | `.prune-nodes A P31 Q4167836` | Remove all instances of Wikimedia category ([Q4167836](https://www.wikidata.org/wiki/Q4167836)). | 5725423 |
| 15 | `.prune-nodes A P31 Q523` | Remove all instances of star ([Q523](https://www.wikidata.org/wiki/Q523)). | 3275598 |
| 16 | `.prune-nodes A P31 Q318` | Remove all instances of galaxy ([Q318](https://www.wikidata.org/wiki/Q318)). | 2100179 |
| 17 | `.prune-nodes A P31 Q4167410` | Remove all instances of Wikimedia disambiguation page ([Q4167410](https://www.wikidata.org/wiki/Q4167410)). | 1512307 |
| 18 | `.prune-nodes A P31 Q113145171` | Remove all instances of type of chemical entity ([Q113145171](https://www.wikidata.org/wiki/Q113145171)). | 285332 |
| 19 | `.prune-nodes A P31 Q11266439` | Remove all instances of Wikimedia template ([Q11266439](https://www.wikidata.org/wiki/Q11266439)). | 803009 |
| 20 | `.prune-nodes A P31 Q79007` | Remove all instances of street ([Q79007](https://www.wikidata.org/wiki/Q79007)). | 3903 |
| 21 | `.prune-nodes A P31 Q13433827` | Remove all instances of encyclopedia article ([Q13433827](https://www.wikidata.org/wiki/Q13433827)). | 654456 |
| 22 | `.prune-nodes A P31 Q101352` | Remove all instances of family name ([Q101352](https://www.wikidata.org/wiki/Q101352)). | 661987 |
| 23 | `.prune-nodes A P31 Q13100073` | Remove all instances of village of the People's Republic of China ([Q13100073](https://www.wikidata.org/wiki/Q13100073)). | 10695 |
| 24 | `.prune-nodes A P279 Q277338` | Remove all subclasses of pseudogene ([Q277338](https://www.wikidata.org/wiki/Q277338)). | 43974 |
| 25 | `.prune-nodes A P31 Q277338` | Remove all instances of pseudogene ([Q277338](https://www.wikidata.org/wiki/Q277338)). | 11172 |
| 26 | `.prune-nodes A P1433 B` | Remove all publication relationships ([P1433](https://www.wikidata.org/wiki/Property:P1433): published in). | 1110396 |
| 27 | `.prune-nodes A P31 Q3305213` | Remove all instances of painting ([Q3305213](https://www.wikidata.org/wiki/Q3305213)). | 1038138 |
| 28 | `.prune-nodes A P31 Q4022` | Remove all instances of river ([Q4022](https://www.wikidata.org/wiki/Q4022)). | 70687 |
| 29 | `.prune-nodes A P31 Q8502` | Remove all instances of mountain ([Q8502](https://www.wikidata.org/wiki/Q8502)). | 102503 |
| 30 | `.prune-nodes A P31 Q486972` | Remove all instances of human settlement ([Q486972](https://www.wikidata.org/wiki/Q486972)). | 61361 |
| 31 | `.prune-nodes A P31 Q2668072` | Remove all instances of collection ([Q2668072](https://www.wikidata.org/wiki/Q2668072)). | 502805 |
| 32 | `.prune-nodes A P31 Q3331189` | Remove all instances of version, edition or translation ([Q3331189](https://www.wikidata.org/wiki/Q3331189)). | 685955 |
| 33 | `.prune-nodes A P407 Q7850` | Remove all language relationships to Chinese ([Q7850](https://www.wikidata.org/wiki/Q7850): language of work or name). | 144497 |
| 34 | `.prune-nodes A P407 Q7737` | Remove all language relationships to Russian ([Q7737](https://www.wikidata.org/wiki/Q7737): language of work or name). | 37121 |
| 35 | `.prune-nodes A P921 B` | Remove all main subject relationships ([P921](https://www.wikidata.org/wiki/Property:P921): main subject). | 554601 |
| 36 | `.prune-nodes A P17 B` | Remove all country relationships ([P17](https://www.wikidata.org/wiki/Property:P17): country). | 4903773 |
| 37 | `.cleanup` | Final removal of isolated nodes. | 2519014 |
| 38 | `.save wikidata-20251222-pruned.bin` | Save the final pruned file. | N/A |

After these steps, the file was ready for publication. The pruning steps focused on the largest data volumes (e.g., biology, chemistry, astronomy, geography) to make it easier for users with standard hardware to get started.
