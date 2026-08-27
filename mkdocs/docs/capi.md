## The C ABI

zelph is a C++ library with a Janet host on top. The C ABI is the third way in: a narrow `extern "C"` surface over the graph and its compiled networks, so a program written in Rust, C, Go, Python or anything else with an FFI can drive zelph directly, without the Janet interpreter in between.

It is deliberately small. Everything that is naturally expressed as a script — rules, `.import`, display schemes, SPARQL — stays in Janet and the REPL. What the C ABI covers is the part a *host application* needs: resolve names to nodes, assert facts, read them back, compile a network out of the graph, evaluate it, train it, and persist the result.

The header is `zelph/capi/zelph_c.h`. Link against `libzelph`.

### Conventions

Every function follows the same five rules, so there is nothing per-function to remember:

| | |
| --- | --- |
| **Status, not exceptions** | Every function returns an `int32_t` status (`ZELPH_OK`, `ZELPH_INVALID_ARGUMENT`, `ZELPH_BUFFER_TOO_SMALL`, `ZELPH_RUNTIME_ERROR`). Results come back through out-parameters. No C++ exception ever crosses the boundary. |
| **One message per thread** | On failure, `zelph_last_error()` returns the reason. It is thread-local and borrowed — valid until the next `zelph_*` call on that thread. A successful call clears it. |
| **Nodes are `uint64_t`** | A node *is* its hash, so it is stable across calls and across a save/load cycle: resolving the same name in a freshly loaded graph yields the same number. `0` is not a node. |
| **Strings are UTF-8** | Strings passed in are borrowed for the duration of the call. Strings handed out are owned by the caller and released with `zelph_string_free()`. |
| **Caller owns array memory** | Arrays are written into a caller-supplied buffer. The `count` parameter is in/out: on entry the capacity, on return the number of elements produced. If the buffer is too small, *nothing* is written, `count` holds what is needed and the call returns `ZELPH_BUFFER_TOO_SMALL` — so passing a null buffer with capacity `0` asks for the size. |

### Lifetime and threading

```c
int32_t zelph_engine_create(zelph_output_fn output, void* user_data, zelph_engine** out_engine);
void    zelph_engine_destroy(zelph_engine* engine);
```

One engine per process. The Janet script engine keeps a process-wide instance pointer for its C functions, so a second engine would silently redirect the first one's calls; `zelph_engine_create` refuses with `ZELPH_RUNTIME_ERROR` while another engine is alive. Destroying the engine invalidates every network handle taken from it.

`output` may be null, in which case the engine writes to the process's standard streams as the `zelph` binary does. Otherwise every line the engine emits is handed to the callback with its channel (`ZELPH_CHANNEL_OUT`, `ZELPH_CHANNEL_ERROR`, `ZELPH_CHANNEL_DIAGNOSTIC`, `ZELPH_CHANNEL_PROMPT`) — which is what a host with its own protocol on stdout, such as a game engine speaking to a GUI, needs.

**`zelph_nn_eval_nodes` is safe to call from any number of threads at once, including while another thread is training the same network.** That guarantee comes from `NeuralNet` itself and is the reason a compiled net can serve a parallel search. Everything that mutates the *graph* — `zelph_resolve`, `zelph_fact`, `zelph_list`, `zelph_load`, `zelph_save`, `zelph_nn_compile`, `zelph_nn_connect_layers`, `zelph_nn_write_back` — is main-thread only, exactly as the corresponding Janet functions are.

### The graph

