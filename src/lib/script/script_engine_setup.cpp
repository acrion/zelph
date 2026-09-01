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

#include "script/script_engine_impl.hpp"

#include "network/reasoning.hpp"

#include <janet.h>

#include <string>

namespace zelph
{
    ScriptEngine::Impl::Impl(network::Reasoning* n) : _n(n)
    {
        s_instance = this;
    }

    ScriptEngine::Impl::~Impl()
    {
        if (s_instance == this) s_instance = nullptr;
        if (_janet_env)
        {
            for (auto& [kw, entry] : _keyword_handlers)
                janet_gcunroot(entry.handler);
            _keyword_handlers.clear();
            janet_gcunroot(_zelph_peg);
            if (_err_sink != nullptr) janet_gcunroot(janet_wrap_buffer(_err_sink));
            janet_deinit();
        }
    }

    void ScriptEngine::Impl::init()
    {
        _main_thread_id = std::this_thread::get_id();
        janet_init();
        _janet_env = janet_core_env(nullptr);
        _err_sink  = janet_buffer(256);
        janet_gcroot(janet_wrap_buffer(_err_sink));
        janet_setdyn("err", janet_wrap_buffer(_err_sink));
        register_zelph_functions();
        setup_module_paths();
        setup_script_runner();
        setup_peg();
        setup_numbers();
    }

