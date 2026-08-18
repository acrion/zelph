/*
Copyright (c) 2025, 2026 acrion innovations GmbH
Authors: Stefan Zipproth, s.zipproth@acrion.ch

This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org

zelph is offered under a commercial and under the AGPL license.
For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.

AGPL licensing:

zelph is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

zelph is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with zelph. If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * The C ABI of zelph.
 *
 * It exists so that a program in another language can drive the graph and its
 * compiled networks without going through the Janet host. The surface is the
 * one the Janet bindings expose for the same purposes, in the same order of
 * arguments, so a caller can be ported between the two without re-reading the
 * semantics.
 *
 * Conventions, uniform across every function here:
 *
 *   - Every function returns an int32_t status, one of the zelph_status
 *     values. Results are written through out-parameters. No exception ever
 *     crosses this boundary; a failure sets the thread's last error, which
 *     zelph_last_error() returns.
 *   - A node is a uint64_t and IS its hash, so it is stable across calls and
 *     across a save/load cycle. 0 is not a node.
 *   - Strings are UTF-8. Strings passed IN are borrowed for the duration of
 *     the call. Strings handed OUT are owned by the caller and released with
 *     zelph_string_free(); the sole exception is zelph_last_error(), which
 *     borrows out of thread-local storage.
 *   - Arrays handed OUT are written into a caller-supplied buffer. The
 *     accompanying count is in/out: on entry the buffer's capacity in
 *     elements, on return the number of elements the call produced. When the
 *     capacity is too small, nothing is written, the count is set to what
 *     would be needed and the call returns ZELPH_BUFFER_TOO_SMALL - so
 *     passing a null buffer with capacity 0 is the way to ask for the size.
 *
 * Threading. One engine per process: the Janet host binds to a single script
 * engine, and creating a second one while the first is alive is refused.
 * Evaluating a compiled net (zelph_nn_eval_nodes) is safe from any number of
 * threads at once, including while another thread trains the same net -
 * that guarantee comes from NeuralNet itself. Everything that mutates the
 * GRAPH (zelph_resolve, zelph_fact, zelph_list, zelph_load, zelph_save,
 * zelph_nn_compile, zelph_nn_connect_layers, zelph_nn_write_back) is main
 * thread only, exactly as the corresponding Janet functions are.
 */

#ifndef ZELPH_C_H
#define ZELPH_C_H