| Function | |
| --- | --- |
| `zelph_resolve(engine, name, lang, out_node)` | Name to node, creating it if needed. `lang` may be null for the current language. |
| `zelph_fact(engine, subject, predicate, objects, object_count, out_fact)` | Create the fact `(subject predicate object...)` and return its node. |
| `zelph_fact_parts(engine, fact, out_subject, out_predicate, out_objects, count)` | The subject, predicate and objects of a fact – the inverse of `zelph_fact`. The objects come back unordered, because a fact’s objects are a set. |
| `zelph_list(engine, elements, count, out_node)` | Cons list; the first element becomes the outermost cell. An empty list is the `nil` node. |
| `zelph_list_elements(engine, list, out_nodes, count)` | The elements of a cons list, in order – the inverse of `zelph_list`. `nil` has none; a node that is not a list is an error, not an empty answer. |
| `zelph_name(engine, node, lang, out_name)` | The node's name, or null when it has none — an answer, not an error. Free with `zelph_string_free`. |
| `zelph_sources(engine, predicate, target, out_nodes, count)` | Every subject of a fact `(X predicate target)`. Directional: `(target predicate X)` does not contribute. |
| `zelph_load(engine, path)` | As the `.load` command, including format detection. |
| `zelph_save(engine, path)` | As the `.save` command. The path must end in `.bin`. |

Structural identity is the property to build on: `zelph_list` over the same nodes returns the same node, because the graph interns structurally identical subgraphs. Two callers that describe the same structure arrive at the same identifier without agreeing on one.

And it survives the file. Because a node *is* its hash, describing the same structure again in a freshly loaded graph arrives at the node the file talks about, so a caller that saved facts about `<a b c>` finds them again without having stored an id anywhere – the structure is the key. `zelph_list_elements` and `zelph_fact_parts` close the loop by reading a structure back, which is what a program needs to use one as a *stored* identifier rather than only as a lookup key. Everything the ABI can build it can also take apart, including what the ABI did not build: a cons cell is the fact `(car cons cdr)`, so the two readers are one mechanism seen twice.

### Networks

| Function | |
| --- | --- |
| `zelph_nn_compile(engine, layers, layer_count, activation, out_handle)` | Compile a feed-forward view of the sub-graph spanned by the layer nodes, input first, output last. `activation` applies to the hidden layers; the output layer is always linear. |
| `zelph_nn_connect_layers(engine, from, to, scale, seed, out_created)` | Fully connect two layers with raw synapses drawn from `[-scale, scale]`. Existing synapses keep their weights, so the call is idempotent and re-wiring never destroys training. |
| `zelph_nn_eval_nodes(engine, handle, in_nodes, in_activations, in_count, top_k, out_nodes, out_scores, count)` | Forward pass with node-addressed multi-hot input. A null activation array means every listed neuron is `1.0`. Results are sorted by descending score, ties by ascending node; `top_k < 0` returns the whole output layer. |
| `zelph_nn_train_nodes(engine, handle, in_nodes, in_activations, in_count, target_nodes, target_activations, target_count, learning_rate, out_loss)` | One SGD step; `out_loss` is the loss *before* the update. |
| `zelph_nn_layer_nodes(engine, handle, layer, out_nodes, count)` | The neurons of one layer in slot order; layer 0 is the input layer. The mapping a slot-addressed caller needs. |
| `zelph_nn_eval_slots(engine, handle, in_slots, in_activations, in_count, top_k, out_nodes, out_scores, count)` | `zelph_nn_eval_nodes` with the active inputs named by slot. A slot outside the input layer is an error. |
| `zelph_nn_train_slots(engine, handle, in_slots, in_activations, in_count, target_nodes, target_activations, target_count, learning_rate, out_loss)` | `zelph_nn_train_nodes` with the input named by slot; the target stays node-addressed. |
| `zelph_nn_accumulator_size(engine, handle, out_size)` | How many doubles one accumulator of this network holds. |
| `zelph_nn_accumulator_set(engine, handle, slots, activations, count, accumulator, accumulator_size)` | The input layer's pre-activation for an active set. Set then eval is `zelph_nn_eval_slots` to the bit. |
| `zelph_nn_accumulator_update(engine, handle, added, added_activations, added_count, removed, removed_activations, removed_count, accumulator, accumulator_size)` | The same vector moved: the removed rows are subtracted before the added ones are added. |
| `zelph_nn_accumulator_eval(engine, handle, accumulator, accumulator_size, top_k, out_nodes, out_scores, count)` | The layers behind the first, from an accumulator. Sorted and limited as `zelph_nn_eval_nodes` is. |
| `zelph_nn_write_back(engine, handle)` | Copy the compiled net's weights into the graph's edge-weight store — required before `zelph_save`, or what is persisted is the untrained graph. |
| `zelph_nn_snapshot_shape(engine, handle, out_sizes, count)` | One element count per weight matrix. |
| `zelph_nn_snapshot(engine, handle, out_weights, count)` | The weights, matrices concatenated in layer order. One matrix is row-major by post-synaptic unit: input `i` to unit `j` is at `j * n_pre + i`. |
| `zelph_nn_restore(engine, handle, weights, weight_count, sizes, size_count)` | Put a snapshot back. The shapes must match. |