    void ScriptEngine::Impl::register_zelph_functions() const
    {
// Helper to handle platform-specific Janet definitions
// On Linux/x64, it's a macro expecting void*.
// On macOS, it's a function expecting JanetCFunction.
#ifdef JANET_NANBOX_64
        auto wrap = [](JanetCFunction f)
        { return janet_wrap_cfunction((void*)f); };
#else
        auto wrap = [](JanetCFunction f)
        { return janet_wrap_cfunction(f); };
#endif
        janet_def(_janet_env, "zelph/fact", wrap((JanetCFunction)janet_cfun_zelph_fact), "(zelph/fact s p o)\nCreate fact.");
        janet_def(_janet_env, "zelph/refute", wrap((JanetCFunction)janet_cfun_zelph_refute), "(zelph/refute s p o)\nClaim that the fact does NOT hold: same node as zelph/fact, created with probability 0 so that Answer::is_wrong holds for it. This is what the statement \"¬(s p o)\" means outside a rule condition. Refused if the graph already claims the fact.");
        janet_def(_janet_env, "zelph/path-guard", wrap((JanetCFunction)janet_cfun_zelph_path_guard), "(zelph/path-guard from to)\nRefuse a path marker whose two ends are both concrete, outside a rule and outside a Janet block: reachability is walked, not asserted. Emitted by the \"P⁺\"/\"P∗\" sugar BEFORE the pattern is built, since building it is what would assert the one step.");

        janet_def(_janet_env, "zelph/list", wrap((JanetCFunction)janet_cfun_zelph_list), "(zelph/list nodes...)\nCreate list from nodes (a Lisp-style cons list with the first node as outermost cell).");

        janet_def(_janet_env, "zelph/list-chars", wrap((JanetCFunction)janet_cfun_zelph_list_chars), "(zelph/list-chars str)\nCreate list from string characters.\nCharacters are reversed before building the cons list so that the least-significant\ncharacter (rightmost in the string) is the outermost cons cell.\nThis matches the compact <...> syntax and enables LSB-first arithmetic via recursion.");
        janet_def(_janet_env, "zelph/set", wrap((JanetCFunction)janet_cfun_zelph_set), "(zelph/set nodes...)\nCreate a SET CONSTANT from elements, the `{...}` literal. Identified by its members, so the same elements always yield the same node, and membership cannot be extended.");

        janet_def(_janet_env, "zelph/collection", wrap((JanetCFunction)janet_cfun_zelph_collection), "(zelph/collection nodes...)\nCreate a COLLECTION from elements, the `@{...}` literal. A container with its own identity: two calls with the same elements yield two different nodes, and (member in collection) adds to it.");

        janet_def(_janet_env, "zelph/conjunction", wrap((JanetCFunction)janet_cfun_zelph_conjunction), "(zelph/conjunction conditions...)\nCreate a rule's condition set, the `(cond, cond, ...)` comma list: a collection tagged `~ conjunction`. "
                                                                                                       "Every condition must be readable as a fact pattern, or as a nested condition set; anything else is refused, "
                                                                                                       "because a rule holding it can never fire.");

        janet_def(_janet_env, "zelph/resolve", wrap((JanetCFunction)janet_cfun_zelph_resolve), "(zelph/resolve name &opt lang)\nResolve a string to its node, creating it if needed. "
                                                                                               "lang defaults to the current language (as set by .lang).");

        janet_def(_janet_env, "zelph/var", wrap((JanetCFunction)janet_cfun_zelph_var), "(zelph/var &opt name)\nCreate a FRESH variable node and return it. "
                                                                                       "A variable SYMBOL passed to zelph/fact is scoped to one evaluation of a Janet block; this node is a value, "
                                                                                       "so the caller's own binding decides how far it reaches -- which is how conditions built in separate blocks "
                                                                                       "join instead of forming a cross product. The optional name is display only (many variables may carry one name).");

        janet_def(_janet_env, "zelph/import", wrap((JanetCFunction)janet_cfun_zelph_import), "(zelph/import path & args)\nLoad and execute a script through the same machinery as the .import "
                                                                                             "command: the path is resolved against the working directory first, then the zelph standard library; "
                                                                                             "the .zph extension is optional. args are passed to the script as strings, available via (dyn :args). "
                                                                                             ".janet files are rejected (use Janet's import/use/dofile). Main thread only.");

        janet_def(_janet_env, "zelph/save", wrap((JanetCFunction)janet_cfun_zelph_save), "(zelph/save file)\nSave the current network to a binary file, like the .save command. "
                                                                                         "The filename must end with '.bin'. Main thread only.");

        janet_def(_janet_env, "zelph/load", wrap((JanetCFunction)janet_cfun_zelph_load), "(zelph/load file)\nLoad a saved network (.bin) or import a Wikidata JSON dump "
                                                                                         "(.json/.json.bz2, creates a .bin cache next to it), like the .load command. Main thread only.");

        janet_def(_janet_env, "zelph/run", wrap((JanetCFunction)janet_cfun_zelph_run), "(zelph/run)\nRun forward chaining to a fixed point, like the .run command. "
                                                                                       "Needed when driving zelph as a library: facts and rules created from Janet only take effect once the engine has run, "
                                                                                       "and outside the REPL neither .run nor auto-run is reachable. Returns nil. Main thread only.");

        janet_def(_janet_env, "zelph/run-once", wrap((JanetCFunction)janet_cfun_zelph_run_once), "(zelph/run-once)\nRun a single inference pass, like the .run-once command. "
                                                                                                 "Derives what one application of the rules yields instead of iterating to a fixed point. Returns nil. Main thread only.");

        janet_def(_janet_env, "zelph/run-delta", wrap((JanetCFunction)janet_cfun_zelph_run_delta), "(zelph/run-delta)\nRun inference seeded by the facts created since the previous run, like the .run-delta command. "
                                                                                                   "Costs time in the size of the addition rather than of the graph, which is what makes assert-then-reason loops practical. "
                                                                                                   "Requires an earlier run, an unchanged rule set and semi-naive evaluation; otherwise it falls back to a full pass. Returns nil. Main thread only.");

        janet_def(_janet_env, "zelph/cluster", wrap((JanetCFunction)janet_cfun_zelph_cluster), "(zelph/cluster &opt name)\nActivate a named cluster, or with nil / \"default\" deactivate cluster tracking; without an argument only report. "
                                                                                               "Returns the name of the cluster that is active afterwards, or nil for the default. "
                                                                                               "Nodes CREATED while a cluster is active are recorded in it, which is what makes zelph/cluster-drop a rollback. "
                                                                                               "Unlike the .cluster command this prints nothing, so it can be used per question inside a loop. Main thread only.");

        janet_def(_janet_env, "zelph/cluster-drop", wrap((JanetCFunction)janet_cfun_zelph_cluster_drop), "(zelph/cluster-drop name)\nRemove every node recorded in the cluster, with its edges and names, and return how many were removed. "
                                                                                                         "Nodes that already existed when the cluster was activated were never recorded, so a drop cannot remove them - which is what makes this safe as scratch space over a loaded graph. "
                                                                                                         "Facts OUTSIDE the cluster that referenced cluster nodes lose those connections. The default cluster cannot be dropped. Main thread only.");

        janet_def(_janet_env, "zelph/clusters", wrap((JanetCFunction)janet_cfun_zelph_clusters), "(zelph/clusters)\nReturn an array of [name node-count] tuples, one per existing cluster. Main thread only.");

        janet_def(_janet_env, "zelph/query", wrap((JanetCFunction)janet_cfun_zelph_query), "(zelph/query node)\nExecute a query and return results as an array of tables.\nEach table maps variable symbols to their bound zelph/node values.\nTakes a zelph/fact containing variables.");

        janet_def(_janet_env, "zelph/exists", wrap((JanetCFunction)janet_cfun_zelph_exists), "(zelph/exists s p o)\nCheck whether the fact was CLAIMED -- asserted or derived -- without creating it. Returns boolean.\n"
                                                                                             "A statement that only occurs as a rule's pattern is not claimed; ask zelph/mentioned for that.");

        janet_def(_janet_env, "zelph/mentioned", wrap((JanetCFunction)janet_cfun_zelph_mentioned), "(zelph/mentioned s p o)\nCheck whether the fact NODE is present in the graph, whether or not anybody claimed it.\n"
                                                                                                   "True for a rule's own conditions and consequences, which zelph/exists reports as absent. Use this to\n"
                                                                                                   "inspect rule structure; use zelph/exists to ask about the data.");

        janet_def(_janet_env, "zelph/name", wrap((JanetCFunction)janet_cfun_zelph_name), "(zelph/name node &opt lang)\nReturn the name of a node as a string, or nil if unnamed.");

        janet_def(_janet_env, "zelph/sources", wrap((JanetCFunction)janet_cfun_zelph_sources), "(zelph/sources predicate target)\nFind all subjects connected to target via predicate. Read-only.");

        janet_def(_janet_env, "zelph/targets", wrap((JanetCFunction)janet_cfun_zelph_targets), "(zelph/targets subject predicate)\nFind all objects connected from subject via predicate. Read-only.");

        janet_def(_janet_env, "zelph/negate", wrap((JanetCFunction)janet_cfun_zelph_negate), "(zelph/negate pattern)\nMark a fact pattern as negation. Returns the pattern node.\nEquivalent to (*(pattern) ~ negation) in zelph syntax.");

        janet_def(_janet_env, "zelph/rule", wrap((JanetCFunction)janet_cfun_zelph_rule), "(zelph/rule conditions & consequences)\nCreate an inference rule.\n"
                                                                                         "conditions: array of fact nodes (the conjunction).\n"
                                                                                         "consequences: one or more fact nodes to deduce.\n"
                                                                                         "Returns the condition set node.");

        janet_def(_janet_env, "zelph/dedup-rule", wrap((JanetCFunction)janet_cfun_zelph_dedup_rule),
                  "(zelph/dedup-rule thunk)\nRun a thunk that builds one rule and return the rule node. "
                  "If the graph already holds a rule that is the same up to renaming of its variables, "
                  "the newly built one is rolled back and the existing node returned instead. "
                  "Emitted automatically around every parsed `... => ...` statement; not needed in hand-written Janet.");

        janet_def(_janet_env, "zelph/car", wrap((JanetCFunction)janet_cfun_zelph_car), "(zelph/car cell)\nReturn the first element (car) of a cons cell, or nil if not a cons cell.");
        janet_def(_janet_env, "zelph/cdr", wrap((JanetCFunction)janet_cfun_zelph_cdr), "(zelph/cdr cell)\nReturn the rest (cdr) of a cons cell. Returns the nil node for the last cell.");

        janet_def(_janet_env, "zelph/register-keyword", wrap((JanetCFunction)janet_cfun_zelph_register_keyword), "(zelph/register-keyword keyword handler)\n(zelph/register-keyword open close handler)\n"
                                                                                                                 "Two-argument form: register a REPL syntax keyword. After the keyword is entered, subsequent "
                                                                                                                 "lines are accumulated verbatim until an empty line, then passed as a single string to handler.\n"
                                                                                                                 "Three-argument form: register an inline keyword (expression island). Whenever `open` occurs "
                                                                                                                 "inside a zelph statement, the text up to `close` is passed to handler, which must return a "
                                                                                                                 "zelph/node; the node replaces the island in the statement. Returning :incomplete extends the "
                                                                                                                 "island to the next occurrence of `close`, so nested delimiters work -- handlers must be free "
                                                                                                                 "of graph side effects until they accept their input.");

        janet_def(_janet_env, "zelph/closure", wrap((JanetCFunction)janet_cfun_zelph_closure), "(zelph/closure start predicate &opt include-start)\nTransitive closure following predicate "
                                                                                               "forward (subject to object). include-start true gives the reflexive closure (SPARQL *).");

        janet_def(_janet_env, "zelph/closure-sources", wrap((JanetCFunction)janet_cfun_zelph_closure_sources), "(zelph/closure-sources target predicate &opt include-target)\nTransitive closure following "
                                                                                                               "predicate backward (object to subject). include-target true gives the reflexive closure.");

        janet_def(_janet_env, "zelph/nn-connect", wrap((JanetCFunction)janet_cfun_zelph_nn_connect), "(zelph/nn-connect from to &opt weight)\nCreate a raw weighted edge (synapse) from -> to, creating nodes as needed. "
                                                                                                     "Synapses live solely in the weight store and never appear in the graph's adjacency, so they are invisible to the reasoning engine by construction; any node is a safe neuron, including fact nodes and structural numbers.");

        janet_def(_janet_env, "zelph/weight", wrap((JanetCFunction)janet_cfun_zelph_weight), "(zelph/weight from to)\n"
                                                                                             "Weight of the directed node pair from -> to. Returns the stored value if a "
                                                                                             "synapse (or an explicitly stored fact probability) exists for the pair; "
                                                                                             "1 if the pair is a real graph edge without a stored entry (the canonical "
                                                                                             "default, e.g. for facts asserted with probability 1); nil if neither exists.");

        janet_def(_janet_env, "zelph/set-weight", wrap((JanetCFunction)janet_cfun_zelph_set_weight), "(zelph/set-weight from to w)\nSet the weight of an existing synapse or edge.");

        janet_def(_janet_env, "zelph/nn-compile", wrap((JanetCFunction)janet_cfun_zelph_nn_compile), "(zelph/nn-compile layers &opt activation)\nCompile a feed-forward view of a sub-graph. layers: array of layer nodes, "
                                                                                                     "input first, output last. Neurons are the subjects of (neuron in layer) facts, ordered by node id. "
                                                                                                     "Returns an integer handle. The compiled net is a discardable cache; the graph stays the source of truth.");

        janet_def(_janet_env, "zelph/nn-nodes", wrap((JanetCFunction)janet_cfun_zelph_nn_nodes), "(zelph/nn-nodes handle layer)\nNeurons of a compiled layer in index order (defines input/output vector order).");

        janet_def(_janet_env, "zelph/nn-eval", wrap((JanetCFunction)janet_cfun_zelph_nn_eval), "(zelph/nn-eval handle inputs)\nForward pass; inputs/outputs are arrays of numbers in zelph/nn-nodes order. "
                                                                                               "Hidden layers use ReLU, the output layer is linear.");

        janet_def(_janet_env, "zelph/nn-train", wrap((JanetCFunction)janet_cfun_zelph_nn_train), "(zelph/nn-train handle inputs targets &opt learning-rate)\nOne SGD step on a single sample; returns the loss "
                                                                                                 "(0.5 * sum of squared errors) before the update. learning-rate defaults to 0.01.");

        janet_def(_janet_env, "zelph/nn-write-back", wrap((JanetCFunction)janet_cfun_zelph_nn_write_back), "(zelph/nn-write-back handle)\nWrite the compiled net's weights back into the graph's edge-weight store, "
                                                                                                           "so they survive .save and are picked up by future zelph/nn-compile calls.");

        janet_def(_janet_env, "zelph/nn-snapshot", wrap((JanetCFunction)janet_cfun_zelph_nn_snapshot), "(zelph/nn-snapshot handle)\nCopy the compiled net's weights out as an array of arrays of numbers, "
                                                                                                       "one per layer transition, each row-major by post-synaptic unit: input i to unit j is at (+ (* j n-pre) i). "
                                                                                                       "Use it to keep the best epoch of a training run: the criterion that says a run has passed its "
                                                                                                       "optimum can only fire afterwards, so without a snapshot the saved weights are always some epochs past the good ones.");

        janet_def(_janet_env, "zelph/nn-restore", wrap((JanetCFunction)janet_cfun_zelph_nn_restore), "(zelph/nn-restore handle snapshot)\nPut a zelph/nn-snapshot back into the compiled net. "
                                                                                                     "Shapes must match; synapses absent from the graph stay absent, since the mask belongs to the graph and not to the weights.");

        janet_def(_janet_env, "zelph/nn-connect-layers", wrap((JanetCFunction)janet_cfun_zelph_nn_connect_layers), "(zelph/nn-connect-layers from-layer to-layer &opt scale seed)\nCreate raw synapses between all members of two layers "
                                                                                                                   "((neuron in layer) facts, ascending node id). Weights are uniform in [-scale, scale]; scale defaults to 0.1, scale 0 gives exact zeros. "
                                                                                                                   "seed defaults to 42 for reproducible initialization. Existing edges are left untouched, so trained weights survive re-wiring. "
                                                                                                                   "Returns the number of edges created.");

        janet_def(_janet_env, "zelph/nn-train-nodes", wrap((JanetCFunction)janet_cfun_zelph_nn_train_nodes), "(zelph/nn-train-nodes handle inputs targets &opt learning-rate)\nOne SGD step, addressing neurons by node instead of by index. "
                                                                                                             "inputs/targets are arrays whose elements are nodes (activation 1) or [node activation] pairs; all other neurons are 0. "
                                                                                                             "A typical call encodes one fact: inputs [S P], targets [O]. Returns the loss before the update. learning-rate defaults to 0.01.");

        janet_def(_janet_env, "zelph/nn-eval-nodes", wrap((JanetCFunction)janet_cfun_zelph_nn_eval_nodes), "(zelph/nn-eval-nodes handle inputs &opt top-k)\nForward pass with node-addressed multi-hot input. Returns an array of [node score] "
                                                                                                           "tuples for the output layer, sorted by descending score (ties by ascending node id), limited to top-k if given.");

        janet_def(_janet_env, "zelph/approx", wrap((JanetCFunction)janet_cfun_zelph_approx), "(zelph/approx pattern net-name)\nTag a fact pattern as a neural rule condition: creates (pattern nn net). "
                                                                                             "Desugared form of ≈net(pattern). Returns the pattern node.");

        janet_def(_janet_env, "zelph/path", wrap((JanetCFunction)janet_cfun_zelph_path), "(zelph/path pattern mode)\nTag a one-step fact pattern as a transitive path condition: creates "
                                                                                         "(pattern closure mode), where mode is \"one-or-more\" (P⁺) or \"zero-or-more\" (P∗). Desugared form of "
                                                                                         "(X P⁺ Y). Returns the tag node, which is what a rule uses as its condition.");

        janet_def(_janet_env, "zelph/set-number-digits", wrap((JanetCFunction)janet_cfun_zelph_set_number_digits), "(zelph/set-number-digits digits)\nRegister the digit alphabet of the loaded number representation, as an "
                                                                                                                   "array of digit nodes or names in ascending order of value (e.g. [\"0\" \"1\"] for binary). "
                                                                                                                   "node_to_string then displays every nil-terminated cons list consisting solely of these digit "
                                                                                                                   "nodes as a decimal &-literal -- the inverse of the &-input syntax (zelph/number). All other "
                                                                                                                   "cons lists keep the generic <...> display. An empty array disables the feature.");

        janet_def(_janet_env, "zelph/no-selffact-sugar", wrap((JanetCFunction)janet_cfun_zelph_no_selffact_sugar), "(zelph/no-selffact-sugar preds...)\nExclude predicates from the self-fact display sugar: facts (X pred X) "
                                                                                                                   "with a registered predicate always render verbose as \"X pred X\", never as \":pred X\". "
                                                                                                                   "Additive across calls, so stacked modules can each register their own operators. "
                                                                                                                   "Input sugar (\":pred X\") keeps working regardless. Intended for term-forming "
                                                                                                                   "operators (+, eml, nand, ...), where subject == object is a hash-consing "
                                                                                                                   "coincidence rather than a request marker.");

        janet_def(_janet_env, "zelph/register-display-scheme", wrap((JanetCFunction)janet_cfun_zelph_register_display_scheme), "(zelph/register-display-scheme name open close &opt options)\nDeclare how this script's own notation is written, so node_to_string can "
                                                                                                                               "render terms the way the script's parser reads them back. open/close enclose a rendering that DEVIATES from the default form "
                                                                                                                               "(elided parentheses, a different numeral prefix); they are emitted verbatim, so use \"$( \" / \" )\" for padded output. "
                                                                                                                               "options is a struct: :numeral-prefix replaces the default \"&\" of number literals inside the scheme; :name-first and "
                                                                                                                               ":name-chars declare which characters a leaf name may start with and consist of. A term containing anything the scheme cannot "
                                                                                                                               "write -- a foreign predicate, a set, a name outside that grammar -- is rendered in the default form instead, so the output "
                                                                                                                               "always stays re-readable. Re-registering a name updates the scheme.");

        janet_def(_janet_env, "zelph/set-infix-display", wrap((JanetCFunction)janet_cfun_zelph_set_infix_display), "(zelph/set-infix-display scheme entries)\nRegister infix operators into a scheme declared by zelph/register-display-scheme. "
                                                                                                                   "entries is an array of [predicate precedence &opt associativity]; associativity is :left (default), :right or :none. Higher "
                                                                                                                   "precedence binds tighter; node_to_string omits the parentheses around an operand whose operator binds tightly enough. Additive "
                                                                                                                   "across calls. A predicate already claimed by any scheme is an error -- otherwise a term's rendering would depend on load order. "
                                                                                                                   "Registered operators are excluded from the self-fact display sugar (see zelph/no-selffact-sugar).");

        janet_def(_janet_env, "zelph/set-application-display", wrap((JanetCFunction)janet_cfun_zelph_set_application_display), "(zelph/set-application-display scheme predicates)\nRegister predicates whose facts are written in call notation: (S P O) renders as "
                                                                                                                               "\"S(O)\", and the predicate name does not appear. The result is self-delimiting -- it never takes parentheses, and its "
                                                                                                                               "argument never needs any. The head S must render as a bare name matching the scheme's leaf grammar; a composite head has "
                                                                                                                               "no call notation, so such a term falls back to the default rendering. Shares the one-scheme-per-predicate namespace with "
                                                                                                                               "zelph/set-infix-display, and excludes the predicates from the self-fact display sugar.");

        janet_def(_janet_env, "zelph/out", wrap((JanetCFunction)janet_cfun_zelph_out), "(zelph/out text)\nEmit text through zelph's output pipeline (Out channel). Unlike Janet's "
                                                                                       "print (raw stdout), the text reaches the REPL, the playground and test collectors, and is "
                                                                                       "not subject to the input-echo suppression inside imported scripts -- use it for import-time notices.");
    }

