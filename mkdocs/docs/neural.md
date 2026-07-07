# Neural Networks in the Graph

zelph can embed neural networks directly inside its semantic network. There is no parallel world, no export/import boundary, no separate tensor runtime with its own identifiers: **the neurons are ordinary graph nodes, and the synapses are ordinary graph edges carrying weights**. A sub-graph can be compiled into a feed-forward network on demand, trained, evaluated, and written back — and inference rules can consult such a network as a condition, using the `≈` operator.

This page covers the full stack: the weighted-edge substrate, the compiled-network cache, the Janet API, the `≈` rule condition, the helper library [`stdlib/nn.zph`](https://github.com/acrion/zelph/blob/main/stdlib/nn.zph), and a complete proof of concept on real Wikidata data ([`stdlib/examples/neural/nn-wikidata-demo.zph`](https://github.com/acrion/zelph/blob/main/stdlib/examples/neural/nn-wikidata-demo.zph)).

The design goal is a form of [neuro-symbolic AI](https://en.wikipedia.org/wiki/Neuro-symbolic_AI): symbolic reasoning (rules, unification, negation) and sub-symbolic learning (weighted connections, gradient descent) operating on the _same_ structures. A prediction made by a network does not need to be translated into the knowledge representation — it already _is_ knowledge representation, and it flows back into the graph as a fact probability.

## A Two-Minute Neural Network Primer

If you are new to neural networks, this is the minimal vocabulary needed for this page. Each term links to a starting point for deeper reading; none of the mathematics beyond this summary is required to _use_ the feature.

- A [feed-forward network](https://en.wikipedia.org/wiki/Feedforward_neural_network) organizes _neurons_ into _layers_. Each neuron computes a weighted sum of the previous layer's activations. The _weights_ are the learnable parameters.
- Hidden layers in zelph apply the [ReLU](<https://en.wikipedia.org/wiki/Rectifier_(neural_networks)>) activation (`max(0, x)`); the output layer is linear (no activation).
- Training uses [stochastic gradient descent](https://en.wikipedia.org/wiki/Stochastic_gradient_descent) (SGD): for one sample, compute the [squared-error loss](https://en.wikipedia.org/wiki/Mean_squared_error) between output and target, then adjust the weights slightly against the error gradient, computed via [backpropagation](https://en.wikipedia.org/wiki/Backpropagation).
- Inputs and targets are encoded as [one-hot / multi-hot](https://en.wikipedia.org/wiki/One-hot) vectors: each neuron corresponds to one entity, and a sample activates exactly the entities it mentions.
- The demo task on this page is [link prediction](https://en.wikipedia.org/wiki/Link_prediction): given a subject and a predicate, predict the most plausible object.

## The Substrate: Raw Weighted Edges

Internally, zelph stores edge weights in a sparse side table keyed by a hash of the directed edge. Nodes and edges that carry no weight cost **nothing** — the neural substrate adds zero memory overhead to the millions of nodes of, say, a Wikidata import.

This weight store is _shared_ with an older zelph concept: **fact probabilities**. A fact's probability has always been stored on the edge from the fact node to its predicate node (range `[0, 1]`, absent entry = 1). Synapse weights use the same store on neuron-to-neuron edges, without the range constraint. In other words, fact probabilities are a _constrained view_ of the general weight store — this unification is what later allows a network's confidence to become a deduced fact's probability with no translation step.

Raw weighted edges are created without any predicate. They therefore have none of the topological signature of a fact (see [Internal Representation of facts](index.md#internal-representation-of-facts)) and are **completely invisible to the reasoning engine**:

```
n1 ~ neuron
n2 ~ neuron
%(zelph/nn-connect "n1" "n2" 0.7)
n1 _P _O
```

The query returns only `n1 ~ neuron` — the synapse is not a fact.

The low-level API:

- **`(zelph/nn-connect from to &opt weight)`** — create a raw weighted edge (default weight 1), creating the nodes if needed.
- **`(zelph/weight from to)`** — read the weight of an edge, or `nil` if the edge does not exist. An existing edge without a stored weight yields 1.
- **`(zelph/set-weight from to w)`** — overwrite the weight of an existing edge.

## Layers Are Sets, Neurons Are Nodes

A _layer_ is simply a node, and its neurons are the subjects of ordinary `(neuron in layer)` membership facts — the same `in` (PartOf) relation used for [sets](index.md#braces-sets):

```
x1 in Lin
x2 in Lin
y  in Lout
```

The canonical neuron order within a layer is ascending node ID (i.e. creation order), which makes vector layouts deterministic.

Because neurons are ordinary nodes, _anything_ can be a neuron: an entity, a predicate, a digit, a fact node. This is the crucial difference from external ML pipelines — there is no identifier mapping between "the entity Q183" and "input neuron 17". They are the same node.

## Compiled Networks: A Discardable Cache

The graph is deliberately not evaluated edge-by-edge during a forward pass — that would be far too slow. Instead, **`(zelph/nn-compile layers)`** takes an array of layer nodes (input first, output last) and compiles a dense snapshot: weight matrices plus a _mask_ recording which synapses actually exist in the graph. It returns an integer handle for subsequent calls.

The philosophy mirrors zelph's predicate index (`.pidx` files): **the graph is the single source of truth, and the compiled network is a discardable cache.** Handles are session-scoped and vanish on `.new`. Three consequences:

1. **Sparsity is preserved.** Only synapses present in the graph are trainable; training can never create a connection that the graph does not contain. Absent synapses stay absent.
2. **Training happens on the cache.** `zelph/nn-train` and `zelph/nn-train-nodes` update the compiled matrices, not the graph.
3. **`(zelph/nn-write-back handle)` transfers the trained weights back into the graph's weight store.** Only then do they survive `.save`, get picked up by future `zelph/nn-compile` calls — and, importantly, become visible to `≈` rule conditions, which always compile from the graph.

**If you train a network and skip `zelph/nn-write-back`, the graph — and therefore every `≈` condition — still sees the untrained weights.** The helper `nn/link-predictor` (see below) writes back automatically.

The remaining compiled-network API:

- **`(zelph/nn-nodes handle layer)`** — the neurons of a compiled layer in index order; this order defines the meaning of plain input/output vectors.
- **`(zelph/nn-eval handle inputs)`** — forward pass with plain number vectors (in `zelph/nn-nodes` order). Hidden layers use ReLU, the output layer is linear.
- **`(zelph/nn-train handle inputs targets &opt learning-rate)`** — one SGD step on one sample; returns the loss (`0.5 · Σ error²`) _before_ the update. Learning rate defaults to 0.01.
- **`(zelph/nn-connect-layers from-layer to-layer &opt scale seed)`** — wiring helper: creates raw synapses between _all_ members of two layers, with uniform random weights in `[-scale, scale]` (`scale` defaults to 0.1; `scale 0` gives exact zeros; `seed` defaults to 42 for reproducibility). Existing edges are left untouched, so the call is idempotent and re-wiring never destroys trained weights.

## Node-Addressed Training

Plain vectors are inconvenient when the training data _is_ the graph. The node-addressed API lets you address neurons by their graph node:

- **`(zelph/nn-train-nodes handle inputs targets &opt learning-rate)`** — one SGD step. `inputs`/`targets` are arrays whose elements are nodes (activation 1) or `[node activation]` pairs; all other neurons are 0 (multi-hot encoding). A typical call encodes one fact: inputs `[S P]`, target `[O]`.
- **`(zelph/nn-eval-nodes handle inputs &opt top-k)`** — forward pass; returns `[node score]` tuples for the output layer, sorted by descending score, optionally limited to the top k.

Two details worth knowing:

- **Implicit negative sampling.** A one-hot target sets the correct object to 1 and _every other output neuron to 0_. Each training step therefore pushes all wrong answers down while pulling the right one up — no explicit negative samples are needed.
- **Graded activations.** `[node 0.5]` feeds a quantitative value instead of a binary flag, e.g. edge weights of another network, or degrees of confidence.

Nodes that are not members of the addressed layer are rejected with an error rather than silently ignored — a deliberate choice, since silently dropping an input would corrupt training data without any signal.

## Declaring a Network in the Graph

Everything so far used session handles. To make a network addressable _from rules_, its architecture must itself be stored in the graph, as an ordinary fact:

```
gnet nn-layers <GIn GOut>
```

This reads: the network `gnet` consists of the layers `GIn` (input) and `GOut` (output) — an ordinary cons-list, input layer first. Deeper networks list hidden layers in between.

The marker predicates `nn` and `nn-layers` are ordinary nodes registered in the language `"zelph"`. They are deliberately **not** core nodes: core node IDs are positional, so introducing new ones would break every existing `.bin` file. A practical consequence: if your current `.lang` is not `zelph` (e.g. `wikidata`), writing `nn-layers` literally would create an unrelated node in that language — use the helper `nn/declare` (below) or `(zelph/resolve "nn-layers" "zelph")` instead.

## Neural Rule Conditions: `≈`

The centerpiece: a rule condition that consults a network instead of (or in addition to) matching facts.

```
(A demo-country yes, A P30 X, ≈geo(A P30 X)) => (A P30-verified X)
```

### Syntax and desugaring

`≈net(pattern)` desugars to **`(zelph/approx pattern "net")`**, which creates the _tag fact_ `(pattern nn net)` in the graph and returns that tag fact. The tag fact — not the inner pattern — becomes the rule condition. This distinction matters: fact creation is idempotent, so if the pattern itself were the condition, an `≈` condition and an ordinary positive condition on the _same_ pattern (as in the rule above) would collapse into one node. Returning the tag fact keeps them distinct, and the reasoning engine recognizes a neural condition purely by its predicate `nn` — structurally analogous to how `!=` guards are recognized.

### What the engine does

When a rule is applied, `≈` conditions are scheduled _late_: the condition-ordering heuristic ranks them after all positive conditions and `!=` guards (which bind variables cheaply) but before negations. By the time an `≈` condition is evaluated, its subject and predicate are bound.

The engine then resolves the network: it reads the `(net nn-layers <...>)` fact, collects the layer members, and compiles the network from the graph — **lazily, with a per-input cache**. The cache is cleared on every new REPL input, so training performed between inputs is picked up automatically on the next `.run`.

The bound pattern components that are members of the input layer are activated (multi-hot), and a forward pass produces scores for the output layer. Two modes follow:

- **Guard mode** — the pattern's object is already bound: the condition succeeds iff the object's calibrated score exceeds 0.5. Typical use: _verify_ existing facts.
- **Generator mode** — the object is an unbound variable: the condition produces one variable binding per output neuron whose calibrated score exceeds 0.5. Typical use: _propose_ new facts.

### Calibration: clamp, not sigmoid

Scores are calibrated by clamping to `[0, 1]`. A sigmoid may look like the textbook choice, but it would be wrong here: training uses one-hot targets in `{0, 1}` under squared-error loss, so raw scores concentrate near 0 (negatives) and 1 (positives). A sigmoid would place the decision threshold at raw score 0 — which nearly every score passes after training — whereas clamping puts the boundary at raw 0.5, exactly between the two target values.

### Confidence becomes fact probability

The calibrated score is a _confidence_. It multiplies along the rule's binding path (several `≈` conditions compound), and when the rule fires, the deduction is created with this confidence as its **fact probability** — stored in the shared weight store on the fact-to-predicate edge, exactly like any other fact probability. Reading it back:

```
%(defn fact-conf [s p o]
   (if (zelph/exists s p o)
     (zelph/weight (zelph/fact s p o) p)   # fact is idempotent; existing probability is not touched
     nil))
```

If the deduced fact already exists, its stored probability is _not_ modified — a rule cannot silently upgrade established knowledge.

### Current restrictions (v1)

- The inner pattern must be a plain S-P-O triple with exactly one object.
- Subject and predicate must be bound when the condition is evaluated (the automatic ordering normally guarantees this).
- `≈` conditions are not supported inside `.prune-facts` / `.prune-nodes` patterns.

## The Helper Library: `stdlib/nn.zph`

The raw API is deliberately low-level. `nn.zph` provides idiomatic helpers (import once via `.import nn`):

| Function                                                                 | Purpose                                                                                                                             |
| :----------------------------------------------------------------------- | :---------------------------------------------------------------------------------------------------------------------------------- |
| `(nn/in-layer nodes layer)`                                              | Add nodes to a layer via `(node in layer)` facts. Idempotent.                                                                       |
| `(nn/declare net layers)`                                                | Create the `(net nn-layers <...>)` fact — the graph-level definition `≈` compiles from.                                             |
| `(nn/layers net)`                                                        | Read a network's layer nodes back from its `nn-layers` fact.                                                                        |
| `(nn/wire-dense net &opt scale seed)`                                    | Densely wire consecutive layers of a declared net (via `zelph/nn-connect-layers`).                                                  |
| `(nn/compile net)`                                                       | Compile a declared net; returns the handle.                                                                                         |
| `(nn/gather subjects preds)`                                             | Collect `(S P O)` triples from the graph as training samples. Read-only.                                                            |
| `(nn/train-triples handle samples &opt epochs lr)`                       | SGD over samples: input `{S, P}`, one-hot target `{O}`. Returns the last epoch's mean loss.                                         |
| `(nn/predict handle inputs &opt k)`                                      | Forward pass; top-k `[node score]` tuples.                                                                                          |
| `(nn/predict-names handle inputs &opt k lang)`                           | Like `nn/predict`, but with node names for easy printing.                                                                           |
| `(nn/link-predictor net subjects preds &named epochs lr extra-subjects)` | End-to-end: gather samples, build `<net>-in`/`<net>-out` layers, declare, wire, compile, train, **write back**. Returns the handle. |

`nn/link-predictor` is the one-call path from "facts in the graph" to "network usable from `≈` rules": because it writes the trained weights back, the graph-compiled network that `≈` sees is the trained one.

## Proof of Concept: Continent Prediction on Wikidata

[`stdlib/examples/neural/nn-wikidata-demo.zph`](https://github.com/acrion/zelph/blob/main/stdlib/examples/neural/nn-wikidata-demo.zph) demonstrates the full loop on real data. The task: learn which continent (property [P30](https://www.wikidata.org/wiki/Property:P30)) a country belongs to, from the P30 facts already present in a Wikidata dump — then use the network from rules to _verify_ existing facts and _propose_ missing ones.

While this demo uses Wikidata, nothing about the neural machinery is Wikidata-specific; the dump merely provides a large, real-world graph to learn from.

The script, step by step:

1. **Select the entities.** Countries are collected as direct instances (P31) of _country_ (Q6256) **or** _sovereign state_ (Q3624078) — many countries carry only the latter.
2. **Materialize the selection as facts** (`A demo-country yes`). This gives the rules a small, precise predicate to anchor on, instead of scanning the ~15 million P31 facts of the full dump.
3. **Train a link predictor:** `(nn/link-predictor "geo" countries [p30] :epochs 150 :lr 0.2)` — input layer: countries + the P30 predicate node, output layer: the continents observed in the training triples. On the demo dump: 145 countries, 118 P30 samples.
4. **Guard rule** — verify existing P30 facts through the network:

```
   (A demo-country yes, A P30 X, ≈geo(A P30 X)) => (A P30-verified X)
```

5. **Generator rule** — let the network propose continents, keep only proposals _not_ backed by an existing fact (note the interplay of `≈` and negation-as-failure):

```
   (A demo-country yes, ≈geo(A P30 X), ¬(A P30 X)) => (A P30-candidate X)
```

6. **Cluster hygiene.** Everything runs inside `.cluster nn-demo`, so the entire experiment — layers, synapses, rules, deductions — can be rolled back with a single `.cluster-drop nn-demo`, leaving the loaded dump untouched.
7. **Verification block.** After `.run`, the script queries the deduced facts and reads their confidences back from the weight store, closing the loop: network score → rule confidence → fact probability.

### Interpreting the results

On the demo dump, the run deduces 99 `P30-verified` facts with confidences between ~0.51 and 1.0, and 43 `P30-candidate` facts — all with confidence ≈ 0.527 pointing to Europe (Q46). The uniformity of the candidates is worth understanding, because it illustrates both what this simple architecture learns and where its limits are:

The input encoding is _identity-based_: each country is its own input neuron. The P30 predicate neuron is shared across **all** training samples, so its weights learn the _marginal distribution_ of continents in the training set (Europe dominates, thanks to the many historical European states in the dump). A country that has **no** P30 fact never receives a training signal on its own input neuron — its weights stay at their zero initialization. For such a country, the network's output is the P30 prior alone: ≈ 0.527 for Europe, just above the threshold. The candidates are therefore _prior-driven_, not country-specific — honest behavior for a model without generalizing features, and a useful baseline signal ("statistically, an unknown country is most likely European in this dataset").

Making predictions country-specific for unseen entities requires features that generalize — the classic next step being [knowledge graph embeddings](https://en.wikipedia.org/wiki/Knowledge_graph_embedding), where entities share a learned vector space instead of having isolated identity neurons. The substrate described on this page (weighted edges, layers as sets, graded activations) is designed to carry such architectures as well.

Two dump-related notes, so they are not mistaken for bugs: entities absent from the pruned demo dump's country selection (e.g. Q183/Germany, which carries only P31 Q3624078 in some prunings) are reported and skipped by the script; and evaluating a node that is not a member of the input layer raises an explicit error by design.

## Limitations and Outlook

The current implementation is a deliberate foundation, not a finished ML framework: networks have no bias terms, hidden layers are ReLU-only, the output is linear, features are identity-based, and `≈` supports plain S-P-O patterns only. What the foundation establishes is the _architectural_ claim: neurons as nodes, synapses as edges, training data gathered by reasoning queries, and network confidences flowing back into the graph as fact probabilities — all without leaving the semantic network.

## Appendix: Complete Session Log

The following is a complete, unedited session log of `stdlib/examples/neural/nn-wikidata-demo.zph` running against a pruned Wikidata dump (zelph 0.9.7), including the test-suite run: from loading the dump through training, both `≈` rules firing during `.run`, and the verification block reading confidences back from the graph.

```text
❯ zelph
zelph 0.9.7-dev
-- REPL mode - type .help for commands, .quit to exit --

zelph> .load /home/stefan/zelph/wikidata-20260309-all-pruned.bin
Auto-run has been disabled due to loading a large dataset.
Loading network from generic file /home/stefan/zelph/wikidata-20260309-all-pruned.bin...
Loading: left chunks=75, right chunks=75, nameOfNode chunks=21, nodeOfName chunks=21
...........................................................................
...........................................................................
.....................
.....................
String pool size after load: 20389119
Network loaded.
 Time needed for loading/importing: 0h0m52.905s
-- 52.906 s --
zelph-> .import nn
Importing file stdlib/nn.zph...
<function nn/link-predictor>
-- 16 ms --
zelph-> .import examples/neural/nn-wikidata-demo
Importing file stdlib/examples/neural/nn-wikidata-demo.zph...
Active cluster: nn-demo
countries found: 145
nn/link-predictor: 118 samples, final mean loss 0.10311
Q29999 (Kingdom of the Netherlands):
    Q49  0.434
    Q18  0.260
    Q15  0.156
Q962 (Q962):
    Q15  0.999
    Q48  0.001
    Q46  0.001
Q12536 (Abbasid Caliphate):
    Q15  0.624
    Q48  0.376
    Q5401  0.001
{((A  P30  X)  nn   geo ) (A  P30  X) (A  demo-country   yes )} => (A  P30-verified  X)
{((¬(A  P30  X))  nn   geo ) (¬(A  P30  X)) (A  demo-country   yes )} => (A  P30-candidate  X)
Starting reasoning with 24 worker threads.
--- Reasoning iteration 1 ---
( Q580188   P30-verified   Q48 ) ⇐ {(( Q580188   P30   Q48 )  nn   geo ) ( Q580188   P30   Q48 ) ( Q580188   demo-country   yes )}
( Q162192   P30-verified   Q18 ) ⇐ {(( Q162192   P30   Q18 )  nn   geo ) ( Q162192   P30   Q18 ) ( Q162192   demo-country   yes )}
( Q193152   P30-verified   Q46 ) ⇐ {(( Q193152   P30   Q46 )  nn   geo ) ( Q193152   P30   Q46 ) ( Q193152   demo-country   yes )}
( Q747314   P30-verified   Q15 ) ⇐ {(( Q747314   P30   Q15 )  nn   geo ) ( Q747314   P30   Q15 ) ( Q747314   demo-country   yes )}
( Q756617   P30-verified   Q49 ) ⇐ {(( Q756617   P30   Q49 )  nn   geo ) ( Q756617   P30   Q49 ) ( Q756617   demo-country   yes )}
( Q838261   P30-verified   Q46 ) ⇐ {(( Q838261   P30   Q46 )  nn   geo ) ( Q838261   P30   Q46 ) ( Q838261   demo-country   yes )}
( Q191077   P30-verified   Q46 ) ⇐ {(( Q191077   P30   Q46 )  nn   geo ) ( Q191077   P30   Q46 ) ( Q191077   demo-country   yes )}
( Q1078602   P30-verified   Q48 ) ⇐ {(( Q1078602   P30   Q48 )  nn   geo ) ( Q1078602   P30   Q48 ) ( Q1078602   demo-country   yes )}
( Q185488   P30-verified   Q46 ) ⇐ {(( Q185488   P30   Q46 )  nn   geo ) ( Q185488   P30   Q46 ) ( Q185488   demo-country   yes )}
( Q170468   P30-verified   Q15 ) ⇐ {(( Q170468   P30   Q15 )  nn   geo ) ( Q170468   P30   Q15 ) ( Q170468   demo-country   yes )}
( Q2578028   P30-verified   Q48 ) ⇐ {(( Q2578028   P30   Q48 )  nn   geo ) ( Q2578028   P30   Q48 ) ( Q2578028   demo-country   yes )}
( Q63158027   P30-verified   Q48 ) ⇐ {(( Q63158027   P30   Q48 )  nn   geo ) ( Q63158027   P30   Q48 ) ( Q63158027   demo-country   yes )}
( Q878319   P30-verified   Q46 ) ⇐ {(( Q878319   P30   Q46 )  nn   geo ) ( Q878319   P30   Q46 ) ( Q878319   demo-country   yes )}
( Q15864   P30-verified   Q46 ) ⇐ {(( Q15864   P30   Q46 )  nn   geo ) ( Q15864   P30   Q46 ) ( Q15864   demo-country   yes )}
( Q3892131   P30-verified   Q18 ) ⇐ {(( Q3892131   P30   Q18 )  nn   geo ) ( Q3892131   P30   Q18 ) ( Q3892131   demo-country   yes )}
( Q9903   P30-verified   Q48 ) ⇐ {(( Q9903   P30   Q48 )  nn   geo ) ( Q9903   P30   Q48 ) ( Q9903   demo-country   yes )}
( Q7233551   P30-verified   Q48 ) ⇐ {(( Q7233551   P30   Q48 )  nn   geo ) ( Q7233551   P30   Q48 ) ( Q7233551   demo-country   yes )}
( Q1649871   P30-verified   Q46 ) ⇐ {(( Q1649871   P30   Q46 )  nn   geo ) ( Q1649871   P30   Q46 ) ( Q1649871   demo-country   yes )}
( Q2369784   P30-verified   Q46 ) ⇐ {(( Q2369784   P30   Q46 )  nn   geo ) ( Q2369784   P30   Q46 ) ( Q2369784   demo-country   yes )}
( Q1415128   P30-verified   Q48 ) ⇐ {(( Q1415128   P30   Q48 )  nn   geo ) ( Q1415128   P30   Q48 ) ( Q1415128   demo-country   yes )}
( Q684030   P30-verified   Q46 ) ⇐ {(( Q684030   P30   Q46 )  nn   geo ) ( Q684030   P30   Q46 ) ( Q684030   demo-country   yes )}
( Q204920   P30-verified   Q46 ) ⇐ {(( Q204920   P30   Q46 )  nn   geo ) ( Q204920   P30   Q46 ) ( Q204920   demo-country   yes )}
( Q11774   P30-verified   Q48 ) ⇐ {(( Q11774   P30   Q48 )  nn   geo ) ( Q11774   P30   Q48 ) ( Q11774   demo-country   yes )}
( Q953432   P30-verified   Q46 ) ⇐ {(( Q953432   P30   Q46 )  nn   geo ) ( Q953432   P30   Q46 ) ( Q953432   demo-country   yes )}
( Q28846511   P30-verified   Q46 ) ⇐ {(( Q28846511   P30   Q46 )  nn   geo ) ( Q28846511   P30   Q46 ) ( Q28846511   demo-country   yes )}
( Q164079   P30-verified   Q46 ) ⇐ {(( Q164079   P30   Q46 )  nn   geo ) ( Q164079   P30   Q46 ) ( Q164079   demo-country   yes )}
( Q962   P30-verified   Q15 ) ⇐ {(( Q962   P30   Q15 )  nn   geo ) ( Q962   P30   Q15 ) ( Q962   demo-country   yes )}
( Q15102440   P30-verified   Q46 ) ⇐ {(( Q15102440   P30   Q46 )  nn   geo ) ( Q15102440   P30   Q46 ) ( Q15102440   demo-country   yes )}
( Q172579   P30-verified   Q46 ) ⇐ {(( Q172579   P30   Q46 )  nn   geo ) ( Q172579   P30   Q46 ) ( Q172579   demo-country   yes )}
( Q431731   P30-verified   Q15 ) ⇐ {(( Q431731   P30   Q15 )  nn   geo ) ( Q431731   P30   Q15 ) ( Q431731   demo-country   yes )}
( Q189988   P30-verified   Q15 ) ⇐ {(( Q189988   P30   Q15 )  nn   geo ) ( Q189988   P30   Q15 ) ( Q189988   demo-country   yes )}
( Q2273304   P30-verified   Q46 ) ⇐ {(( Q2273304   P30   Q46 )  nn   geo ) ( Q2273304   P30   Q46 ) ( Q2273304   demo-country   yes )}
( Q2899771   P30-verified   Q46 ) ⇐ {(( Q2899771   P30   Q46 )  nn   geo ) ( Q2899771   P30   Q46 ) ( Q2899771   demo-country   yes )}
( Q912052   P30-verified   Q48 ) ⇐ {(( Q912052   P30   Q48 )  nn   geo ) ( Q912052   P30   Q48 ) ( Q912052   demo-country   yes )}
( Q176495   P30-verified   Q46 ) ⇐ {(( Q176495   P30   Q46 )  nn   geo ) ( Q176495   P30   Q46 ) ( Q176495   demo-country   yes )}
( Q2597352   P30-verified   Q46 ) ⇐ {(( Q2597352   P30   Q46 )  nn   geo ) ( Q2597352   P30   Q46 ) ( Q2597352   demo-country   yes )}
( Q599613   P30-verified   Q46 ) ⇐ {(( Q599613   P30   Q46 )  nn   geo ) ( Q599613   P30   Q46 ) ( Q599613   demo-country   yes )}
( Q736727   P30-verified   Q46 ) ⇐ {(( Q736727   P30   Q46 )  nn   geo ) ( Q736727   P30   Q46 ) ( Q736727   demo-country   yes )}
( Q207521   P30-verified   Q15 ) ⇐ {(( Q207521   P30   Q15 )  nn   geo ) ( Q207521   P30   Q15 ) ( Q207521   demo-country   yes )}
( Q190025   P30-verified   Q27611 ) ⇐ {(( Q190025   P30   Q27611 )  nn   geo ) ( Q190025   P30   Q27611 ) ( Q190025   demo-country   yes )}
( Q4304392   P30-verified   Q48 ) ⇐ {(( Q4304392   P30   Q48 )  nn   geo ) ( Q4304392   P30   Q48 ) ( Q4304392   demo-country   yes )}
( Q43287   P30-verified   Q46 ) ⇐ {(( Q43287   P30   Q46 )  nn   geo ) ( Q43287   P30   Q46 ) ( Q43287   demo-country   yes )}
( Q870055   P30-verified   Q48 ) ⇐ {(( Q870055   P30   Q48 )  nn   geo ) ( Q870055   P30   Q48 ) ( Q870055   demo-country   yes )}
( Q175276   P30-verified   Q46 ) ⇐ {(( Q175276   P30   Q46 )  nn   geo ) ( Q175276   P30   Q46 ) ( Q175276   demo-country   yes )}
( Q218023   P30-verified   Q15 ) ⇐ {(( Q218023   P30   Q15 )  nn   geo ) ( Q218023   P30   Q15 ) ( Q218023   demo-country   yes )}
( Q20949725   P30-verified   Q46 ) ⇐ {(( Q20949725   P30   Q46 )  nn   geo ) ( Q20949725   P30   Q46 ) ( Q20949725   demo-country   yes )}
( Q18285930   P30-verified   Q46 ) ⇐ {(( Q18285930   P30   Q46 )  nn   geo ) ( Q18285930   P30   Q46 ) ( Q18285930   demo-country   yes )}
( Q717   P30-verified   Q18 ) ⇐ {(( Q717   P30   Q18 )  nn   geo ) ( Q717   P30   Q18 ) ( Q717   demo-country   yes )}
( Q1232887   P30-verified   Q46 ) ⇐ {(( Q1232887   P30   Q46 )  nn   geo ) ( Q1232887   P30   Q46 ) ( Q1232887   demo-country   yes )}
( Q2415003   P30-verified   Q46 ) ⇐ {(( Q2415003   P30   Q46 )  nn   geo ) ( Q2415003   P30   Q46 ) ( Q2415003   demo-country   yes )}
( Q170588   P30-verified   Q49 ) ⇐ {(( Q170588   P30   Q49 )  nn   geo ) ( Q170588   P30   Q49 ) ( Q170588   demo-country   yes )}
( Q618399   P30-verified   Q15 ) ⇐ {(( Q618399   P30   Q15 )  nn   geo ) ( Q618399   P30   Q15 ) ( Q618399   demo-country   yes )}
( Q771193   P30-verified   Q46 ) ⇐ {(( Q771193   P30   Q46 )  nn   geo ) ( Q771193   P30   Q46 ) ( Q771193   demo-country   yes )}
( Q115166787   P30-verified   Q48 ) ⇐ {(( Q115166787   P30   Q48 )  nn   geo ) ( Q115166787   P30   Q48 ) ( Q115166787   demo-country   yes )}
( Q34   P30-verified   Q46 ) ⇐ {(( Q34   P30   Q46 )  nn   geo ) ( Q34   P30   Q46 ) ( Q34   demo-country   yes )}
( Q16056854   P30-verified   Q46 ) ⇐ {(( Q16056854   P30   Q46 )  nn   geo ) ( Q16056854   P30   Q46 ) ( Q16056854   demo-country   yes )}
( Q114327408   P30-verified   Q46 ) ⇐ {(( Q114327408   P30   Q46 )  nn   geo ) ( Q114327408   P30   Q46 ) ( Q114327408   demo-country   yes )}
( Q243652   P30-verified   Q46 ) ⇐ {(( Q243652   P30   Q46 )  nn   geo ) ( Q243652   P30   Q46 ) ( Q243652   demo-country   yes )}
( Q140359   P30-verified   Q46 ) ⇐ {(( Q140359   P30   Q46 )  nn   geo ) ( Q140359   P30   Q46 ) ( Q140359   demo-country   yes )}
( Q330362   P30-verified   Q46 ) ⇐ {(( Q330362   P30   Q46 )  nn   geo ) ( Q330362   P30   Q46 ) ( Q330362   demo-country   yes )}
( Q1054184   P30-verified   Q48 ) ⇐ {(( Q1054184   P30   Q48 )  nn   geo ) ( Q1054184   P30   Q48 ) ( Q1054184   demo-country   yes )}
( Q12536   P30-verified   Q15 ) ⇐ {(( Q12536   P30   Q15 )  nn   geo ) ( Q12536   P30   Q15 ) ( Q12536   demo-country   yes )}
( Q31747   P30-verified   Q46 ) ⇐ {(( Q31747   P30   Q46 )  nn   geo ) ( Q31747   P30   Q46 ) ( Q31747   demo-country   yes )}
( Q267584   P30-verified   Q48 ) ⇐ {(( Q267584   P30   Q48 )  nn   geo ) ( Q267584   P30   Q48 ) ( Q267584   demo-country   yes )}
( Q1998866   P30-verified   Q46 ) ⇐ {(( Q1998866   P30   Q46 )  nn   geo ) ( Q1998866   P30   Q46 ) ( Q1998866   demo-country   yes )}
( Q2454585   P30-verified   Q46 ) ⇐ {(( Q2454585   P30   Q46 )  nn   geo ) ( Q2454585   P30   Q46 ) ( Q2454585   demo-country   yes )}
( Q33   P30-verified   Q46 ) ⇐ {(( Q33   P30   Q46 )  nn   geo ) ( Q33   P30   Q46 ) ( Q33   demo-country   yes )}
( Q8733   P30-verified   Q48 ) ⇐ {(( Q8733   P30   Q48 )  nn   geo ) ( Q8733   P30   Q48 ) ( Q8733   demo-country   yes )}
( Q703695   P30-verified   Q48 ) ⇐ {(( Q703695   P30   Q48 )  nn   geo ) ( Q703695   P30   Q48 ) ( Q703695   demo-country   yes )}
( Q3623202   P30-verified   Q46 ) ⇐ {(( Q3623202   P30   Q46 )  nn   geo ) ( Q3623202   P30   Q46 ) ( Q3623202   demo-country   yes )}
( Q173065   P30-verified   Q46 ) ⇐ {(( Q173065   P30   Q46 )  nn   geo ) ( Q173065   P30   Q46 ) ( Q173065   demo-country   yes )}
( Q1147441   P30-verified   Q48 ) ⇐ {(( Q1147441   P30   Q48 )  nn   geo ) ( Q1147441   P30   Q48 ) ( Q1147441   demo-country   yes )}
( Q216632   P30-verified   Q15 ) ⇐ {(( Q216632   P30   Q15 )  nn   geo ) ( Q216632   P30   Q15 ) ( Q216632   demo-country   yes )}
( Q19083   P30-verified   Q5401 ) ⇐ {(( Q19083   P30   Q5401 )  nn   geo ) ( Q19083   P30   Q5401 ) ( Q19083   demo-country   yes )}
( Q671658   P30-verified   Q828 ) ⇐ {(( Q671658   P30   Q828 )  nn   geo ) ( Q671658   P30   Q828 ) ( Q671658   demo-country   yes )}
( Q1483495   P30-verified   Q46 ) ⇐ {(( Q1483495   P30   Q46 )  nn   geo ) ( Q1483495   P30   Q46 ) ( Q1483495   demo-country   yes )}
( Q12060881   P30-verified   Q48 ) ⇐ {(( Q12060881   P30   Q48 )  nn   geo ) ( Q12060881   P30   Q48 ) ( Q12060881   demo-country   yes )}
( Q335088   P30-verified   Q48 ) ⇐ {(( Q335088   P30   Q48 )  nn   geo ) ( Q335088   P30   Q48 ) ( Q335088   demo-country   yes )}
( Q10957559   P30-verified   Q46 ) ⇐ {(( Q10957559   P30   Q46 )  nn   geo ) ( Q10957559   P30   Q46 ) ( Q10957559   demo-country   yes )}
( Q1155700   P30-verified   Q48 ) ⇐ {(( Q1155700   P30   Q48 )  nn   geo ) ( Q1155700   P30   Q48 ) ( Q1155700   demo-country   yes )}
( Q245160   P30-verified   Q5401 ) ⇐ {(( Q245160   P30   Q5401 )  nn   geo ) ( Q245160   P30   Q5401 ) ( Q245160   demo-country   yes )}
( Q7313   P30-verified   Q48 ) ⇐ {(( Q7313   P30   Q48 )  nn   geo ) ( Q7313   P30   Q48 ) ( Q7313   demo-country   yes )}
( Q4147013   P30-verified   Q15 ) ⇐ {(( Q4147013   P30   Q15 )  nn   geo ) ( Q4147013   P30   Q15 ) ( Q4147013   demo-country   yes )}
( Q1057542   P30-verified   Q538 ) ⇐ {(( Q1057542   P30   Q538 )  nn   geo ) ( Q1057542   P30   Q538 ) ( Q1057542   demo-country   yes )}
( Q774783   P30-verified   Q46 ) ⇐ {(( Q774783   P30   Q46 )  nn   geo ) ( Q774783   P30   Q46 ) ( Q774783   demo-country   yes )}
( Q107258515   P30-verified   Q48 ) ⇐ {(( Q107258515   P30   Q48 )  nn   geo ) ( Q107258515   P30   Q48 ) ( Q107258515   demo-country   yes )}
( Q37102   P30-verified   Q15 ) ⇐ {(( Q37102   P30   Q15 )  nn   geo ) ( Q37102   P30   Q15 ) ( Q37102   demo-country   yes )}
( Q203493   P30-verified   Q46 ) ⇐ {(( Q203493   P30   Q46 )  nn   geo ) ( Q203493   P30   Q46 ) ( Q203493   demo-country   yes )}
( Q1470101   P30-verified   Q46 ) ⇐ {(( Q1470101   P30   Q46 )  nn   geo ) ( Q1470101   P30   Q46 ) ( Q1470101   demo-country   yes )}
( Q29520   P30-verified   Q48 ) ⇐ {(( Q29520   P30   Q48 )  nn   geo ) ( Q29520   P30   Q48 ) ( Q29520   demo-country   yes )}
( Q187035   P30-verified   Q46 ) ⇐ {(( Q187035   P30   Q46 )  nn   geo ) ( Q187035   P30   Q46 ) ( Q187035   demo-country   yes )}
( Q23366230   P30-verified   Q46 ) ⇐ {(( Q23366230   P30   Q46 )  nn   geo ) ( Q23366230   P30   Q46 ) ( Q23366230   demo-country   yes )}
( Q1048340   P30-verified   Q46 ) ⇐ {(( Q1048340   P30   Q46 )  nn   geo ) ( Q1048340   P30   Q46 ) ( Q1048340   demo-country   yes )}
( Q200262   P30-verified   Q46 ) ⇐ {(( Q200262   P30   Q46 )  nn   geo ) ( Q200262   P30   Q46 ) ( Q200262   demo-country   yes )}
( Q127424576   P30-verified   Q15 ) ⇐ {(( Q127424576   P30   Q15 )  nn   geo ) ( Q127424576   P30   Q15 ) ( Q127424576   demo-country   yes )}
( Q4453003   P30-verified   Q48 ) ⇐ {(( Q4453003   P30   Q48 )  nn   geo ) ( Q4453003   P30   Q48 ) ( Q4453003   demo-country   yes )}
( Q110362913   P30-verified   Q48 ) ⇐ {(( Q110362913   P30   Q48 )  nn   geo ) ( Q110362913   P30   Q48 ) ( Q110362913   demo-country   yes )}
( Q80211   P30-verified   Q46 ) ⇐ {(( Q80211   P30   Q46 )  nn   geo ) ( Q80211   P30   Q46 ) ( Q80211   demo-country   yes )}
( Q2712121   P30-verified   Q46 ) ⇐ {(( Q2712121   P30   Q46 )  nn   geo ) ( Q2712121   P30   Q46 ) ( Q2712121   demo-country   yes )}
( Q2526751   P30-candidate   Q46 ) ⇐ {((¬( Q2526751   P30   Q46 ))  nn   geo ) (¬( Q2526751   P30   Q46 )) ( Q2526751   demo-country   yes )}
( Q62389   P30-candidate   Q46 ) ⇐ {((¬( Q62389   P30   Q46 ))  nn   geo ) (¬( Q62389   P30   Q46 )) ( Q62389   demo-country   yes )}
( Q42345769   P30-candidate   Q46 ) ⇐ {((¬( Q42345769   P30   Q46 ))  nn   geo ) (¬( Q42345769   P30   Q46 )) ( Q42345769   demo-country   yes )}
( Q188736   P30-candidate   Q46 ) ⇐ {((¬( Q188736   P30   Q46 ))  nn   geo ) (¬( Q188736   P30   Q46 )) ( Q188736   demo-country   yes )}
( Q814959   P30-candidate   Q46 ) ⇐ {((¬( Q814959   P30   Q46 ))  nn   geo ) (¬( Q814959   P30   Q46 )) ( Q814959   demo-country   yes )}
( Q3136869   P30-candidate   Q46 ) ⇐ {((¬( Q3136869   P30   Q46 ))  nn   geo ) (¬( Q3136869   P30   Q46 )) ( Q3136869   demo-country   yes )}
( Q1530762   P30-candidate   Q46 ) ⇐ {((¬( Q1530762   P30   Q46 ))  nn   geo ) (¬( Q1530762   P30   Q46 )) ( Q1530762   demo-country   yes )}
( Q109534069   P30-candidate   Q46 ) ⇐ {((¬( Q109534069   P30   Q46 ))  nn   geo ) (¬( Q109534069   P30   Q46 )) ( Q109534069   demo-country   yes )}
( Q126282254   P30-candidate   Q46 ) ⇐ {((¬( Q126282254   P30   Q46 ))  nn   geo ) (¬( Q126282254   P30   Q46 )) ( Q126282254   demo-country   yes )}
( Q3167772   P30-candidate   Q46 ) ⇐ {((¬( Q3167772   P30   Q46 ))  nn   geo ) (¬( Q3167772   P30   Q46 )) ( Q3167772   demo-country   yes )}
( Q96051590   P30-candidate   Q46 ) ⇐ {((¬( Q96051590   P30   Q46 ))  nn   geo ) (¬( Q96051590   P30   Q46 )) ( Q96051590   demo-country   yes )}
( Q1152126   P30-candidate   Q46 ) ⇐ {((¬( Q1152126   P30   Q46 ))  nn   geo ) (¬( Q1152126   P30   Q46 )) ( Q1152126   demo-country   yes )}
( Q282475   P30-candidate   Q46 ) ⇐ {((¬( Q282475   P30   Q46 ))  nn   geo ) (¬( Q282475   P30   Q46 )) ( Q282475   demo-country   yes )}
( Q85804030   P30-candidate   Q46 ) ⇐ {((¬( Q85804030   P30   Q46 ))  nn   geo ) (¬( Q85804030   P30   Q46 )) ( Q85804030   demo-country   yes )}
( Q134302030   P30-candidate   Q46 ) ⇐ {((¬( Q134302030   P30   Q46 ))  nn   geo ) (¬( Q134302030   P30   Q46 )) ( Q134302030   demo-country   yes )}
( Q977566   P30-candidate   Q46 ) ⇐ {((¬( Q977566   P30   Q46 ))  nn   geo ) (¬( Q977566   P30   Q46 )) ( Q977566   demo-country   yes )}
( Q332005   P30-candidate   Q46 ) ⇐ {((¬( Q332005   P30   Q46 ))  nn   geo ) (¬( Q332005   P30   Q46 )) ( Q332005   demo-country   yes )}
( Q5362837   P30-candidate   Q46 ) ⇐ {((¬( Q5362837   P30   Q46 ))  nn   geo ) (¬( Q5362837   P30   Q46 )) ( Q5362837   demo-country   yes )}
( Q370372   P30-candidate   Q46 ) ⇐ {((¬( Q370372   P30   Q46 ))  nn   geo ) (¬( Q370372   P30   Q46 )) ( Q370372   demo-country   yes )}
( Q2362063   P30-candidate   Q46 ) ⇐ {((¬( Q2362063   P30   Q46 ))  nn   geo ) (¬( Q2362063   P30   Q46 )) ( Q2362063   demo-country   yes )}
( Q6037274   P30-candidate   Q46 ) ⇐ {((¬( Q6037274   P30   Q46 ))  nn   geo ) (¬( Q6037274   P30   Q46 )) ( Q6037274   demo-country   yes )}
( Q113411770   P30-candidate   Q46 ) ⇐ {((¬( Q113411770   P30   Q46 ))  nn   geo ) (¬( Q113411770   P30   Q46 )) ( Q113411770   demo-country   yes )}
( Q976099   P30-candidate   Q46 ) ⇐ {((¬( Q976099   P30   Q46 ))  nn   geo ) (¬( Q976099   P30   Q46 )) ( Q976099   demo-country   yes )}
( Q137386301   P30-candidate   Q46 ) ⇐ {((¬( Q137386301   P30   Q46 ))  nn   geo ) (¬( Q137386301   P30   Q46 )) ( Q137386301   demo-country   yes )}
( Q449639   P30-candidate   Q46 ) ⇐ {((¬( Q449639   P30   Q46 ))  nn   geo ) (¬( Q449639   P30   Q46 )) ( Q449639   demo-country   yes )}
( Q1968554   P30-candidate   Q46 ) ⇐ {((¬( Q1968554   P30   Q46 ))  nn   geo ) (¬( Q1968554   P30   Q46 )) ( Q1968554   demo-country   yes )}
( Q30890672   P30-candidate   Q46 ) ⇐ {((¬( Q30890672   P30   Q46 ))  nn   geo ) (¬( Q30890672   P30   Q46 )) ( Q30890672   demo-country   yes )}
( Q13125117   P30-candidate   Q46 ) ⇐ {((¬( Q13125117   P30   Q46 ))  nn   geo ) (¬( Q13125117   P30   Q46 )) ( Q13125117   demo-country   yes )}
( Q2480041   P30-candidate   Q46 ) ⇐ {((¬( Q2480041   P30   Q46 ))  nn   geo ) (¬( Q2480041   P30   Q46 )) ( Q2480041   demo-country   yes )}
( Q138562037   P30-candidate   Q46 ) ⇐ {((¬( Q138562037   P30   Q46 ))  nn   geo ) (¬( Q138562037   P30   Q46 )) ( Q138562037   demo-country   yes )}
( Q950101   P30-candidate   Q46 ) ⇐ {((¬( Q950101   P30   Q46 ))  nn   geo ) (¬( Q950101   P30   Q46 )) ( Q950101   demo-country   yes )}
( Q96028967   P30-candidate   Q46 ) ⇐ {((¬( Q96028967   P30   Q46 ))  nn   geo ) (¬( Q96028967   P30   Q46 )) ( Q96028967   demo-country   yes )}
( Q494625   P30-candidate   Q46 ) ⇐ {((¬( Q494625   P30   Q46 ))  nn   geo ) (¬( Q494625   P30   Q46 )) ( Q494625   demo-country   yes )}
( Q5124786   P30-candidate   Q46 ) ⇐ {((¬( Q5124786   P30   Q46 ))  nn   geo ) (¬( Q5124786   P30   Q46 )) ( Q5124786   demo-country   yes )}
( Q4453007   P30-candidate   Q46 ) ⇐ {((¬( Q4453007   P30   Q46 ))  nn   geo ) (¬( Q4453007   P30   Q46 )) ( Q4453007   demo-country   yes )}
( Q16674373   P30-candidate   Q46 ) ⇐ {((¬( Q16674373   P30   Q46 ))  nn   geo ) (¬( Q16674373   P30   Q46 )) ( Q16674373   demo-country   yes )}
( Q707128   P30-candidate   Q46 ) ⇐ {((¬( Q707128   P30   Q46 ))  nn   geo ) (¬( Q707128   P30   Q46 )) ( Q707128   demo-country   yes )}
( Q3267672   P30-candidate   Q46 ) ⇐ {((¬( Q3267672   P30   Q46 ))  nn   geo ) (¬( Q3267672   P30   Q46 )) ( Q3267672   demo-country   yes )}
( Q6773257   P30-candidate   Q46 ) ⇐ {((¬( Q6773257   P30   Q46 ))  nn   geo ) (¬( Q6773257   P30   Q46 )) ( Q6773257   demo-country   yes )}
( Q11681694   P30-candidate   Q46 ) ⇐ {((¬( Q11681694   P30   Q46 ))  nn   geo ) (¬( Q11681694   P30   Q46 )) ( Q11681694   demo-country   yes )}
( Q20507218   P30-candidate   Q46 ) ⇐ {((¬( Q20507218   P30   Q46 ))  nn   geo ) (¬( Q20507218   P30   Q46 )) ( Q20507218   demo-country   yes )}
( Q12491063   P30-candidate   Q46 ) ⇐ {((¬( Q12491063   P30   Q46 ))  nn   geo ) (¬( Q12491063   P30   Q46 )) ( Q12491063   demo-country   yes )}
( Q1362278   P30-candidate   Q46 ) ⇐ {((¬( Q1362278   P30   Q46 ))  nn   geo ) (¬( Q1362278   P30   Q46 )) ( Q1362278   demo-country   yes )}
--- Reasoning iteration 2 ---
Reasoning complete. Total unification matches processed: 816. Total contradictions found: 0.
Reasoning summary: 816 matches processed, 0 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m2.344s – 816 matches processed, 0 contradictions found.
Ready.
P30-verified facts: 99
  Q1057542 (Republic of Hawaii) -> Q538  conf=1.000
  Q747314 (Bechuanaland Protectorate) -> Q15  conf=0.994
  Q245160 (Democratic Republic of Georgia) -> Q5401  conf=1.000
  Q170468 (Q170468) -> Q15  conf=0.609
  Q115166787 (Q115166787) -> Q48  conf=0.989
P30-candidate facts: 43
  Q1530762 (Kingdom of Kandy) -> Q46  conf=0.527
  Q109534069 (Kingdom of Wolaita) -> Q46  conf=0.527
  Q3136869 (Kingdom of Dambadeniya) -> Q46  conf=0.527
  Q3267672 (Republic of Entre R\u00edos) -> Q46  conf=0.527
  Q814959 (Beiyang Government) -> Q46  conf=0.527
Q183 not in the country set of this dump (pruning artifact); skipping Q183 checks
-- 2.497 s --
wikidata->
```