The node-addressed entry points are the ones that matter for a sparse input layer: the input is the *list of active neurons*, so a 768-input encoding with 32 pieces on the board costs 32 terms, not 768.

A caller that evaluates the same layer millions of times can go one step further and name the active neurons by their slot in the input layer rather than by their node. That skips a hash lookup per active neuron, which on a small network is the largest single item an evaluation has left – 0.17 of 0.43 microseconds for 34 active inputs of 780. Resolve the mapping once with `zelph_nn_layer_nodes` after compiling, then pass slots for ever after.

### Keeping the first layer between calls

An accumulator is the input layer’s pre-activation vector, maintained by the caller and shifted by the difference between one active input set and the next rather than being reconstructed from all of them. Where consecutive queries share most of their active inputs – a search over states that change by a few features per move, or a fixed context scored against many candidates – that transforms the cost of the first layer from `O(active)` into `O(changed)`, and what remains is the layers behind it.

The buffer is `zelph_nn_accumulator_size` doubles and belongs to the caller, so it incurs no allocation, no handle and no lock to copy: a search retains one per ply and duplicates the parent’s on the way down. Measured on a 780 × 32 × 1 network with 34 active inputs, two of which a move changes: 0.12 µs for a moved accumulator against 0.32 µs for the same evaluation built from scratch.

Two points to grasp. An accumulator holds value only in relation to the weights it was constructed with, thus a training iteration nullifies each of those. And `zelph_nn_accumulator_update` is not bit-identical to `zelph_nn_accumulator_set` when applied to the same active set: adding and subtracting rows introduces rounding disparities compared to summing them in a single operation, and that divergence builds up across a sequence of updates. Set it afresh whenever that becomes relevant.

### The hidden-layer activation

`ZELPH_ACTIVATION_RELU` is `max(0, x)` and is the default, the value `0` of the enum.
`ZELPH_ACTIVATION_LEAKY_RELU` is `max(0.01 x, x)`.

The difference is not a matter of taste. **With a plain ReLU, a hidden layer whose every unit
is negative for every input has an output of exactly 0 and a gradient of exactly 0.** No
further training can leave that state — it is absorbing — and a small net trained online can
walk into it and stay there. A leaky unit passes a hundredth of the gradient instead of none,
which is the difference between "slow" and "never"; `src/test/test_capi.cpp` pins exactly
that, with 500 training steps that move a leaky net and leave a ReLU one bit for bit
unchanged.

The activation is a property of the compiled **view**, not of the graph: a net trained with
one must be evaluated with the same one, or every output that came from a unit below zero
changes. Nothing in the file records it, so a caller that uses anything but the default has
to record the choice itself — as a fact in the graph, next to whatever else describes how the
net was built.

Snapshot and restore exist because training walks past its best point: the criterion that says "stop" can only fire after the fact, so without a way back the weights that get saved are always some epochs late.

### A complete example

