# Binary Data Files

Here, I will regularly publish precompiled `.bin` files for zelph that you can load and use directly. These files contain prepared semantic networks, mainly based on Wikidata data, but also on other domains. The focus is on efficiency: compared with JSON files, which can take hours to read, `.bin` files load in just a few minutes, depending on the hardware.

I plan to upload new `.bin` files regularly based on current Wikidata dumps (see [Wikidata Dumps](https://dumps.wikimedia.org/wikidatawiki/entities/) for transparency), and also to provide files for other data sources in the future.

## Available Files

All `.bin` files are available on [Hugging Face](https://huggingface.co/datasets/acrion/zelph).

Currently, I offer the following Wikidata variants:

| File                               | Variant                                                                            |       Nodes | File Size | RAM Usage | Name Entries (`wikidata` / `en`) | Load Time |
| ---------------------------------- | ---------------------------------------------------------------------------------- | ----------: | --------: | --------: | -------------------------------: | --------: |
| `wikidata-20260309-all.bin`        | Current full Wikidata dump for high-memory systems                                 | 983,424,620 |    82 GiB | 223.7 GiB |         119,231,266 / 83,261,799 |   23m 23s |
| `wikidata-20260309-all-pruned.bin` | Current pruned Wikidata dump optimised for substantially lower memory requirements |  74,608,727 |   5.6 GiB |  15.4 GiB |           13,610,498 / 6,778,692 |     58.7s |
| `wikidata-20171227.bin`            | Historic full Wikidata dump from 2017                                              | 203,190,311 |    18 GiB |  44.6 GiB |          42,187,613 / 27,960,315 |    3m 20s |
| `wikidata-20171227-pruned.bin`     | Historic pruned Wikidata dump from 2017 with reduced memory requirements           |  17,407,259 |   1.4 GiB |   3.8 GiB |            4,307,749 / 2,324,957 |     14.0s |

The values above reflect observed loading statistics from zelph on my system. Actual loading times and memory usage may vary depending on hardware and build configuration.

## Using the Files

To load a `.bin` file in zelph, start zelph in interactive mode and use the command:

```zelph
.load /path/to/file.bin
```

This loads the network directly into memory. Afterwards, you can execute queries, define rules, or start inference (e.g. with `.run`). For Wikidata-specific work, first load the script `wikidata.zph` (see [Wikidata Integration](wikidata.md)) and adjust the language:

```zelph
.import sample_scripts/wikidata.zph
.lang wikidata
```

Tip: if you work with the full JSON file, zelph automatically creates a `.bin` cache file during the first import to speed up future runs.

## Generation of the Pruned Files

The pruned versions mentioned above were created by systematically pruning (removing) large knowledge domains from the corresponding full Wikidata dumps. The goal was to reduce biological, chemical, astronomical, and geographical domains in order to lower the memory requirement without losing the core data. The process involved loading the data, targeted removal of nodes and facts based on instance ([P31](https://www.wikidata.org/wiki/Property:P31)) and subclass relationships ([P279](https://www.wikidata.org/wiki/Property:P279)), and cleanup operations. For details, please refer to the corresponding log files, see [https://github.com/acrion/zelph/tree/main/logs](https://github.com/acrion/zelph/tree/main/logs)