    void ScriptEngine::Impl::setup_module_paths() const
    {
        const char* code = R"janet(
            (when-let [jp (os/getenv "JANET_PATH")]
              (each p (string/split (if (= :windows (os/which)) ";" ":") jp)
                (when (and p (not= p ""))
                  (array/push module/paths [(string p "/:all:.jimage") :image])
                  (array/push module/paths [(string p "/:all:.janet") :source])
                  (array/push module/paths [(string p "/:all:/init.janet") :source])
                  (array/push module/paths [(string p "/:all:.so") :native]))))
        )janet";
        Janet       out;
        janet_dostring(_janet_env, code, "module-paths", &out);
        flush_err_trace();
    }

    void ScriptEngine::Impl::setup_script_runner() const
    {
        // janet-CLI-compatible script runner: evaluate the file in a fresh
        // environment (inheriting the zelph bindings via the core env
        // prototype chain), then call its main function - if defined - with
        // the script path followed by the arguments.
        const char* code = R"janet(
                (defn zelph/run-script
                  `Run a Janet source file the way the janet CLI would: evaluate it in a fresh environment and call its main function (if defined) with the script path and arguments. Relative imports such as (use ./foo) resolve against the script's directory.`
                  [path & args]
                  # Fresh-process semantics per run: require caches modules
                  # process-wide, so without this, edits to files pulled in via
                  # (use ./foo) would be invisible to a repeated .import within
                  # the same session.
                  (loop [k :in (keys module/cache)]
                    (put module/cache k nil))
                  (def env (make-env))
                  (def subargs [path ;args])
                  (put env *args* subargs)
                  (dofile path :env env)
                  (when-let [entry (get env 'main)
                             main (or (get entry :value) (get (get entry :ref) 0))]
                    (when (function? main)
                      (main ;subargs)))
                  nil)
            )janet";

        Janet out;
        int   status = janet_dostring(_janet_env, code, "script-runner", &out);
        flush_err_trace();
        if (status != JANET_SIGNAL_OK) janet_stacktrace(nullptr, out);
    }

    void ScriptEngine::Impl::setup_peg()
    {
        // zelph Grammar:
        // 1. :atom -> Alphanumeric or Symbols (excluding reserved)
        // 2. :list-compact -> <123> Compact list: split into individual chars
        // 3. :list-nodes -> < a b > Node list: space-separated elements
        // 4. :set -> { ... }
        // 5. :nested -> ( ... ) Recursive statements inside ( ... )
        // 6. :quoted -> "..."
        // 7. :focused -> *Element (Returns the element instead of the container)
        // 8. :unquote -> ,identifier (Reference to a Janet variable)
        // 9. :selffact -> :pred X (self-fact sugar: desugars to (X pred X))
        // Returns tagged tuples like [:atom "val"], [:list-compact "val"] or [:nested sub-stmt...] for C++ processing
        std::string peg_setup = R"zph(
            (def zelph-grammar
              ~{:ws (set " \t\r\f\n\0\v")
                :s* (any :ws)
                :s+ (some :ws)

                # > and < are reserved to act as delimiters.
                # , is reserved for unquoting Janet variables.
                # To use them as atoms, we define specific rules below.
                :reserved (set " \t\r\n\0\v<\"(){}*>,¬")

                # Identifiers
                :symchars (if-not :reserved 1)
                :var-underscore (* "_" (any :symchars))
                :var-uppercase  (* (range "AZ") (not :symchars))

                # A variable must start with underscore or be a single uppercase letter
                :var-token (choice :var-underscore :var-uppercase)

                # Atoms
                # A quoted atom knows exactly two escapes, \" and \\, so that
                # every name is writable: without them a Wikidata label
                # carrying a quote could not be entered at all, and the line
                # zelph printed for it read back as several atoms. A
                # backslash in front of anything else stays an ordinary
                # character (Windows paths, LaTeX).
                :quoted (capture (* "\"" (any (choice (* "\\" 1) (if-not "\"" 1))) "\""))

                # Normal atoms (sequences of non-reserved chars)
                :raw-atom (capture (some :symchars))

                # Multi-char Arrows containing reserved chars (must be checked before raw-atom/ops)
                :arrow-multi (capture (choice "=>" "->" "-->" "<=>" "<=" ">="))

                # Single-char Operators (from reserved set)
                :op-single (capture (choice ">" "<"))

                # Structure Tags
                :tag-var    (group (* (constant :var)  (capture :var-token)))

                # Unquote: ,identifier references a Janet variable
                :tag-unquote (group (* (constant :unquote) "," (capture (some :symchars))))

                # Neural condition sugar: ≈net(pattern). Syntax only -- desugars to
                # (zelph/approx pattern "net"), which tags the pattern in the graph.
                :tag-approx (group (* (constant :approx) "≈" (capture (some :symchars)) :s* :val-any))

                # Number literal: &<token>. Syntax only -- the interpretation is
                # delegated to the redefinable Janet function zelph/number, so the
                # internal number representation is defined by scripts, not by C++.
                # (& as prefix is a nod to BBC BASIC / Amstrad CPC number literals.)
                :tag-number (group (* (constant :number) "&" (capture (some :symchars))))

                # Self-fact sugar: :pred X. Syntax only -- desugars to the
                # self-fact (X pred X), the stdlib marker idiom (:simplify T
                # for (T simplify T)). ':' stays an ordinary symchar
                # elsewhere, so atoms with inner colons (URLs, wd:Q5) are
                # unaffected; only a LEADING colon on a value position
                # triggers the sugar. The predicate is a single plain token;
                # a variable token (e.g. :R) keeps variable semantics.
                :tag-selffact (group (* (constant :selffact) ":" (capture (some :symchars)) :s* :val-any))

                # Atom Definition Order:
                # 1. Quoted (always safe)
                # 2. Multi-char arrows (e.g. "=>"). Must be before raw-atom because "=" is a symchar.
                # 3. Raw atoms (e.g. "abc", "=")
                # 4. Single ops (e.g. ">"). Checked last to prefer longer matches or delimiters.
                :tag-atom   (group (* (constant :atom) (choice :quoted :arrow-multi :raw-atom :op-single)))

                :star-atom  (group (* (constant :atom) (capture "*")))

                # 1. Compact List: <abc> — no spaces between chars, split into individual character nodes.
                #    Characters are stored reversed internally (LSB-first for numbers).
                :tag-list-compact (group (* (constant :list-compact) (* "<" (capture (some (if-not (set "> \t\r\n") 1))) ">")))

                # Recursive definitions need forward declaration in PEG if simple recursive descent isn't enough,
                # but Janet PEG handles this via the :val-any choice reference.

                # Focused Value: *Value (e.g. *A or *{...} or *(...))
                # Returns [:focused value-node]
                :tag-focused (group (* (constant :focused) "*" :val-any))

                # Negation sugar: ¬Value  (e.g. ¬(A is green))
                # Returns [:negation value-node]
                :tag-negation (group (* (constant :negation) "¬" :s* :val-any))

                # Conjunction sugar: comma-separated conditions inside parentheses
                :conj-cond (group (* (constant :condition) :val-any (any (sequence :s+ :val-any))))
                :comma-sep (* :s* "," (not :symchars) :s*)

                # Nested Facts: ( A B C )
                :tag-nested (choice
                              (group (* (constant :conjunction) "(" :s* :conj-cond (some (* :comma-sep :conj-cond)) :s* ")"))
                              (group (* (constant :nested) "(" :s* :stmt-any :s* ")")))

                # Sets: { A B C }
                :set-content (any (sequence :s* :val-any))
                :tag-set    (group (* (constant :set) "{" :set-content :s* "}"))

                # Collection literal: @{...}. A container with its own
                # identity whose membership can grow, as opposed to the set
                # constant {...} which IS its members. The marker follows
                # Janet, where {...} is the immutable struct and @{...} the
                # mutable table -- and it costs no reserved character: "@"
                # stays an ordinary symchar, only "@{" is special.
                :tag-collection (group (* (constant :collection) "@{" :set-content :s* "}"))

                # 2. Node List: < a b > — space-separated, stored as cons list (last element outermost).
                #    The user writes elements in the order they should be displayed; node_to_string reverses
                #    the internal order back for output. For numbers, write digits in reverse: <3 2 1>
                #    represents the number 123 (same internal form as the compact <123>).
                # The loop (if-not ">" :val-any) ensures we don't consume the closing delimiter.
                :list-content (any (sequence :s* (if-not ">" :val-any)))
                :tag-list-nodes (group (* (constant :list-nodes) (* "<" :list-content :s* ">")))

                # Value order:
                # Check lists first so "<" starts a list if possible.
                :val-any (choice :tag-focused :tag-negation :tag-approx :tag-selffact :tag-var :tag-unquote :tag-number :tag-list-compact :tag-list-nodes :tag-collection :tag-atom :star-atom :tag-nested :tag-set)

                # A statement is a sequence of values separated by whitespace
                # Used inside ( ... ) and at top level for facts
                :stmt-any (sequence :val-any (any (sequence :s+ :val-any)))

                # Top Level Parsing
                # Everything is captured into a :root group, or a :conjunction if comma separated.
                # C++ logic decides if it's a single value or a fact (S P O) based on element count.
                :main (sequence :s* (choice
                                        (group (* (constant :conjunction) :conj-cond (some (* :comma-sep :conj-cond))))
                                        (group (* (constant :root) :stmt-any))) :s* -1)})

            (defn zelph-safe-parse [peg text]
               (peg/match peg text))
        )zph";

        Janet out;
        int   status = janet_dostring(_janet_env, peg_setup.c_str(), "setup", &out);
        flush_err_trace();
        if (status != JANET_SIGNAL_OK) janet_stacktrace(nullptr, out);

        janet_dostring(_janet_env, "(def zelph-peg (peg/compile zelph-grammar))", "init", &out);
        flush_err_trace();
        _zelph_peg = out;
        janet_gcroot(_zelph_peg);
    }

    void ScriptEngine::Impl::setup_numbers() const
    {
        const char* code = R"janet(
            (defn zelph/number
              "Fallback for &-literals: no number representation is loaded."
              [s]
              (error (string "number literal &" s " has no representation - "
                             "load a script that defines zelph/number "
                             "(e.g. decimal-arithmetic.zph or binary-arithmetic.zph)")))
        )janet";

        Janet out;
        janet_dostring(_janet_env, code, "default-numbers", &out);
        flush_err_trace();
    }
}