#include <zelph_export.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Status codes. Every function in this header returns one of these. */
    enum zelph_status
    {
        ZELPH_OK = 0,

        /* A null pointer, a zero node, an out-of-range handle, an empty
           layer list - anything the caller can see from its own side. */
        ZELPH_INVALID_ARGUMENT = 1,

        /* The output buffer is smaller than the result; the count
           out-parameter holds the number of elements required. */
        ZELPH_BUFFER_TOO_SMALL = 2,

        /* The call reached the engine and the engine refused it. The reason
           is in zelph_last_error(). */
        ZELPH_RUNTIME_ERROR = 3
    };

    /* How a hidden layer's pre-activation becomes its activation.
     *
     * A property of the compiled VIEW: a net trained with one must be
     * evaluated with the same one, or its output changes. RELU is what every
     * net compiled before this option existed used, and is the value to pass
     * unless there is a reason not to. */
    enum zelph_activation
    {
        ZELPH_ACTIVATION_RELU = 0,

        /* max(0.01 x, x). The gradient is never exactly zero - which matters
           because with a plain ReLU a hidden layer whose every unit is
           negative for every input has an output of exactly 0 AND a gradient
           of exactly 0. That state is absorbing: no further training can
           leave it, and a small online-trained net can walk into it. */
        ZELPH_ACTIVATION_LEAKY_RELU = 1
    };

    /* Mirrors zelph::io::OutputChannel.

       The channel is how an EMBEDDED caller stays quiet, and an embedder
       almost certainly wants to be. zelph's primary front end is a REPL, so it
       narrates: a line per derived fact on OUT, the progress of a reasoning
       run on DIAGNOSTIC ("Starting reasoning with 24 worker threads.", a
       summary, a per-iteration note), decoration on PROMPT. Inside another
       program that is noise at best - and for a host with its own protocol on
       stdout, a protocol error.

         OUT         derived facts, query answers: the REPL's results
         ERROR       something went wrong; keep this one
         DIAGNOSTIC  progress and summaries: the REPL's narration
         PROMPT      the REPL's own decoration

       A caller that wants none of the narration ignores everything except
       ERROR. Passing a null callback to zelph_engine_create is NOT that - it
       means "write to the process's standard streams", which is the REPL's
       behaviour and the loudest option. */
    enum zelph_channel
    {
        ZELPH_CHANNEL_OUT        = 0,
        ZELPH_CHANNEL_ERROR      = 1,
        ZELPH_CHANNEL_DIAGNOSTIC = 2,
        ZELPH_CHANNEL_PROMPT     = 3
    };

    typedef struct zelph_engine zelph_engine;

    typedef uint64_t zelph_node;

    /* Handle of a compiled network, as returned by zelph_nn_compile. */
    typedef int32_t zelph_net;

    /* Receives everything the engine prints. `newline` is 1 when the engine
       ended the line, 0 when it did not (prompts, partial writes). The text
       is borrowed for the duration of the call. */
    typedef void (*zelph_output_fn)(void* user_data, int32_t channel, const char* text, int32_t newline);

    /* The message of the last failed call ON THIS THREAD, or "" if the last
       call succeeded. Borrowed: valid until the next zelph_* call on this
       thread. */
    ZELPH_EXPORT const char* zelph_last_error(void);

    /* Release a string handed out by this API (zelph_name). Passing null is
       allowed and does nothing. */
    ZELPH_EXPORT void zelph_string_free(char* text);

    /* Create the engine. `output` may be null, in which case output goes to
       the process's standard streams, as it does for the zelph binary.
       Refuses with ZELPH_RUNTIME_ERROR while another engine exists. */
    ZELPH_EXPORT int32_t zelph_engine_create(zelph_output_fn output, void* user_data, zelph_engine** out_engine);

    /* Destroy the engine. Passing null is allowed and does nothing. Every
       compiled-net handle of this engine dies with it. */
    ZELPH_EXPORT void zelph_engine_destroy(zelph_engine* engine);

    /* ---------------------------------------------------------------- graph */

    /* Resolve a name to its node, creating the node if it does not exist.
       `lang` may be null for the engine's current language. */
    ZELPH_EXPORT int32_t zelph_resolve(zelph_engine* engine, const char* name, const char* lang, zelph_node* out_node);

    /* Create the fact (subject predicate object...) and return its node. At
       least one object is required. */
    ZELPH_EXPORT int32_t zelph_fact(zelph_engine*     engine,
                                    zelph_node        subject,
                                    zelph_node        predicate,
                                    const zelph_node* objects,
                                    size_t            object_count,
                                    zelph_node*       out_fact);

    /* The parts of a fact node: its subject, its predicate and its objects –
       the inverse of zelph_fact. A statement is a node, so a caller that
       stored one, or received one from a query, can read it back rather than
       having to remember what it built.

       out_subject and out_predicate can be null when only the objects are
       desired. No output is generated unless the object buffer is
       sufficiently large, as throughout this ABI. A node that is not a fact
       answers ZELPH_INVALID_ARGUMENT. */
    ZELPH_EXPORT int32_t zelph_fact_parts(zelph_engine* engine,
                                          zelph_node    fact,
                                          zelph_node*   out_subject,
                                          zelph_node*   out_predicate,
                                          zelph_node*   out_objects,
                                          size_t*       count);

    /* Build a cons list from nodes. The first element becomes the outermost
       cons cell. An empty list is the nil node, as it is in Janet. */
    ZELPH_EXPORT int32_t zelph_list(zelph_engine* engine, const zelph_node* elements, size_t count, zelph_node* out_node);

    /* The elements of a cons list, in order - the inverse of zelph_list. The
       nil node is a list of no elements. A node that is neither nil nor a cons
       cell is not a list and answers ZELPH_INVALID_ARGUMENT, as does a chain
       that ends in something other than nil: half a structure is not an
       answer. */
    ZELPH_EXPORT int32_t zelph_list_elements(zelph_engine* engine, zelph_node list, zelph_node* out_nodes, size_t* count);

    /* The name of a node, or null in *out_name when it has none. `lang` may
       be null for the current language; the lookup falls back to another
       language, as zelph/name does. Free the result with zelph_string_free. */
    ZELPH_EXPORT int32_t zelph_name(zelph_engine* engine, zelph_node node, const char* lang, char** out_name);

    /* Every subject connected to `target` through `predicate`, i.e. the
       subjects of the facts (X predicate target). Directional: a fact
       (target predicate X) does not contribute. */
    ZELPH_EXPORT int32_t zelph_sources(zelph_engine* engine,
                                       zelph_node    predicate,
                                       zelph_node    target,
                                       zelph_node*   out_nodes,
                                       size_t*       count);

    /* Load a saved network (.bin) or import a data dump, exactly as the
       .load command does - including format detection and the checks that
       come with it. */
    ZELPH_EXPORT int32_t zelph_load(zelph_engine* engine, const char* path);

    /* Save the graph, exactly as the .save command does. The path must end
       in .bin. */
    ZELPH_EXPORT int32_t zelph_save(zelph_engine* engine, const char* path);

    /* ------------------------------------------------------------ reasoning */
    /*
     * The graph is not only a store: rules over it are the reason zelph
     * exists, and this is the surface a program needs to use them. Everything
     * here is main thread only.
     */

    /* A variable, for use inside a rule or a query pattern.
     *
     * Variables are remembered by name for as long as the engine lives, so
     * asking twice for "A" yields the same node - which is what makes a
     * pattern built in one call queryable in another. zelph_clear_variables
     * forgets them, so a later pattern can reuse the names for fresh ones. */
    ZELPH_EXPORT int32_t zelph_variable(zelph_engine* engine, const char* name, zelph_node* out_node);
    ZELPH_EXPORT int32_t zelph_clear_variables(zelph_engine* engine);

    /* A SET CONSTANT: identified by its members, so the same elements always
       yield the same node and membership cannot be extended. This is what a
       conjunction of conditions is built from. */
    ZELPH_EXPORT int32_t zelph_set(zelph_engine* engine, const zelph_node* elements, size_t count, zelph_node* out_node);

    /* A COLLECTION: a container with its own identity, so two calls with the
       same elements yield two different nodes. */
    ZELPH_EXPORT int32_t zelph_collection(zelph_engine* engine, const zelph_node* elements, size_t count, zelph_node* out_node);

    /* Mark a fact pattern as a negation, i.e. negation as failure. Evaluated
       against the SATURATED positive fact base, never against in-flight
       state - that is zelph's stratification rule and it is what makes
       "no defender remains" expressible. */
    ZELPH_EXPORT int32_t zelph_negate(zelph_engine* engine, zelph_node pattern, zelph_node* out_node);

    /* Does this fact exist? Creates nothing. */
    ZELPH_EXPORT int32_t zelph_exists(zelph_engine*     engine,
                                      zelph_node        subject,
                                      zelph_node        predicate,
                                      const zelph_node* objects,
                                      size_t            object_count,
                                      int32_t*          out_exists);

    /* Every object connected from `subject` through `predicate`, i.e. the
       objects of the facts (subject predicate X). The mirror of
       zelph_sources. */
    ZELPH_EXPORT int32_t zelph_targets(zelph_engine* engine,
                                       zelph_node    subject,
                                       zelph_node    predicate,
                                       zelph_node*   out_nodes,
                                       size_t*       count);

    /* An inference rule: when every condition holds, deduce every
       consequence. Returns the condition set. */
    ZELPH_EXPORT int32_t zelph_rule(zelph_engine*     engine,
                                    const zelph_node* conditions,
                                    size_t            condition_count,
                                    const zelph_node* consequences,
                                    size_t            consequence_count,
                                    zelph_node*       out_condition_set);

    /* Forward chaining. Facts and rules only take effect once the engine has
       run.
     *   run       - to a fixed point
     *   run_once  - a single pass
     *   run_delta - seeded by what was created since the previous run, so the
     *               cost follows the addition rather than the graph. That
     *               difference is what decides whether reasoning can happen
     *               inside a loop. */
    ZELPH_EXPORT int32_t zelph_run(zelph_engine* engine);
    ZELPH_EXPORT int32_t zelph_run_once(zelph_engine* engine);
    ZELPH_EXPORT int32_t zelph_run_delta(zelph_engine* engine);

    /* Whether the unification engine may spread a relation's candidates over
       worker threads. On by default, and the REPL has toggled it with
       `.parallel` since long before this header existed - a C caller could
       not reach it at all.

       It is a throughput/latency trade, not a correctness one: the derived
       facts are the same either way. Parallelism pays on a large graph and
       costs on a small one, where dispatch dominates the scan it replaces.
       A caller reasoning about many small fact bases in a loop - which is
       what `run_delta` and clusters are for - is exactly the case that wants
       it off.

       `enabled` is 0 or 1; `out_previous` may be null. */
    ZELPH_EXPORT int32_t zelph_set_parallel(zelph_engine* engine, int32_t enabled, int32_t* out_previous);

    /* Answer a query pattern - a fact containing variables.
     *
     * The bindings come back FLAT: `pairs` holds `2 * n` node ids per row,
     * alternating variable and bound value, and `row_sizes[i]` says how many
     * PAIRS row i contributed. Both counts are in/out as everywhere else, so
     * a call with capacity 0 asks for the sizes.
     *
     * The variable is reported as the NODE the caller created, not as a name,
     * so no string crosses the boundary and no lookup is needed to read the
     * answer. */
    ZELPH_EXPORT int32_t zelph_query(zelph_engine* engine,
                                     zelph_node    pattern,
                                     zelph_node*   pairs,
                                     size_t*       pair_count,
                                     size_t*       row_sizes,
                                     size_t*       row_count);

    /* Activate a named cluster, or deactivate tracking with a null name.
     *
     * Nodes CREATED while a cluster is active are recorded in it, which is
     * what makes dropping it a rollback - and what turns a monotonic graph
     * into a workspace. */
    ZELPH_EXPORT int32_t zelph_cluster(zelph_engine* engine, const char* name);

    /* The active cluster's name, or null in *out_name for the default. Free
       with zelph_string_free. */
    ZELPH_EXPORT int32_t zelph_cluster_active(zelph_engine* engine, char** out_name);

    /* Remove every node the cluster recorded, with its edges and names, and
       report how many went. Nodes that already existed when the cluster was
       activated were never recorded, so a drop cannot reach them. */
    ZELPH_EXPORT int32_t zelph_cluster_drop(zelph_engine* engine, const char* name, int64_t* out_removed);

    /* How many nodes a cluster holds, or -1 when there is no such cluster. */
    ZELPH_EXPORT int32_t zelph_cluster_count(zelph_engine* engine, const char* name, int64_t* out_count);

    /* -------------------------------------------------------------- networks */

    /* Compile a feed-forward view of the sub-graph spanned by the given
       layer nodes, input first, output last. At least two layers.
       `activation` is one of the zelph_activation values and applies to the
       hidden layers; the output layer is always linear. */
    ZELPH_EXPORT int32_t zelph_nn_compile(zelph_engine*     engine,
                                          const zelph_node* layers,
                                          size_t            layer_count,
                                          int32_t           activation,
                                          zelph_net*        out_handle);

    /* Fully connect two layers with raw synapses, weights drawn uniformly
       from [-scale, scale]. Existing synapses keep their weights, so the
       call is idempotent and trained weights survive re-wiring.
       `out_created` may be null. */
    ZELPH_EXPORT int32_t zelph_nn_connect_layers(zelph_engine* engine,
                                                 zelph_node    from_layer,
                                                 zelph_node    to_layer,
                                                 double        scale,
                                                 uint64_t      seed,
                                                 int64_t*      out_created);

    /* Forward pass with node-addressed multi-hot input, i.e. the input is
       the list of neurons that are active. `activations` may be null, which
       means every listed input is 1.0. Results are sorted by descending
       score, ties by ascending node. `top_k` < 0 returns the whole output
       layer. `out_scores` may be null if only the nodes are wanted.

       Safe to call concurrently with itself and with training. */
    ZELPH_EXPORT int32_t zelph_nn_eval_nodes(zelph_engine*     engine,
                                             zelph_net         handle,
                                             const zelph_node* input_nodes,
                                             const double*     input_activations,
                                             size_t            input_count,
                                             int32_t           top_k,
                                             zelph_node*       out_nodes,
                                             double*           out_scores,
                                             size_t*           count);

    /* One SGD step on a single node-addressed sample. Returns the loss
       BEFORE the update in `out_loss`, which may be null. */
    ZELPH_EXPORT int32_t zelph_nn_train_nodes(zelph_engine*     engine,
                                              zelph_net         handle,
                                              const zelph_node* input_nodes,
                                              const double*     input_activations,
                                              size_t            input_count,
                                              const zelph_node* target_nodes,
                                              const double*     target_activations,
                                              size_t            target_count,
                                              double            learning_rate,
                                              double*           out_loss);

    /* Write the compiled net's weights back into the graph's edge-weight
       store, which is what zelph_save then persists. */
    ZELPH_EXPORT int32_t zelph_nn_write_back(zelph_engine* engine, zelph_net handle);

    /* The shape of a snapshot: one element count per weight matrix, in the
       order zelph_nn_snapshot writes them. Their sum is the length
       zelph_nn_snapshot needs. */
    ZELPH_EXPORT int32_t zelph_nn_snapshot_shape(zelph_engine* engine, zelph_net handle, size_t* out_sizes, size_t* count);

    /* Copy the weights out, matrices concatenated in layer order. */
    ZELPH_EXPORT int32_t zelph_nn_snapshot(zelph_engine* engine, zelph_net handle, double* out_weights, size_t* count);

    /* Put a snapshot back. `sizes` describes how `weights` splits into
       matrices and must match the compiled net's shape. */
    ZELPH_EXPORT int32_t zelph_nn_restore(zelph_engine* engine,
                                          zelph_net     handle,
                                          const double* weights,
                                          size_t        weight_count,
                                          const size_t* sizes,
                                          size_t        size_count);

#ifdef __cplusplus
}
#endif

#endif /* ZELPH_C_H */