```c
#include <zelph_c.h>

#include <stdio.h>

static void report(void* user, int32_t channel, const char* text, int32_t newline)
{
    (void)user;
    (void)newline;
    if (channel == ZELPH_CHANNEL_ERROR) printf("engine: %s\n", text);
}

#define CHECK(call)                                           \
    if ((call) != ZELPH_OK)                                   \
    {                                                         \
        printf("%s failed: %s\n", #call, zelph_last_error()); \
        return 1;                                             \
    }

int main(void)
{
    zelph_engine* z = NULL;
    CHECK(zelph_engine_create(report, NULL, &z))

    /* Two neurons in an input layer, two in an output layer. */
    zelph_node in = 0, out = 0, part_of = 0;
    CHECK(zelph_resolve(z, "In", NULL, &in))
    CHECK(zelph_resolve(z, "Out", NULL, &out))
    CHECK(zelph_resolve(z, "in", NULL, &part_of))

    const char* neurons[4] = {"i1", "i2", "o1", "o2"};
    zelph_node  node[4];
    for (int i = 0; i < 4; ++i)
    {
        zelph_node fact  = 0;
        zelph_node layer = i < 2 ? in : out;
        CHECK(zelph_resolve(z, neurons[i], NULL, &node[i]))
        CHECK(zelph_fact(z, node[i], part_of, &layer, 1, &fact))
    }

    int64_t created = 0;
    CHECK(zelph_nn_connect_layers(z, in, out, 0.0, 1, &created))
    printf("synapses created: %lld\n", (long long)created);

    const zelph_node layers[2] = {in, out};
    zelph_net        net       = -1;
    CHECK(zelph_nn_compile(z, layers, 2, &net))

    /* i1 -> o1, i2 -> o2 */
    for (int epoch = 0; epoch < 60; ++epoch)
    {
        CHECK(zelph_nn_train_nodes(z, net, &node[0], NULL, 1, &node[2], NULL, 1, 0.5, NULL))
        CHECK(zelph_nn_train_nodes(z, net, &node[1], NULL, 1, &node[3], NULL, 1, 0.5, NULL))
    }

    for (int i = 0; i < 2; ++i)
    {
        zelph_node top   = 0;
        double     score = 0;
        size_t     count = 1;
        char*      name  = NULL;
        CHECK(zelph_nn_eval_nodes(z, net, &node[i], NULL, 1, 1, &top, &score, &count))
        CHECK(zelph_name(z, top, NULL, &name))
        printf("%s -> %s (%.3f)\n", neurons[i], name, score);
        zelph_string_free(name);
    }

    zelph_engine_destroy(z);
    return 0;
}
```

Built against a `build-release` tree:

```bash
gcc -std=c11 example.c \
    -I<zelph>/src/lib/capi -I<zelph>/build-release/src/lib \
    -L<zelph>/build-release/bin -lzelph \
    -Wl,-rpath,<zelph>/build-release/bin \
    -o example
```

```
synapses created: 4
i1 -> o1 (1.000)
i2 -> o2 (1.000)
```

### Reasoning

The graph is not only a store, and this is the surface a program needs to use rules over it.

| Function | |
| --- | --- |
| `zelph_variable(engine, name, out_node)` | A variable for a rule or a query pattern. Remembered by name, so asking twice gives the same node - which is what makes a pattern built in one call queryable in another. `zelph_clear_variables` forgets them |
| `zelph_set(engine, elements, count, out_node)` | A set constant: identified by its members, so the same elements always yield the same node |
| `zelph_collection(engine, elements, count, out_node)` | A container with an identity of its own: two calls with the same elements yield two different nodes |
| `zelph_negate(engine, pattern, out_node)` | Mark a pattern as negation as failure - evaluated against the saturated positive fact base, never against in-flight state |
| `zelph_exists(engine, s, p, objects, n, out_exists)` | Does this fact exist? Creates nothing |
| `zelph_targets(engine, subject, predicate, out_nodes, count)` | The mirror of `zelph_sources` |
| `zelph_rule(engine, conditions, n, consequences, m, out)` | When every condition holds, deduce every consequence. Returns the condition set |
| `zelph_run` / `zelph_run_once` / `zelph_run_delta` | Forward chaining: to a fixed point, one pass, or seeded by what was created since the previous run |
| `zelph_set_parallel` | Whether unification may spread a relation's candidates over worker threads. On by default; returns the previous value |
| `zelph_query(engine, pattern, pairs, pair_count, row_sizes, row_count)` | Answer a pattern. The bindings come back flat - `2n` node ids per row, alternating variable and value - with one size per row |
| `zelph_cluster(engine, name)` | Activate a cluster, or deactivate with a null name. Nodes *created* while one is active are recorded in it |
| `zelph_cluster_active` / `_drop` / `_count` | The active cluster's name; remove everything a cluster recorded and report how many; how large a cluster is |

