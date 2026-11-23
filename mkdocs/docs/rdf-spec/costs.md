# Costs

This page details the mapping costs in memory and time. All assertions regarding the mapping are specified behaviour: the value store, the content index, the dense array of value nodes, the expanders, the requests, and the import parameters have not yet been implemented. Its measurements pertain to today’s engine, the release build of commit `0124979`; when a specified structure comprises ordinary facts – an expansion, a full value, a check – it was assembled manually and measured. Each figure on the page says whether it is measured or computed, and on what foundation it is based; a figure for which no basis could be established is not included on the page.

The full Wikidata network contains nearly a billion nodes and demands greater RAM than any other publicly available network ([Binary Data Files](../binaries.md#available-files)), thus memory constraints take precedence over time. That is why [D2](decisions.md#d2) makes every expansion a request, why [D12](decisions.md#d12) leaves descriptions and aliases out of the default import, and why value nodes are specified outside the node map.

## How the Figures Are Made

**Measured** refers to a run using today’s engine. Memory denotes the resident size a script reads from `/proc/self/status` before and after constructing something, within the same process; a figure for a network as a load holds it originates from a network that was built, saved, and loaded in a fresh process. Node counts come from `.stat`. Instructions represent user-space instructions counted by `perf stat -e instructions:u` during a sequential execution (`.parallel` off); they repeat to 0.01 %, and the comparison tables prioritize them. Times correspond to the minimum among three runs. The system features an Intel i9-12900T; timed runs were pinned to its performance cores, memory runs to its efficiency cores, and other processes kept the load average between 2 and 6 (the check 3 run below ran at 12.8). Sizes are small: up to two million names, a million facts, and 20,000 compared pairs. The full network was not loaded for this page.

**Computed** refers to arithmetic derived from stated foundations: the code at commit `0124979` for sizes and thresholds, the measurements above for costs per unit, and counts from Wikidata for scenarios. Every figure for the full network is computed.

**Counts of Wikidata** originate in analyses conducted on 4 November 2026 and stem from one of two origins. The majority are estimates derived from a block sample of the JSON dump from 9 March 2026: each sample corresponds to the bzip2 block housing a uniformly drawn byte offset, each line contributes to the count of the block where it starts, and the Hansen-Hurwitz estimator computes the aggregate within a 95 % confidence interval. (Taking the subsequent block following a random offset and scaling by a ratio introduces a bias of up to threefold on this dump, where blocks come in pairs consisting of a large and a tiny one.) Infrequent or densely grouped measures – such as distinct full value nodes or the items of a class – are actual counts retrieved from the Wikidata Query Service on 4 November 2026. Every count listed below names its source.

**What `binaries.md` measured.** The RAM column in it was measured across two engines. The full network’s 223.7 GiB dates from the engine that kept two node maps; `-small` (5.5 GiB, 26,533,048 nodes, 222.6 B per node), the pruned network from 2017, and the `P279` slice were measured again once the engine moved to a single node map (commit `f91692d`). The full network’s 244.2 B per node (223.7 GiB across 983,424,620 nodes) thus reflects data from the earlier engine.

**Resident size and live memory.** The allocator, mimalloc, keeps freed memory in residence: following the loading of a network comprising a million names, each 62 characters long, the process holds 356.7 MB by default and 266.5 MB when freed memory is returned at once (`MIMALLOC_PURGE_DELAY=0`); a million facts across two million names occupy 591 MB compared to 529 MB (measured). The costs per unit presented on this page are taken with freed memory returned at once, thus reflecting the space the structures actually consume; a process’s resident size may exceed these figures.

## The Memory Model

### The Node Map

Each node within the graph has an entry in a single node map, an `ankerl::unordered_dense::segmented_map` (version 4.8.1) whose entry holds the node along with both sides of its adjacency (`node_entry`, `src/lib/network/network.hpp`). The table gives its components (computed from the code).

| Part | Size | Basis |
| --- | --- | --- |
| Entry: key and two adjacency sets | 40 B | 8 B key, two `adjacency_set`s each comprising an 8-byte word and a mode byte, 16 B each |
| Storage of entries | segments containing 64 entries (2,560 B), 0.125 B of segment pointer per entry | ankerl’s segments hold a power of two of elements, confined within 4,096 B |
| Bucket | 8 B; a power of two of them, filled to the load factor 0.8 | ankerl’s standard bucket; `node_map_sizing.hpp` |
| Bucket share per entry | 14.4 B on average across a doubling cycle, 10-20 B at any fixed size | 8 B divided by the load, with load ranging from 0.4 to 0.8 |
| Full network (983,424,620 nodes) | 2^31 buckets (16 GiB), load 0.458 | 2^30 buckets hold 858,993,472 entries |
| Full network’s node map | 52.75 GiB: 36.64 GiB for entries, 0.11 GiB for segment pointers, 16 GiB for buckets | the rows above |

The earlier engine kept two maps composed of 24-byte entries, each backed by an independent bucket array: 76.1 GiB for the full network. One map reduces that footprint by 23.3 GiB, equating to 25.5 B per node (computed; for `-small` the same calculation yields 18.1 B per node, and the commit merging the maps measured 19.8 B). Today’s engine would consequently hold the full network in roughly 200 GiB, with the node map accounting for approximately a quarter (computed: 223.7 GiB measured on the earlier engine, less 23.3 GiB). An adjacency set holds one neighbour within its entry, up to 128 within a dedicated block (8 B per neighbour, the block’s capacity a power of two), and additional neighbours in a hash set; this storage is not included in the 52.75 GiB.

### The Bucket Step and the Cap

The bucket array doubles when the entries exceed 0.8 times the buckets. At 2^31 buckets it holds 1,717,986,944 entries, so entry **1,717,986,945** triggers a doubling to 2^32 buckets: the 32 GiB array replaces the 16 GiB one, which ankerl frees first, causing the network to increase by **16 GiB at once** (computed; `src/test/test_node_map.cpp` pins the counts). The full network is 734,562,324 nodes below the step. The doubling places every entry again, within the insertion that triggers it, which holds both adjacency locks during the operation; its duration at that size was not measured.

`.node-map-load-factor` raises the factor from 2^31 buckets onward up to 0.95, and leaves 0.8 below. At 0.9 the step comes at entry 1,932,735,233, at 0.95 at entry 2,040,109,441 – 214,748,288 and 322,122,496 entries later (computed). A lookup in such a full map requires probing more buckets.

The node map cannot grow past 2^32 = 4,294,967,296 entries: ankerl’s standard bucket indexes its entry using 32 bits. At 2^32 buckets, every bucket is utilized, so starting from 3,435,973,888 entries, the load exceeds 0.8 and increases toward 1.0, and entry 4,294,967,297 throws `std::overflow_error` (computed from `unordered_dense.h`).

A node that never enters the node map incurs no cost with respect to the step. Value nodes are specified to live in a dense array of their own (next section); every other kind of node is stored in the map.

### One Node of Each Kind

The cost of one additional node (measured unless marked computed). Beneath the step, introducing a node into the full network costs no bucket share: its 2^31 buckets are already allocated.

| Node | Bytes | Basis |
| --- | --- | --- |
| Named node, a name up to 15 characters long | 202-225 B | 200,000 to 2 million names, written during the session or loaded; entry, both name maps, the name’s string |
| Named node, a name 36-46 / 62 / 100 characters long | 253 / 272 / 316-319 B | a million names; a string beyond 15 characters requires its own heap block |
| A second name for a node, in a different language, identical text | 84-92 B | 200,000 and a million `.name` lines; two map entries, no new string |
| Fact as a load holds it, its subject and object with no other edges | 132 B | a million facts across two million loaded names, no bucket growth |
| Fact as a load holds it, five facts per subject | 178 B | a million facts, 200,000 subjects; includes a doubling of the buckets (16.8 B per fact) |
| Fact written via `fact()`, in the session that performs the write | 236-262 B; 361-383 B with fact-path stores enabled | 100,000 and a million facts; loaded from the saved file, the 132-178 B above |
| Node derived by `cmp`, in the run that derives it | 403-421 B (decimal), 330-340 B (binary) | 191,974 and 133,040 nodes derived from 20,000 and 5,000 comparisons |
| Value node, as specified | 70.4 B + its text | computed in the next section |

A graph comprising N facts over fresh names, introducing two new names per fact, grows by 773.6 B per fact between N = 100,000 and N = 1,000,000 when the stores are active, and by 647.9 B when they are inactive (measured; 871.6 B and 729.4 B with the allocator’s default).

## Value Nodes

A value node is specified to consist of the components listed below, none of which is a name or a fact ([Literals as Value Nodes](literals.md)); their sizes are computed.

| Part | Bytes | Basis |
| --- | --- | --- |
| Record in the value store | 16 B | text offset in 48 bits, text length in 32, datatype index and tag index in 24 each; one record per sequence number |
| Text | its lexical bytes | stored once in the store’s text arena |
| Content index | 22.4 B | a segmented set of value-node ids hashed across datatype, tag and text read from the store: 8 B id and 14.4 B of buckets; a map keyed by the text would instead require 24 B and 14.4 B, totalling 38.4 B |
| Adjacency | 32 B | two adjacency sets in a dense array indexed by the sequence number, no key, no bucket; a node map entry would instead take 40 B and a bucket share, averaging 54.4 B |
| **Together** | **70.4 B + text** | 92.8 B + text with a node-map entry |

The dense array prevents values from entering the node map, ensuring that no number of values can trigger the step or the cap. All descriptions across all languages would bring 0.06-0.17 billion distinct values (see [Labels and languages](#labels-and-languages)); within the node map, they would take the network from 4.11-4.33 billion entries to 4.17-4.51 billion, past the cap at the upper count (computed).

The requirement for a value’s text to be stored once and in a compact form – neither as a name nor as a structure composed of facts – avoids both drawbacks. As a name, a value with up to 15 characters would incur a cost of 202–225 B rather than roughly 85 B, a 100-character value would consume 316–319 B instead of approximately 170 B (measured names versus the computed value node), and it would collide with any entity sharing the same spelling. As a character list, a text of n characters may require up to n cons cells, each a fact of 132–178 B: a 10-character text could occupy up to 1.3–1.8 KB, fewer when lists share cells (computed). [D5](decisions.md#d5)’s character list thus exists solely upon request.

## Expansion on Request

### Nodes per Distinct Value

An expansion is ordinary graph structure, so its cost can be measured today by writing the specified shapes as facts ([Expanding Values on Request](expansion.md), [Dates and Intervals](dates.md), [Numbers](numbers.md)). The table counts the number of nodes added by one additional distinct value, `.stat` before and after: “marginal” over 20,000 values of identical type, “first” for 1,000 values in a graph lacking them (measured; years: 1,000 years added to the span from year 1 to year 1000). Each count includes the value node, represented here by a named atom serving as its proxy, and the expansion statement.

| Value and shape | Decimal: marginal / first | Binary: marginal / first |
| --- | --- | --- |
| Integer below 10^7: `V zelph:value &N` | 4.77 / 6.54 | 8.60 / 14.38 |
| Negative integer: `V zelph:value (neg zint &N)` | 5.77 / 7.54 | 9.60 / 15.38 |
| Decimal: `V zelph:value (&M dec &K)`, M of 4-6 digits | 4.74 / 6.53 | 6.57 / 12.13 |
| Day, 1800-2025: `V zelph:interval (&D interval &D)` | 4.06 / 5.64 | 5.29 / 10.59 |
| Day before 4713 BC: `((neg zint &D) interval (neg zint &D))` | 6.15 / 7.83 | 8.46 / 14.13 |
| Instant: `((&D at &S) interval (&D at &S))` | 5.60 / 9.07 | 6.79 / 16.91 |
| Year: `(&F interval &L)`, two day numbers | 7.52 / 7.52 | 15.43 / 15.44 |

Counting each node, including the value node, at the 132-178 B of a fact, a day costs 0.5-1.0 KB under decimal and 0.7-1.9 KB under binary, a year 1.0-1.3 KB and 2.0-2.7 KB (computed). The decimal substrate proves less costly across all types; `integer-arithmetic`, `primes`, `math`, and the modules built upon them import `binary-arithmetic` unless a different substrate was imported prior.

### Distinct Values, Not Occurrences

A value node is identified by its content ([D14](decisions.md#d14)), and a full value node of Wikidata by the hash of its content, meaning a value appearing a million times is a single node with one expansion; each occurrence incurs a fact. The difference is large for dates and small for quantities. In main snaks, the block sample counts 72.4 million time values (1.05 billion when references are included), compared to 428,354 full value nodes of times and 302,217 distinct time literals on the Query Service; quantities involve 58.9 million full value nodes, while coordinates amount to 12.8 million (Query Service, 4 November 2026). Expanding each time value costs at most 428,354 × 7.5 nodes under decimal, roughly 3.2 million (6.6 million under binary; computed from the year row); expanding every quantity amount results in 220-326 million nodes, 27-54 GiB (computed from 58.9 million values and the integer and decimal rows above).

### What Drives the Cost

Every expanded numeral adds cons cells, and each cell is a fact whose subject is one of the substrate’s digit atoms – ten in total under decimal, two under binary – thereby granting those atoms one adjacency entry per cell. Cells are shared: a numeral reuses the cells of every numeral sharing identical higher digits, which explains why the marginal counts are lower than the first ones and why the cost per value falls as values accumulate. Today’s engine answers a fully bound condition with a single probe rather than traversing an atom’s adjacency (see [Comparisons](#comparisons)), meaning a larger digit hub does not slow a comparison down. The stdlib’s rules still incur a cost once per run for the cells present: 20,000 unrelated numerals (approximately 66,000 cells under decimal, 164,000 under binary) added roughly 120 million instructions to the run under decimal and 460 million under binary, regardless of the number of comparisons (measured).

### Doubles

`xsd:double` and `xsd:float` expand to their exact values ([D11](decisions.md#d11)), and an exact double contains significantly more digits than its textual representation. Constructed manually on a blank graph (measured; each count incorporates roughly five nodes for the initial application of `zelph:value` and `dec`):

| Lexical form | Significant digits: text / exact | Nodes, decimal: exact / text | Nodes, binary: exact / text |
| --- | --- | --- | --- |
| `0.1` | 1 / 55 | 63 / 7 | 191 / 7 |
| `52.516666666667` | 14 / 49 | 58 / 22 | 173 / 55 |
| `13.383333333333` | 14 / 51 | 59 / 21 | 178 / 50 |
| `1.0E-7` | 2 / 66 | 75 / – | 228 / – |
| `2.2250738585072009E-308` | 17 / 767 | 777 / – | 2,562 / – |

Across the double values in the block sample, an exact double averages approximately 52 significant digits, compared to 8.3 in Wikidata’s text (analysis conducted on 4 November 2026). A double is therefore expanded solely for the values that a request selects.

### Throughput of an Expansion Pass

An expander written in Janet that writes one expansion per value, using `zelph/number` and `zelph/fact` on the primary thread, achieved throughput of 149,000 to 174,000 day-precision dates per second under decimal and 24,000 to 25,000 under binary, along with 137,000 and 20,000 integers per second (measured, 50,000 distinct values, fact-path stores enabled and disabled). At these speeds, every time value requires less than 3 s and every quantity amount takes roughly 7 minutes under decimal and 48 under binary (computed). The analysis conducted on 4 November 2026 measured approximately 60,000 and 9,000 expansions per second on the earlier engine.

## Labels and Languages

**English labels** incur the same cost in an RDF import as they do in the JSON import ([D7](decisions.md#d7)): each label becomes the node’s name in its language, so an RDF import writes identical name entries as the JSON import – the full network holds 83,261,799 `en` names (`binaries.md`) – with no value node and no fact. Wikidata’s RDF writes every label three times (`rdfs:label`, `skos:prefLabel`, `schema:name`); all three give the same name. The mapping adds no additional byte for them (computed).

**`mul` as a language of its own** ([D13](decisions.md#d13)) adds a second name to every entity that possesses both an `en` and a `mul` label: 9.15 million entities in the block sample, 97.6 % of which share the same text, so the string pool hardly grows. At 84-92 B per second name, this incurs a cost of 0.72-0.79 GiB (computed). Entities that have only a `mul` label retain a single name, expressed in `mul` rather than `en`.

**Descriptions and aliases** never become names ([D7](decisions.md#d7)); each would be a value node bearing its tag and a fact. Descriptions across all languages amount to 3.240 billion (95 %: 3.129-3.351 billion; block sample, 4,800 blocks pooled) with 0.058-0.173 billion distinct texts. On the full network, their facts alone take the node map to 4.11-4.33 billion entries, past the step and reaching the cap of 4.29 billion, and incur a cost of 383-555 GiB, plus 3.8-11.3 GiB for value nodes, their texts, and the 16 GiB step (computed). That is why the default import includes only `en` and `mul` labels and nothing else ([D12](decisions.md#d12)). Aliases were not counted.

**Labels answered from names, in contrast to labels stored as statements.** The engine is specified to answer `X rdfs:label L` from the name maps ([D16](decisions.md#d16)), meaning a rule that reads labels costs no node and no byte; a value is materialized solely when a consequence needs it ([Labels, Names and Languages](names.md)). Storing each English label as a statement `S rdfs:label V` would instead incur 83,261,799 facts and value nodes: 10.2-13.8 GiB for the facts, 5.5 GiB for the value nodes, and the texts, 79 MiB per byte of their mean length (computed).

**Many label languages.** Today’s networks carry three or four naming languages; Wikidata encompasses roughly 564 label languages (the analysis, block sample). When a node is removed, each language’s map is inspected (`zelph_names.cpp`), resulting in a network that imports all label languages incurring approximately 564 probes per removed node (computed).

## Comparisons

### What the Specification Changes

Nothing in the stdlib’s `cmp`, `<`, `>`, `==` or `(A cmp B) = X`: dates and numbers expand to stdlib numbers, and comparisons remain unchanged ([Numbers](numbers.md)). What the specification changes is the volume. Check 1 evaluates roughly 4.0 million pairs of birth and death dates, Check 2 at most 4.2×10^8 citations ([Three Checks on Wikidata](checks.md)). A data check may compare either via `cmp`, a computation whose result the graph preserves, or via the comparison condition `A zelph:lt B`, which the engine answers like `!=` and which derives nothing ([D36](decisions.md#d36), [Numeric Comparisons](../logic.md#numeric-comparisons)).

### What a Comparison Costs Today

Measured: 7-digit operands drawn from [2,300,000, 2,500,000), N pairs, one `cmp` fact per pair (`cI is (&a cmp &b)`) or two facts per pair along with the rule `(P a A, P b B, A zelph:lt B) => (P less yes)`; a sequential run; with and without 20,000 unrelated 7-digit numerals in the graph. Instructions are those of the run; “the comparison alone” is the difference between the same rule with `A zelph:ne B` (which holds for every pair) and without it.

| Substrate | N | Extra numerals | `cmp`: ms per pair | `cmp`: instructions per pair | `cmp`: nodes per pair | Rule with `zelph:lt`: instructions per pair | The comparison alone |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| decimal | 1,000 | – | 0.235 | 2.93 M | 10.73 | 67 K | 12.1 K |
| decimal | 1,000 | 20,000 | 0.237 | 3.06 M | 10.73 | 183 K | 11.1 K |
| decimal | 4,000 | – | 0.254 | 2.96 M | 10.10 | 49 K | 11.3 K |
| decimal | 4,000 | 20,000 | 0.260 | 2.99 M | 10.10 | 79 K | 10.9 K |
| binary | 1,000 | – | 0.653 | 8.01 M | 28.92 | 117 K | 28.0 K |
| binary | 1,000 | 20,000 | 0.777 | 8.54 M | 28.92 | 586 K | 29.5 K |
| binary | 4,000 | – | 0.772 | 8.29 M | 26.93 | 91 K | 25.7 K |
| binary | 4,000 | 20,000 | 0.808 | 8.46 M | 26.93 | 205 K | 25.7 K |

The rule with the condition derives precisely one fact per pair meeting its criteria – its conclusion – and nothing beyond. A comparison based on the condition takes 11–12 thousand instructions in decimal mode and 26–30 thousand in binary, roughly 1 µs and 2 µs at the 12.3–12.5 billion instructions per second achieved in these runs; a comparison using `cmp` takes 0.24–0.26 ms and results in 10–11 nodes under decimal, 0.65–0.81 ms and 27–29 nodes under binary. Both expense measures scale linearly with N and are nearly unaffected by the graph structure: adding 20,000 more numerals increases `cmp`’s instruction count by 1–4% under decimal, 2–7% under binary, and has no impact on the condition’s intrinsic cost. What grows with the numerals is the run’s fixed share ([What drives the cost](#what-drives-the-cost)), distributed across fewer pairs at N = 1,000.

### What the Figures Rest On

Two properties of today’s engine carry these figures.

- **A fully bound condition is answered by a single probe.** A condition with a fixed relation and all its subject and objects fully bound is answered by an exact-triple lookup (`src/lib/network/unification.cpp`, counted as `exact_probes` in `.prof`; [Measurement Methodology](../internals/measurement.md), “A third recorded change”). The digit tests within `cmp` constitute such conditions, so a comparison no longer walks the adjacency of a digit atom, which grows with each numeral in the graph.
- **The record of statements with several objects is per predicate.** A ground fact featuring two or more objects, or a merge of two statements, marks its predicate (`zelph_impl.hpp`, `.stat`); a condition over a marked predicate is read through the statement’s atoms rather than through a single lookup. Measured using 1,000 decimal `cmp` pairs: a fact `x q a b` with an unrelated predicate increases the run’s instructions by 3.7 %; the same fact with `cons`, the predicate associated with every numeral cell, multiplies instructions by 14.8 and time by 18.9; a network loaded from a file that does not record predicates, treating every predicate as marked, multiplies instructions by 15.0 and time by 21. Verdicts and derived nodes remain identical across all four runs.

Every published `.bin` file precedes the record, meaning a check run on any of them today pays the factor of the last measurement; the networks are regenerated for the release, and a network constructed by today’s engine retains the record upon saving (one loaded from a file without it does not recover it through a save). The specification’s conversion of collections ([D27](decisions.md#d27)) uses a merge entry of its own that marks no predicate.

### The Checks at Full Scale

Both checks were measured as [Three Checks on Wikidata](checks.md) writes them, including negated conditions on deprecated ranks and on dates marked “circa,” across full values constructed manually: `S fv:P V` and `V zelph:interval (F interval L)` at day precision, 20,000 items with birth and death dates for check 1, and 20,000 citations drawn from 10,000 dated works for check 2. No statement carries a rank or a qualifier in this context, so each negation holds. The `cmp` form writes `(L cmp G)` from the identical conditions and derives the violation from `L < G`; both forms find the same violations. Per pair, measured:

| Check, form | Decimal: µs / instructions / nodes per pair | Binary: µs / instructions / nodes per pair |
| --- | --- | --- |
| 1, `zelph:lt` | 18.2 / 187 K / 0.49 | 29.5 / 222 K / 0.49 |
| 1, `cmp` | 417 / 3.81 M / 10.12 | 1,001 / 9.53 M / 23.50 |
| 2, `zelph:lt` | 16.1 / 160 K / 0.51 | 24.6 / 185 K / 0.51 |
| 2, `cmp` | 352 / 3.51 M / 9.63 | 911 / 9.01 M / 21.81 |

At full scale (computed, single-threaded, decimal unless otherwise stated): the pair counts represent the block sample’s estimate for check 1 (approximately 4.0 million items possessing both a date of birth and a date of death) and serve as an upper bound for check 2 (no more than 4.2×10^8 statements of P2860 across eight slices of the dump, not all of which pertain to works with a date); the cost per pair corresponds to that of the 20,000-pair graph, hence the latencies associated with a 200 GiB graph are excluded.

| Check | With `zelph:lt` | With `cmp` |
| --- | --- | --- |
| 1 | 7.5×10^11 instructions, roughly a minute (binary: 2 minutes); one fact per violation | 1.5×10^13 instructions, 28 minutes; 40.5 million derived nodes, 15.2-15.9 GiB (binary: 67 minutes, 94 million nodes, 29-30 GiB) |
| 2 | 6.7×10^13 instructions, 1.9 hours (binary: 2.9 hours); one fact per violation | 1.5×10^15 instructions, 41 hours, 4.0 billion derived nodes: at this upper bound the network would need 5.0 billion entries, past the cap of the node map |

The comparisons themselves constitute a minor portion of the condition’s cost: at 11.3 thousand instructions each, they consume 4 s during check 1 and 6.4 minutes during check 2. The remainder arises from the join of the statements, dates, and intervals, along with the negated conditions, which more than double it: without these, the rules took 90 and 72 thousand instructions per pair under decimal. `cmp` shares the work of a date pair that appears twice, as its comparison term is a single node, yet the number of distinct date pairs among the citations was not counted, so the `cmp` figures stay upper bounds. The split form of a day bound, which would reduce the number of nodes left by `cmp`, remains an [open decision](open-decisions.md#day-numbers-in-two-parts).

Check 3 computes rather than compares. Measured using the standard library’s trial division (`stdlib/primes.zph`) under decimal, for the integers 100,000-100,039: 1,242 divisions, 747 derived nodes and 473 million instructions per division, 74 ms per division in a single run at a load of 12.8. Checking every integer up to 121,232 requires 3,579,464 divisions when using the standard library’s candidates 2, 3, 4, … and 945,977 if only primes are tried as divisors; using prime divisors results in 0.71 billion derived nodes, 265-277 GiB and approximately 10 hours, and up to 10,000 it is 43,753 divisions, 12-13 GiB and half an hour (computed; the durations derived from the instructions per division at 12.3 billion instructions per second, the rate that the comparison runs reached on one performance core, where the 74 ms of the loaded run would give about twice the duration). The bound of check 3 ([D26](decisions.md#d26)) sets its cost.

## Wikidata Scenarios

Each row adds one import to the current full network (983,424,620 nodes, 734,562,324 beneath the step). Each figure is computed from the costs established previously – measured: a fact 132-178 B, a named node 202-225 B with a short name and 253 B with a 36-character hash, a second name 84-92 B; computed: a value node 70.4 B and its associated text – along with the count listed in the basis column.

| Import | Node-map entries added | RAM added | Basis |
| --- | --- | --- | --- |
| The default import: labels in `en` and `mul`, no literal values | 112,763 | 16-20 MiB, and 0.72-0.79 GiB for the second names in `mul` | 87,746 claims with property, lexeme, form, sense or EntitySchema values and 25,017 new named nodes (Query Service); 9.15 million entities with `en` and `mul` labels (sample) |
| Dates of birth, death and publication as `S fv:P V`, with full values | 66.0-68.8 million | 8.2-11.5 GiB | 11.65 million P569 and P570 and 51.80 million P577 statements (sample); at most 428,354 time values (Query Service), each a node with five facts and one value node; the upper figures include its expansion |
| All quantities with full values | 388-515 million | 58.5-103 GiB | 152-162 million statements (sample); at most 58.9 million quantity values (Query Service), each a node with three facts and one value node, or five and three with bounds |
| The same, every amount expanded | 608-841 million | 85.5-157 GiB | the row above and the expansions of 58.9 million amounts; the network reaches 1.59-1.83 billion entries, on either side of the step |
| All main-snak literal values | at least 874.7 million | 107-145 GiB, 16 GiB for the step, and the value nodes | 874.7 million statements (sample); the network reaches 1.858 billion entries on the facts alone, past the step |
| Descriptions in all languages | 3.129-3.351 billion | 383-555 GiB, 3.8-11.3 GiB of value nodes, their texts, the step | see [Labels and languages](#labels-and-languages) |
| All English labels as label statements | 83.3 million | 15.7-19.3 GiB and the texts | 83,261,799 `en` names (`binaries.md`); answered from the names: nothing |

Only imports of every value of a kind arrive at the step: all main-snak literal values, all descriptions, all quantities with every amount expanded. The default import adds 0.01 % to the node map; [D2](decisions.md#d2)’s requests and [D12](decisions.md#d12)’s parameters ensure the network remains under the step. Nor would every triple from Wikidata become a fact and therefore a node: of the 15.03 billion triples the Query Service held in July 2023 (Phabricator task T337021), 0.80 billion were `rdfs:label` triples, which become names, while each statement, reference, and value IRI adds a named node of its own, so an import of everything would hold approximately 14.2 billion facts and at least 1.7 billion statement nodes (the analysis, computed from T337021 and the 1.744 billion statements of the block sample) – more than three times the node map’s cap.

## Risks

**A predicate-selective RDF reader is unable to restrict the full values it keeps in a single traversal.** Wikibase writes the description of a full value exclusively at the moment its dump writer first encounters that value, storing it in a lossy bag of 16^5 slots (`ComplexValueRdfHelper.php`, `HashDedupeBag.php`), which frequently occurs after a statement from a different property: in a 100 MB slice, 7.0 % of the quantity links point to a description that came after another property’s link (analysis conducted on 4 November 2026). Consequently, a reader that keeps only selected properties must therefore keep every description in one pass, amounting to roughly 225 million triples for the 58.9 million quantity values, or lose some; a pipe does not permit a second pass. For full values read selectively from Wikidata’s RDF, [D2](decisions.md#d2)’s promise that costs arise only when a value is required therefore applies to the expansions and not to the descriptions that are kept; the value pass of the JSON importer keeps only the values of the listed properties ([Two Ways In](wikidata-mapping.md#two-ways-in)).

**The bucket step and the cap.** Costs are not linear near 1.72 billion entries: the entry that crosses the step introduces a 16 GiB jump immediately and places every entry again under both adjacency locks, and 4.29 billion entries is a hard limit. Only imports of everything of a given kind attain either threshold; `.node-map-load-factor` defers the step by up to 322 million entries, at the price of slower lookups, and only a wider bucket type could raise the cap.

**Merges and what they deactivate.** A name merge in today’s engine keeps the fact-path stores armed, unless a variable or a set pattern constructed outside a rule is involved. Every merge still clears the predicate indexes and the cache of fact structures, and turns the `.pidx` side files of a loaded network off until the next load of a whole file; a merge between two statements also marks the survivor’s predicate, with the cost measured above (`zelph_names.cpp`, `zelph_caches.cpp`, `zelph_index.cpp`). One merge into a large loaded network therefore costs the next path condition the index it would have read; the specified conversion of collections uses a merge entry of its own, which neither disarms the stores nor marks a predicate ([Collections and Lists](collections.md)).