A query reports the variable as the **node the caller created**, not as a name, so no string
crosses the boundary and no lookup is needed to read an answer.

Clusters are what make the monotonic graph usable as a workspace. The loop a caller runs is
### Staying quiet

zelph narrates, because its primary front end is a REPL: a line per derived fact on
`ZELPH_CHANNEL_OUT`, the progress of a reasoning run on `ZELPH_CHANNEL_DIAGNOSTIC`
("Starting reasoning with 24 worker threads.", a summary, a per-iteration note), decoration on
`ZELPH_CHANNEL_PROMPT`. **Inside another program that is noise**, and for a host with its own
protocol on stdout it is a protocol error.

The channel is how you say so. An embedded caller ignores everything except
`ZELPH_CHANNEL_ERROR`:

```c
static void quiet(void* user, int32_t channel, const char* text, int32_t newline)
{
    if (channel != ZELPH_CHANNEL_ERROR) return;
    fputs(text, stderr);
    if (newline) fputc('\n', stderr);
}
```

Passing a **null** callback is not the quiet option — it means "write to the process's standard
streams", which is the REPL's behaviour and the loudest one. Keeping `ERROR` matters: a caller
that silences that too will debug the next failure blind.

`zelph_set_parallel` is a throughput/latency choice and not a semantic one: the derived facts
are the same either way. Parallelism pays on a large graph and costs on a small one, where
dispatch dominates the scan it replaces. A caller reasoning about many small fact bases in a
loop is the case that wants it off - and it was reachable from the REPL as `.parallel` long
before a C caller could touch it.

activate, assert, `zelph_run_delta`, query, deactivate, drop - and what a drop removes is
exactly what was created inside it, so a graph loaded from disk is never at risk.

### Rust

Two crates in `rust/` sit on this ABI and are maintained with it:

| | |
| --- | --- |
| `zelph-sys` | The raw declarations, generated from `zelph_c.h` by bindgen **at build time**, so header and bindings cannot drift. Its `build.rs` also rebuilds the C++ library on every `cargo build` — a stale library silently invalidates every measurement taken against it, and nothing in the output says so. `ZELPH_BUILD_DIR` selects the CMake build directory (default `build-release`), `ZELPH_NO_BUILD` skips the CMake step. |
| `zelph` | The safe wrapper: `Engine` (resolve, fact, list, elements, name, sources, load, save, compile), `Net` (best, eval, train, write_back, snapshot, restore), failures as `Result<_, zelph::Error>` with a `kind()` to branch on. |

The types state what the C header only documents. `Engine` is neither `Send` nor `Sync`, because graph mutation belongs to the thread that created it. `Net` is both — so a compiled network can be handed to a pool of search threads and evaluated there while another thread trains it, and the compiler checks that rather than a comment.

```rust
let z = zelph::Engine::new()?;
let net = z.compile(&[z.resolve("In")?, z.resolve("Out")?])?;

std::thread::scope(|scope| {
    for _ in 0..3 {
        scope.spawn(|| { net.best(&inputs).unwrap(); });   // &Net crosses threads
    }
    net.train(&inputs, &targets, 0.05).unwrap();           // while this runs
});
```

    cd rust && cargo test

Note the layers are declared with ordinary facts — `(i1 in In)` — which is the same statement a `.zph` script writes as `i1 in In`. The network is not a separate kind of object; it is a *view* of the graph, and that is why `zelph_save` persists it without a network format existing at all.
