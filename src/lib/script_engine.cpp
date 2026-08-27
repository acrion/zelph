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

#include "script_engine.hpp"
#include "network/fact_structure.hpp"
#include "network/neural.hpp"
#include "network/reasoning.hpp"
#include "network/rule_identity.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"

#include <janet.h>

#include <algorithm>
#include <filesystem>
#include <janetconf.h>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace zelph;

// True if a PEG-AST node is the literal atom `text`. Used to recognise `=>`
// in predicate position, i.e. that a statement defines a rule.
static bool is_atom(Janet node, const char* text)
{
    const Janet* data;
    int32_t      len;
    if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
    if (!janet_checktype(data[0], JANET_KEYWORD)) return false;
    if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) != "atom") return false;

    if (janet_checktype(data[1], JANET_STRING))
        return std::string(reinterpret_cast<const char*>(janet_unwrap_string(data[1]))) == text;
    if (janet_checktype(data[1], JANET_BUFFER))
    {
        const JanetBuffer* b = janet_unwrap_buffer(data[1]);
        return std::string(reinterpret_cast<const char*>(b->data), b->count) == text;
    }
    return false;
}

// True if a PEG-AST node is a `¬` pattern, through any number of plain
// groupings -- "¬(F)" and "(¬(F))" are the same statement.
//
// The question has to be asked of the SYNTAX, not of the resulting node: the
// negation tag is a fact about the pattern node, and a ground pattern is
// hash-consed, so a node negated in one rule carries the tag everywhere. Only
// the AST knows which statement wrote the "¬".
static bool is_negation_ast(Janet node)
{
    const Janet* data;
    int32_t      len;
    if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
    if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

    const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
    if (type == "negation") return true;
    if (type == "nested" && len == 2) return is_negation_ast(data[1]);
    return false;
}

// Does a `¬` stand anywhere inside this AST? is_negation_ast asks whether an
// argument IS one; a plain statement has to ask whether one is buried in it,
// because the operator is dropped at whatever depth it sits: "x q (¬(a p b))"
// and "x q (y r (¬(a p b)))" both build the operand with zelph/fact and then
// tag it, so both assert the very fact the "¬" denies.
//
// Asked of the SYNTAX for the same reason is_negation_ast is, and the walk
// is over children rather than over the known tag names on purpose: a value
// form added to the grammar later must not open the hole again by not being
// listed.
static bool contains_negation_ast(Janet node)
{
    const Janet* data;
    int32_t      len;
    if (!janet_indexed_view(node, &data, &len) || len < 1) return false;

    if (janet_checktype(data[0], JANET_KEYWORD)
        && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) == "negation")
        return true;

    for (int32_t i = 1; i < len; ++i)
        if (contains_negation_ast(data[i])) return true;

    return false;
}

// The same question for the neural condition. `≈` is written as a prefix on a
// value, so the AST names it directly.
static bool is_approx_ast(Janet node)
{
    const Janet* data;
    int32_t      len;
    if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
    if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

    const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
    if (type == "approx") return true;
    if (type == "nested" && len == 2) return is_approx_ast(data[1]);
    return false;
}

// --- Implementation Class ---

class ScriptEngine::Impl
{
public:
    static Impl* s_instance; // Required for static Janet C-function callbacks

    network::Reasoning* _n;
    JanetTable*         _janet_env = nullptr;
    Janet               _zelph_peg{};
    bool                _log_janet_functions = false;

    // Set while zelph/dedup-rule runs its thunk: the facts built there are a
    // rule's patterns, not claims, so zelph/fact must not revoke a
    // pattern marking then.
    bool _building_rule = false;

    // Set while a user's Janet BLOCK runs -- `%(...)` in the REPL or in a
    // script. Only there is a zelph/fact call a statement of its own and
    // therefore a claim. Everywhere else the same call builds part of
    // something else: the subterms of a parsed statement, the fact pattern a
    // command like .explain evaluates read-only, the term an inline keyword
    // island returns. Naming a statement is not claiming it, so the
    // revocation in zelph/fact asks for this flag rather than for the
    // absence of one of those contexts.
    bool _in_janet_block = false;

    // Set while a command RESOLVES a printed pattern to the node it denotes --
    // ".explain (a p b)", ".prune-facts (a p b)", ".node a p b". The code is
    // generated the same way a statement's is, so it runs through zelph/fact,
    // which is the assertion API; the scratch cluster around it is what has
    // always kept the assertion from surviving.
    //
    // That was enough until a fact could be REFUTED. Zelph::fact refuses to
    // claim a fact the graph holds as known-wrong -- rightly -- so the moment
    // ¬(a p b) became writable, every command that addresses a pattern by
    // printing it back answered "Unknown node" for exactly the facts a user
    // has most reason to look at. Under this flag the pattern is LOOKED UP
    // instead of asserted -- exactly, and only where the graph holds it; see
    // the note at the lookup itself for why both halves of that matter.
    bool _resolving_pattern = false;

    // Save/restore around a nested evaluation (an .import inside a block, a
    // keyword handler): a plain assignment would leak the inner context.
    struct BlockScope
    {
        BlockScope(bool& flag, const bool value)
            : _flag(flag)
            , _saved(flag)
        {
            _flag = value;
        }
        ~BlockScope() { _flag = _saved; }
        bool&      _flag;
        const bool _saved;
    };

    // A registered syntax keyword. Two kinds share this entry, the
    // registration API (zelph/register-keyword) and the handler protocol
    // (text in, :incomplete veto, result out):
    //   - block keywords (inline_mode == false): line-based. Detected as the
    //     first token of a REPL/script line, accumulated until an empty line
    //     (see Interactive::process). The handler result is printed.
    //   - inline keywords (inline_mode == true): expression islands. Whenever
    //     the opening delimiter (the map key) occurs inside a zelph statement,
    //     the text up to `close` is passed to the handler, which must return a
    //     zelph/node; the node replaces the island in the statement (spliced
    //     via the unquote mechanism). :incomplete extends the island to the
    //     next occurrence of `close`, so nested delimiters work without the
    //     host knowing the island's grammar.
    struct KeywordEntry
    {
        Janet       handler{};
        bool        inline_mode = false;
        std::string close; // inline mode only
    };
    std::map<std::string, KeywordEntry> _keyword_handlers;

    // True while inline-keyword expansion has already prepared (cleared) the
    // scoped-variable map for the statement being processed; process_janet
    // and evaluate_expression then skip their own clear exactly once, so
    // island handlers and the surrounding statement share one variable scope.
    bool _scoped_vars_preloaded = false;

    enum class HandlerCall
    {
        Dispatched,
        Incomplete
    };

    // Call a Janet function on a fiber of its own, with that fiber rooted for
    // the duration of the call. Use this instead of janet_pcall anywhere the
    // call can happen while the VM is already running -- which is every call
    // site here, because zelph/import and zelph/run-script let a Janet script
    // re-enter statement processing.
    //
    // janet_pcall does NOT root the fiber it creates, and janet_collect marks
    // only janet_vm.root_fiber plus the explicit roots, reaching nested fibers
    // through the parent's `child` link -- which janet_pcall does not set.
    // janet_continue_no_check adopts a fiber as root_fiber only while there is
    // none, so a nested call runs on a fiber that no root can reach: the first
    // collection inside it frees the fiber that is currently running, and
    // everything read afterwards is a use-after-free. The crash then surfaces
    // wherever the callee next touches its own stack, arbitrarily far from
    // here, and moves or disappears with any change to allocation timing.
    //
    // Found as a segfault on `.import decimal-arithmetic` followed by
    // `.import symbolic-core`: building a rule allocates enough to trigger a
    // collection inside the thunk. run_janet_script roots its fiber by hand
    // for exactly this reason.
    static JanetSignal pcall_rooted(JanetFunction* const fun, const int32_t argc, const Janet* const argv, Janet* const out)
    {
        // Nothing is rooted between creating the fiber and rooting it, so no
        // allocation may happen in between either.
        const int         gc_handle = janet_gclock();
        JanetFiber* const fiber     = janet_fiber(fun, 64, argc, argv);
        if (!fiber)
        {
            janet_gcunlock(gc_handle);
            *out = janet_cstringv("could not create fiber");
            return JANET_SIGNAL_ERROR;
        }
        janet_gcroot(janet_wrap_fiber(fiber));
        janet_gcunlock(gc_handle);

        const JanetSignal sig = janet_continue(fiber, janet_wrap_nil(), out);

        // `out` is reachable through the fiber (last_value), so the caller
        // must not allocate before reading it -- the same contract janet_pcall
        // has, and the reason run_janet_script extracts its error text first.
        janet_gcunroot(janet_wrap_fiber(fiber));
        return sig;
    }

    // Shared invocation for both keyword kinds: pcall with error wrapping and
    // the :incomplete veto protocol. Under force a veto is an error (EOF in
    // scripts for block keywords; see expand_inline_keywords for islands).
    HandlerCall call_keyword_handler(const std::string& name, const Janet handler, const std::string& text, const bool force, Janet& result)
    {
        JanetFunction* f   = janet_unwrap_function(handler);
        Janet          arg = janet_cstringv(text.c_str());

        if (pcall_rooted(f, 1, &arg, &result) != JANET_SIGNAL_OK)
        {
            std::string err = "Janet error in handler for keyword '" + name + "'";
            if (janet_checktype(result, JANET_STRING))
                err += ": " + std::string(reinterpret_cast<const char*>(janet_unwrap_string(result)));
            else if (janet_checktype(result, JANET_BUFFER))
            {
                JanetBuffer* b = janet_unwrap_buffer(result);
                err += ": " + std::string(reinterpret_cast<const char*>(b->data), b->count);
            }
            throw std::runtime_error(err);
        }

        if (janet_checktype(result, JANET_KEYWORD))
        {
            const uint8_t* kw = janet_unwrap_keyword(result);
            if (std::string(reinterpret_cast<const char*>(kw)) == "incomplete")
            {
                if (!force) return HandlerCall::Incomplete;
                throw std::runtime_error("Keyword block for '" + name + "' is incomplete");
            }
        }

        return HandlerCall::Dispatched;
    }

    // Expand inline keywords ("expression islands") in a zelph statement.
    // Runs in parse_zelph_to_janet BEFORE the PEG, so every consumer of zelph
    // syntax (REPL statements, imported scripts, command patterns) gets the
    // expansion. Each island's handler receives the raw text between the
    // delimiters and must return a zelph/node; the node is bound to a fresh
    // Janet name and spliced back via the existing unquote mechanism
    // (",$inlineN"), which makes islands valid in every value position.
    //
    // Close-delimiter search is a raw substring scan: the HANDLER owns the
    // island's grammar and arbitrates false splits via the :incomplete veto
    // (a ')' nested inside the island extends it to the next ')'). Handlers
    // must therefore be side-effect-free until they accept their input --
    // the contract sparql.zph already follows.
    //
    // Openers are not matched inside quoted atoms or comments (mirroring
    // is_zelph_complete's scanning rules).
    std::string expand_inline_keywords(const std::string& input)
    {
        _scoped_vars_preloaded = false;

        std::vector<std::pair<const std::string*, const KeywordEntry*>> inline_kws;
        for (const auto& [open, entry] : _keyword_handlers)
            if (entry.inline_mode) inline_kws.push_back({&open, &entry});
        if (inline_kws.empty()) return input;

        std::string out;
        out.reserve(input.size());

        bool   in_string  = false;
        bool   escape     = false;
        bool   in_comment = false;
        size_t island     = 0;

        size_t i = 0;
        while (i < input.size())
        {
            const char c = input[i];

            if (in_comment)
            {
                if (c == '\n') in_comment = false;
                out += c;
                ++i;
                continue;
            }
            if (escape)
            {
                escape = false;
                out += c;
                ++i;
                continue;
            }
            if (in_string)
            {
                if (c == '\\')
                    escape = true;
                else if (c == '"')
                    in_string = false;
                out += c;
                ++i;
                continue;
            }
            if (c == '#')
            {
                in_comment = true;
                out += c;
                ++i;
                continue;
            }
            if (c == '"')
            {
                in_string = true;
                out += c;
                ++i;
                continue;
            }

            // Longest matching opener at this position wins.
            const std::string*  open  = nullptr;
            const KeywordEntry* entry = nullptr;
            for (const auto& [o, e] : inline_kws)
            {
                if (input.compare(i, o->size(), *o) == 0 && (!open || o->size() > open->size()))
                {
                    open  = o;
                    entry = e;
                }
            }
            if (!open)
            {
                out += c;
                ++i;
                continue;
            }

            if (!_scoped_vars_preloaded)
            {
                // One shared variable scope for the whole statement: a variable
                // created inside an island must unify with the same name in the
                // surrounding statement (rule authoring). Cleared here, at the
                // true start of statement processing; process_janet skips its
                // own clear once (_scoped_vars_preloaded).
                {
                    std::lock_guard<std::mutex> lock(_state_mutex);
                    _scoped_variables.clear();
                }
                _scoped_vars_preloaded = true;
            }

            const std::string& close         = entry->close;
            const size_t       content_begin = i + open->size();

            size_t k = input.find(close, content_begin);
            if (k == std::string::npos)
                throw std::runtime_error("Inline keyword '" + *open + "': missing closing delimiter '" + close + "'");

            Janet result = janet_wrap_nil();
            while (true)
            {
                const std::string inner = input.substr(content_begin, k - content_begin);
                if (call_keyword_handler(*open, entry->handler, inner, false, result) == HandlerCall::Dispatched)
                    break;

                k = input.find(close, k + close.size());
                if (k == std::string::npos)
                    throw std::runtime_error("Inline keyword '" + *open + "': handler reports incomplete input and no further '" + close + "' follows");
            }

            const network::Node n = zelph_unwrap_node(result);
            if (!n)
                throw std::runtime_error("Inline keyword '" + *open + "': handler must return a zelph/node");

            const std::string name = "$inline" + std::to_string(island++);
            janet_def(_janet_env, name.c_str(), result, "inline keyword expansion result");
            out += "," + name;

            i = k + close.size();
        }

        return out;
    }

    // Compiled neural networks (session-scoped caches, discarded on .reset).
    // Handles handed to Janet are indexes into this vector.
    std::vector<std::unique_ptr<network::NeuralNet>> _neural_nets;

    network::NeuralNet* get_net(int32_t handle)
    {
        // The returned pointer stays valid after unlocking: the vector owns
        // the nets via unique_ptr and entries are never removed during a
        // session, so only the vector itself needs protection (push_back may
        // reallocate the vector's buffer concurrently).
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (handle < 0 || static_cast<size_t>(handle) >= _neural_nets.size()) return nullptr;
        return _neural_nets[static_cast<size_t>(handle)].get();
    }

    // Track variables used in the current scope/statement
    std::map<std::string, network::Node> _scoped_variables;

    // Memoized rule fingerprints, see find_duplicate_rule. Keyed by node,
    // which is a structural hash, so an entry can never go stale. 0 means
    // "not a rule".
    std::unordered_map<network::Node, std::size_t> _rule_shapes;

    // Guards the script engine's own bookkeeping (_scoped_variables,
    // _neural_nets) against concurrent access from Janet threads
    // (ev/spawn-thread). Calls INTO the reasoning engine are synchronized
    // by zelph itself and are not covered here.
    std::mutex _state_mutex;

    // Set by Interactive; backs the Janet function zelph/import.
    ImportHandler _import_handler;

    // Set by Interactive; backs zelph/save and zelph/load.
    CommandHandler _command_handler;

    // See ScriptEngine::set_echo_predicate.
    EchoPredicate _echo_predicate;

    bool echo_enabled() const
    {
        return !_echo_predicate || _echo_predicate();
    }

    // The thread that owns _janet_env. zelph/import must run here: the REPL
    // pipeline it delegates to executes Janet code in the main VM, which is
    // not usable from other Janet threads (each has its own VM).
    std::thread::id _main_thread_id;

    void clear_scoped_variables()
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        _scoped_variables.clear();
    }

    bool has_scoped_variables()
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        return !_scoped_variables.empty();
    }

    explicit Impl(network::Reasoning* n)
        : _n(n)
    {
        s_instance = this;
    }

    ~Impl()
    {
        if (s_instance == this) s_instance = nullptr;
        if (_janet_env)
        {
            for (auto& [kw, entry] : _keyword_handlers)
                janet_gcunroot(entry.handler);
            _keyword_handlers.clear();
            janet_gcunroot(_zelph_peg);
            janet_deinit();
        }
    }

    void init()
    {
        _main_thread_id = std::this_thread::get_id();
        janet_init();
        _janet_env = janet_core_env(nullptr);
        register_zelph_functions();
        setup_module_paths();
        setup_script_runner();
        setup_peg();
        setup_numbers();
    }

    void register_zelph_functions() const
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

    void setup_module_paths() const
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
    }

    void setup_script_runner() const
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
        if (status != JANET_SIGNAL_OK) janet_stacktrace(nullptr, out);
    }

    void setup_peg()
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
        if (status != JANET_SIGNAL_OK) janet_stacktrace(nullptr, out);

        janet_dostring(_janet_env, "(def zelph-peg (peg/compile zelph-grammar))", "init", &out);
        _zelph_peg = out;
        janet_gcroot(_zelph_peg);
    }

    void setup_numbers() const
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
    }

    static std::string format_janet(Janet j)
    {
        JanetString   desc = janet_description(j);
        JanetByteView view = {desc, janet_string_length(desc)};
        return std::string(reinterpret_cast<const char*>(view.bytes), view.len);
    }

    void log_janet_call(const std::string& func_name, int32_t argc, Janet* argv, bool is_entry, Janet ret = janet_wrap_nil()) const
    {
        if (!_log_janet_functions) return;

        std::ostringstream oss;
        oss << func_name << " inputs: ";
        for (int32_t i = 0; i < argc; ++i)
        {
            if (i > 0) oss << " ";
            oss << format_janet(argv[i]);
        }

        if (!is_entry)
            oss << " output: " << format_janet(ret);

        _n->diagnostic(oss.str());
    }

    // Converts Janet Types to Nodes.
    network::Node resolve_janet_arg(Janet arg)
    {
        if (janet_checktype(arg, JANET_ABSTRACT) || janet_checktype(arg, JANET_NUMBER))
        {
            return zelph_unwrap_node(arg);
        }
        else if (janet_checktype(arg, JANET_STRING))
        {
            // It's a standard named Node (Atom)
            const uint8_t* str  = janet_unwrap_string(arg);
            std::string    wstr = reinterpret_cast<const char*>(str);
            return _n->node(wstr, _n->lang());
        }
        else if (janet_checktype(arg, JANET_SYMBOL))
        {
            // It's a Variable
            const uint8_t* sym   = janet_unwrap_symbol(arg);
            std::string    s_sym = reinterpret_cast<const char*>(sym);

            std::lock_guard<std::mutex> lock(_state_mutex);
            if (_scoped_variables.count(s_sym)) return _scoped_variables[s_sym];

            network::Node v = _n->var();
            _n->set_name(v, s_sym, _n->lang(), false);
            _scoped_variables[s_sym] = v;
            return v;
        }
        return 0;
    }

    // Read-only variant: resolves strings to existing nodes without creating new ones.
    // Returns 0 if the node does not exist. Used by zelph/exists, zelph/sources, zelph/targets.
    network::Node resolve_janet_arg_no_create(Janet arg) const
    {
        if (janet_checktype(arg, JANET_ABSTRACT) || janet_checktype(arg, JANET_NUMBER))
        {
            return zelph_unwrap_node(arg);
        }
        else if (janet_checktype(arg, JANET_STRING))
        {
            const uint8_t* str  = janet_unwrap_string(arg);
            std::string    wstr = reinterpret_cast<const char*>(str);

            // Check regular named nodes
            network::Node n = _n->get_node(wstr, _n->lang());
            if (n) return n;

            // Check core nodes (e.g. "~", "=>", "in", "..")
            return _n->get_core_node(wstr);
        }
        return 0;
    }

    // Check whether a fact exists in the graph without creating it.
    // Returns true if the fact (subject predicate object...) is known.
    // Shared by zelph/exists and zelph/mentioned. The two ask different
    // questions about the same node: whether the statement was CLAIMED --
    // asserted or derived -- and whether the node is present at all, which a
    // rule's ground pattern is without anybody having claimed it.
    static Janet fact_probe(const char* name, const bool asserted_only, int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_boolean(0);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        network::Node s = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node p = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!s || !p)
        {
            Janet res = janet_wrap_boolean(0);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        for (int32_t i = 2; i < argc; ++i)
        {
            network::Node o = s_instance->resolve_janet_arg_no_create(argv[i]);
            if (!o)
            {
                Janet res = janet_wrap_boolean(0);
                if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, false, res);
                return res;
            }
            objs.insert(o);
        }

        const network::Answer ans   = s_instance->_n->check_fact(s, p, objs);
        network::Node         node  = ans.relation();
        bool                  known = ans.is_known();

        if (!known)
        {
            // A fact carrying FURTHER objects satisfies this one: `a p b`
            // holds when the graph says `a p b c`. That is what unification
            // matches, what a rule with exactly this condition fires on, and
            // what `¬` refuses to succeed against -- only the exact hash
            // could not see it. The SPARQL layer asks its ground triples
            // through here, so it answered "no" to a triple its own
            // variable patterns answer "yes" to.
            if (const network::Node wider = network::containing_fact(s_instance->_n, s, p, objs); wider != 0)
            {
                node  = wider;
                known = true;
            }
        }

        if (known && asserted_only) known = s_instance->_n->is_asserted_fact(node);

        Janet res = janet_wrap_boolean(known ? 1 : 0);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, false, res);
        return res;
    }

    static Janet janet_cfun_zelph_exists(int32_t argc, Janet* argv)
    {
        return fact_probe("zelph/exists", true, argc, argv);
    }

    static Janet janet_cfun_zelph_mentioned(int32_t argc, Janet* argv)
    {
        return fact_probe("zelph/mentioned", false, argc, argv);
    }

    // Return the name of a node as a string, or nil if unnamed.
    // Optional second argument specifies the language (defaults to current).
    static Janet janet_cfun_zelph_name(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/name", argc, argv, true);

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/name", argc, argv, false, res);
            return res;
        }

        std::string lang = s_instance->_n->lang();
        if (argc >= 2 && janet_checktype(argv[1], JANET_STRING))
        {
            lang = reinterpret_cast<const char*>(janet_unwrap_string(argv[1]));
        }

        std::string name = s_instance->_n->get_name(n, lang, true);
        if (name.empty())
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/name", argc, argv, false, res);
            return res;
        }

        Janet res = janet_cstringv(name.c_str());
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/name", argc, argv, false, res);
        return res;
    }

    // Find all subjects connected to target via predicate.
    // (zelph/sources "in" set-node) → elements of the set
    // (zelph/sources "~" concept)   → instances of that concept
    //
    // Implemented as a manual traversal (mirroring janet_cfun_zelph_targets)
    // instead of get_sources, because the required semantics are directional:
    // target must participate in the *object role*. A node X connected to
    // target through a fact "target predicate X" must not be reported.
    static Janet janet_cfun_zelph_sources(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_array(janet_array(0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/sources", argc, argv, true);

        network::Node predicate = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node target    = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!predicate || !target)
        {
            Janet res = janet_wrap_array(janet_array(0));
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/sources", argc, argv, false, res);
            return res;
        }

        network::adjacency_set sources = s_instance->_n->get_fact_subjects(predicate, target);

        JanetArray* result = janet_array(static_cast<int32_t>(sources.size()));
        for (network::Node src : sources)
        {
            janet_array_push(result, zelph_wrap_node(src));
        }
        Janet res = janet_wrap_array(result);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/sources", argc, argv, false, res);
        return res;
    }

    // Find all objects connected from subject via predicate.
    // (zelph/targets elem-node "cons") → cdr of cons cell (rest of list)
    // (zelph/targets inst-node "~")    → concept node
    // (zelph/targets node "in")        → container (set)
    static Janet janet_cfun_zelph_targets(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_array(janet_array(0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/targets", argc, argv, true);

        network::Node subject   = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node predicate = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!subject || !predicate)
        {
            Janet res = janet_wrap_array(janet_array(0));
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/targets", argc, argv, false, res);
            return res;
        }

        network::adjacency_set targets = s_instance->_n->get_fact_objects(subject, predicate);

        JanetArray* result = janet_array(static_cast<int32_t>(targets.size()));
        for (network::Node nd : targets)
        {
            janet_array_push(result, zelph_wrap_node(nd));
        }
        Janet res = janet_wrap_array(result);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/targets", argc, argv, false, res);
        return res;
    }

    // Shared implementation for the two closure bindings.
    static Janet closure_impl(int32_t argc, Janet* argv, const char* name, bool forward)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_array(janet_array(0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        network::Node anchor    = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node predicate = s_instance->resolve_janet_arg_no_create(argv[1]);
        bool          include   = argc >= 3 && janet_truthy(argv[2]);

        network::adjacency_set nodes;
        if (anchor && predicate)
        {
            nodes = forward
                      ? s_instance->_n->transitive_targets(anchor, predicate, include)
                      : s_instance->_n->transitive_sources(anchor, predicate, include);
        }

        JanetArray* result = janet_array(static_cast<int32_t>(nodes.size()));
        for (network::Node nd : nodes)
        {
            janet_array_push(result, zelph_wrap_node(nd));
        }
        Janet res = janet_wrap_array(result);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, false, res);
        return res;
    }

    static Janet janet_cfun_zelph_closure(int32_t argc, Janet* argv)
    {
        return closure_impl(argc, argv, "zelph/closure", true);
    }

    static Janet janet_cfun_zelph_closure_sources(int32_t argc, Janet* argv)
    {
        return closure_impl(argc, argv, "zelph/closure-sources", false);
    }

    // Read a Janet array/tuple of numbers into a vector<double>.
    static std::vector<double> janet_number_vector(Janet v, const char* what)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(v, &data, &len))
            janet_panicf("%s: expected an array or tuple of numbers", what);

        std::vector<double> out;
        out.reserve(static_cast<size_t>(len));
        for (int32_t i = 0; i < len; ++i)
        {
            if (!janet_checktype(data[i], JANET_NUMBER))
                janet_panicf("%s: element %d is not a number", what, i);
            out.push_back(janet_unwrap_number(data[i]));
        }
        return out;
    }

    // Create a raw weighted edge (synapse) from -> to, creating the nodes if
    // necessary. Raw edges carry no predicate and are invisible to reasoning.
    static Janet janet_cfun_zelph_nn_connect(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_nil();

        network::Node from = s_instance->resolve_janet_arg(argv[0]);
        network::Node to   = s_instance->resolve_janet_arg(argv[1]);
        if (!from || !to) janet_panicf("zelph/nn-connect: could not resolve nodes");

        const double w = argc >= 3 ? janet_getnumber(argv, 2) : 1.0;

        s_instance->_n->set_synapse(from, to, w);
        return janet_wrap_nil();
    }

    // Weight of the raw edge from -> to, or nil if no such edge exists.
    static Janet janet_cfun_zelph_weight(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        network::Node a = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node b = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!a || !b) return janet_wrap_nil();

        // Synapse entry (or explicitly stored fact probability): its value.
        // Real edge without stored entry: canonical weight 1.
        // Neither: nil.
        if (s_instance->_n->has_synapse(a, b))
            return janet_wrap_number(s_instance->_n->edge_weight(a, b, 1.0));
        if (s_instance->_n->has_right_edge(a, b))
            return janet_wrap_number(1.0);
        return janet_wrap_nil();
    }

    // Set the weight of an existing raw edge.
    static Janet janet_cfun_zelph_set_weight(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 3);
        if (!s_instance) return janet_wrap_nil();

        network::Node a = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node b = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!a || !b) janet_panicf("zelph/set-weight: could not resolve nodes");

        std::string err;
        try
        {
            s_instance->_n->set_edge_weight(a, b, janet_getnumber(argv, 2));
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/set-weight: %s (use zelph/nn-connect to create a synapse)", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Compile a feed-forward view of a sub-graph. Argument: indexed collection
    // of layer nodes, input first, output last. Returns an integer handle.
    static Janet janet_cfun_zelph_nn_compile(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();

        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(argv[0], &data, &len) || len < 2)
            janet_panicf("zelph/nn-compile: expected an array of at least 2 layer nodes");

        std::vector<network::Node> layers;
        layers.reserve(static_cast<size_t>(len));
        for (int32_t i = 0; i < len; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg_no_create(data[i]);
            if (!n) janet_panicf("zelph/nn-compile: layer at index %d could not be resolved", i);
            layers.push_back(n);
        }

        // The hidden-layer activation. Optional, and defaulting to the one
        // every net compiled before this argument existed was trained with -
        // a net evaluated with a different activation is a different net.
        network::Activation activation = network::Activation::Relu;
        if (argc >= 2 && !janet_checktype(argv[1], JANET_NIL))
        {
            const std::string name = janet_checktype(argv[1], JANET_KEYWORD)
                                       ? reinterpret_cast<const char*>(janet_unwrap_keyword(argv[1]))
                                       : reinterpret_cast<const char*>(janet_getstring(argv, 1));
            if (name == "leaky-relu")
                activation = network::Activation::LeakyRelu;
            else if (name != "relu")
                janet_panicf("zelph/nn-compile: unknown activation '%s' - use :relu or :leaky-relu", name.c_str());
        }

        std::string err;
        try
        {
            auto net = network::NeuralNet::compile(*s_instance->_n, layers, activation);

            std::lock_guard<std::mutex> lock(s_instance->_state_mutex);
            s_instance->_neural_nets.push_back(std::move(net));
            return janet_wrap_integer(static_cast<int32_t>(s_instance->_neural_nets.size() - 1));
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-compile: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Neurons of a compiled layer in index order (defines input/output order).
    static Janet janet_cfun_zelph_nn_nodes(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-nodes: invalid network handle");

        const int32_t layer = janet_getinteger(argv, 1);
        if (layer < 0 || static_cast<size_t>(layer) >= net->layer_count())
            janet_panicf("zelph/nn-nodes: layer index out of range");

        const auto& nodes  = net->layer_nodes(static_cast<size_t>(layer));
        JanetArray* result = janet_array(static_cast<int32_t>(nodes.size()));
        for (network::Node n : nodes)
        {
            janet_array_push(result, zelph_wrap_node(n));
        }
        return janet_wrap_array(result);
    }

    // Forward pass. inputs: numbers in zelph/nn-nodes order of layer 0.
    static Janet janet_cfun_zelph_nn_eval(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-eval: invalid network handle");

        std::vector<double> in = janet_number_vector(argv[1], "zelph/nn-eval");

        std::string err;
        try
        {
            const std::vector<double> out    = net->forward(in);
            JanetArray*               result = janet_array(static_cast<int32_t>(out.size()));
            for (const double v : out)
            {
                janet_array_push(result, janet_wrap_number(v));
            }
            return janet_wrap_array(result);
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-eval: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // One SGD step on a single sample; returns the loss before the update.
    static Janet janet_cfun_zelph_nn_train(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, 4);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-train: invalid network handle");

        std::vector<double> in  = janet_number_vector(argv[1], "zelph/nn-train");
        std::vector<double> tgt = janet_number_vector(argv[2], "zelph/nn-train");
        const double        lr  = argc >= 4 ? janet_getnumber(argv, 3) : 0.01;

        std::string err;
        try
        {
            return janet_wrap_number(net->train_step(in, tgt, lr));
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-train: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Copy the compiled net's weights out, as an array of arrays of numbers.
    static Janet janet_cfun_zelph_nn_snapshot(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-snapshot: invalid network handle");

        const auto& w    = net->weights();
        JanetArray* outer = janet_array(static_cast<int32_t>(w.size()));
        for (const auto& matrix : w)
        {
            JanetArray* inner = janet_array(static_cast<int32_t>(matrix.size()));
            for (const double v : matrix)
            {
                janet_array_push(inner, janet_wrap_number(v));
            }
            janet_array_push(outer, janet_wrap_array(inner));
        }
        return janet_wrap_array(outer);
    }

    // Put a snapshot back. Shapes must match the compiled net.
    static Janet janet_cfun_zelph_nn_restore(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-restore: invalid network handle");

        const Janet* outer;
        int32_t      outer_len;
        if (!janet_indexed_view(argv[1], &outer, &outer_len))
            janet_panicf("zelph/nn-restore: expected an array of weight matrices");

        std::vector<std::vector<double>> w;
        w.reserve(static_cast<size_t>(outer_len));
        for (int32_t k = 0; k < outer_len; ++k)
        {
            w.push_back(janet_number_vector(outer[k], "zelph/nn-restore"));
        }

        std::string err;
        try
        {
            net->set_weights(w);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-restore: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Write trained weights back into the graph's edge-weight store.
    static Janet janet_cfun_zelph_nn_write_back(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-write-back: invalid network handle");

        net->write_back(*s_instance->_n);
        return janet_wrap_nil();
    }

    // Parse an indexed collection whose elements are either a node-like value
    // (activation 1) or a [node activation] pair, into (Node, activation)
    // pairs. Node-like values are resolved without creating nodes. Graded
    // activations allow feeding quantitative graph data (e.g. edge weights of
    // another compiled net) as training samples.
    static std::vector<std::pair<network::Node, double>> janet_node_activations(Janet v, const char* what)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(v, &data, &len))
            janet_panicf("%s: expected an array or tuple of nodes or [node activation] pairs", what);

        std::vector<std::pair<network::Node, double>> out;
        out.reserve(static_cast<size_t>(len));

        for (int32_t i = 0; i < len; ++i)
        {
            Janet  element    = data[i];
            double activation = 1.0;

            const Janet* pair;
            int32_t      pair_len;
            if ((janet_checktype(element, JANET_TUPLE) || janet_checktype(element, JANET_ARRAY))
                && janet_indexed_view(element, &pair, &pair_len))
            {
                if (pair_len != 2 || !janet_checktype(pair[1], JANET_NUMBER))
                    janet_panicf("%s: element %d must be a node or a [node activation] pair", what, i);
                element    = pair[0];
                activation = janet_unwrap_number(pair[1]);
            }

            network::Node n = s_instance->resolve_janet_arg_no_create(element);
            if (!n) janet_panicf("%s: element %d could not be resolved to an existing node", what, i);
            out.emplace_back(n, activation);
        }
        return out;
    }

    // Fully connect two layers with raw synapses. Existing edges are left
    // untouched, so trained weights survive re-wiring and the call is
    // idempotent. Intended for dense hidden layers; data-driven sparse wiring
    // should use zelph/nn-connect per edge instead.
    static Janet janet_cfun_zelph_nn_connect_layers(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 4);
        if (!s_instance) return janet_wrap_nil();

        network::Node from_layer = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node to_layer   = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!from_layer || !to_layer) janet_panicf("zelph/nn-connect-layers: could not resolve layer nodes");

        const double   scale = argc >= 3 ? janet_getnumber(argv, 2) : 0.1;
        const uint64_t seed  = argc >= 4 ? static_cast<uint64_t>(janet_getnumber(argv, 3)) : 42u;

        std::string err;
        try
        {
            const int64_t created = network::connect_layers(*s_instance->_n, from_layer, to_layer, scale, seed);
            return janet_wrap_number(static_cast<double>(created));
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-connect-layers: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // One SGD step with node-addressed input/target.
    static Janet janet_cfun_zelph_nn_train_nodes(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, 4);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-train-nodes: invalid network handle");

        auto         in  = janet_node_activations(argv[1], "zelph/nn-train-nodes");
        auto         tgt = janet_node_activations(argv[2], "zelph/nn-train-nodes");
        const double lr  = argc >= 4 ? janet_getnumber(argv, 3) : 0.01;

        std::string err;
        try
        {
            return janet_wrap_number(net->train_nodes(in, tgt, lr));
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-train-nodes: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Forward pass with node-addressed input; returns scored output nodes.
    static Janet janet_cfun_zelph_nn_eval_nodes(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-eval-nodes: invalid network handle");

        auto          in    = janet_node_activations(argv[1], "zelph/nn-eval-nodes");
        const int32_t top_k = argc >= 3 ? janet_getinteger(argv, 2) : -1;

        std::string err;
        try
        {
            auto scored = net->eval_nodes(in);

            std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b)
                      { return a.second != b.second ? a.second > b.second : a.first < b.first; });

            const size_t n = top_k < 0 ? scored.size() : std::min(static_cast<size_t>(top_k), scored.size());

            JanetArray* result = janet_array(static_cast<int32_t>(n));
            for (size_t i = 0; i < n; ++i)
            {
                Janet pair[2] = {zelph_wrap_node(scored[i].first), janet_wrap_number(scored[i].second)};
                janet_array_push(result, janet_wrap_tuple(janet_tuple_n(pair, 2)));
            }
            return janet_wrap_array(result);
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-eval-nodes: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Tag a fact pattern as a neural condition and return the TAG FACT
    // (pattern nn <net>) -- not the pattern. The tag fact itself becomes
    // the rule condition, structurally analogous to a != guard, so the
    // pattern can additionally appear as an ordinary (binding) condition
    // in the same rule without the two collapsing into one node.
    static Janet janet_cfun_zelph_approx(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/approx", argc, argv, true);

        network::Node pattern = zelph_unwrap_node(argv[0]);
        if (!pattern) janet_panicf("zelph/approx: first argument must be a fact pattern node");

        const uint8_t* str     = janet_getstring(argv, 1);
        network::Node  net     = s_instance->_n->node(reinterpret_cast<const char*>(str), s_instance->_n->lang());
        network::Node  nn_pred = s_instance->_n->node("nn", "zelph");

        network::Node tag = s_instance->_n->fact(pattern, nn_pred, {net});

        Janet res = zelph_wrap_node(tag);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/approx", argc, argv, false, res);
        return res;
    }

    // Tag a one-step pattern as a transitive path condition. The desugared
    // form of (X P⁺ Y) and (X P∗ Y), and the exact counterpart of
    // zelph/approx: the graph holds an ordinary fact ABOUT the pattern, so
    // nothing new had to become a core node.
    static Janet janet_cfun_zelph_path(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/path", argc, argv, true);

        network::Node pattern = zelph_unwrap_node(argv[0]);
        if (!pattern) janet_panicf("zelph/path: first argument must be a fact pattern node");

        const std::string mode = reinterpret_cast<const char*>(janet_getstring(argv, 1));
        if (mode != "one-or-more" && mode != "zero-or-more")
            janet_panicf("zelph/path: mode must be \"one-or-more\" or \"zero-or-more\", got \"%s\"", mode.c_str());

        network::Node mode_node    = s_instance->_n->node(mode, "zelph");
        network::Node closure_pred = s_instance->_n->node("closure", "zelph");

        network::Node tag = s_instance->_n->fact(pattern, closure_pred, {mode_node});

        Janet res = zelph_wrap_node(tag);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/path", argc, argv, false, res);
        return res;
    }

    // A GROUND path outside a rule is the one shape a path marker has no
    // reading for. As a rule condition it is a reachability TEST, and with a
    // variable in it -- "S P279⁺ Q3" -- it is a question the engine answers.
    // Typed on its own line with both ends concrete it asserted the one step
    // underneath it: "a p⁺ b" put `a p b` into the graph as a claim and hung a
    // closure tag off it that nothing ever reads, because only a rule
    // condition is ever walked.
    //
    // Whether the ends are variables is not visible to the parser -- it is
    // decided when the tokens are RESOLVED -- so the refusal cannot live
    // beside the one for a path in a consequence slot. It cannot live in
    // zelph/path either: by then the operand FACT has been built, which is the
    // half that does the damage. So the sugar emits this guard ahead of the
    // construction, with the two ends bound exactly once.
    //
    // A Janet block is exempt: there the caller is using the API directly and
    // may well be assembling a condition for zelph/rule by hand.
    static Janet janet_cfun_zelph_path_guard(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        if (s_instance->_in_janet_block || s_instance->_building_rule) return janet_wrap_nil();

        // Decided on the ARGUMENT, not on a resolved node: resolve_janet_arg
        // reads a Janet symbol as a variable and a Janet string as a name, so
        // the two are already told apart here -- and asking this way creates
        // nothing, which matters for a statement that is about to be refused.
        // A node value (an evaluated subterm) is asked the general question.
        const auto is_open = [](Janet arg)
        {
            if (janet_checktype(arg, JANET_SYMBOL)) return true;
            if (janet_checktype(arg, JANET_STRING)) return false;
            const network::Node nd = zelph_unwrap_node(arg);
            return nd == 0 || s_instance->_n->var_in_closure(nd);
        };

        if (is_open(argv[0]) || is_open(argv[1])) return janet_wrap_nil();

        janet_panicf("\"⁺\" and \"∗\" are condition operators: reachability is what the engine "
                     "WALKS, not what you assert. Write a variable to ASK (\"S p⁺ b\"), or use "
                     "the path condition in a rule.");
    }

    // Register the digit alphabet for &-literal display (inverse of the
    // &-input syntax). Digits are given in ascending order of value; the
    // base is the array length. C++ makes no assumptions about the digit
    // names, their count, or their internal order -- the only hardcoded
    // convention is that &-literals are always decimal, on input and output.
    static Janet janet_cfun_zelph_set_number_digits(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set-number-digits", argc, argv, true);

        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(argv[0], &data, &len))
            janet_panicf("zelph/set-number-digits: expected an array or tuple of digit nodes/names");

        std::vector<network::Node> digits;
        digits.reserve(static_cast<size_t>(len));
        for (int32_t i = 0; i < len; ++i)
        {
            network::Node nd = s_instance->resolve_janet_arg(data[i]);
            if (!nd) janet_panicf("zelph/set-number-digits: digit at index %d could not be resolved", i);
            digits.push_back(nd);
        }

        std::string err;
        try
        {
            s_instance->_n->set_number_digits(digits);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/set-number-digits: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Register a display scheme: the delimiters a script uses to enclose its
    // own notation, its numeral prefix, and the identifier grammar of its
    // leaves. C++ stores these verbatim -- it never parses the scheme's
    // syntax and knows nothing about the domain.
    static Janet janet_cfun_zelph_register_display_scheme(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, 4);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/register-display-scheme", argc, argv, true);

        network::DisplayScheme scheme;
        scheme.name  = reinterpret_cast<const char*>(janet_getstring(argv, 0));
        scheme.open  = reinterpret_cast<const char*>(janet_getstring(argv, 1));
        scheme.close = reinterpret_cast<const char*>(janet_getstring(argv, 2));

        if (argc >= 4 && !janet_checktype(argv[3], JANET_NIL))
        {
            const auto option = [&](const char* key, std::string& out)
            {
                const Janet v = janet_get(argv[3], janet_ckeywordv(key));
                if (janet_checktype(v, JANET_STRING))
                    out = reinterpret_cast<const char*>(janet_unwrap_string(v));
                else if (janet_checktype(v, JANET_BUFFER))
                {
                    JanetBuffer* b = janet_unwrap_buffer(v);
                    out            = std::string(reinterpret_cast<const char*>(b->data), b->count);
                }
            };
            option("numeral-prefix", scheme.numeral_prefix);
            option("name-first", scheme.name_first);
            option("name-chars", scheme.name_chars);
        }

        std::string err;
        try
        {
            s_instance->_n->register_display_scheme(scheme);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/register-display-scheme: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Register infix operators into a previously declared scheme.
    static Janet janet_cfun_zelph_set_infix_display(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set-infix-display", argc, argv, true);

        const std::string name = reinterpret_cast<const char*>(janet_getstring(argv, 0));

        std::size_t scheme = 0;
        if (!s_instance->_n->find_display_scheme(name, scheme))
            janet_panicf("zelph/set-infix-display: unknown display scheme '%s' (register it first)", name.c_str());

        const Janet* rows;
        int32_t      row_count;
        if (!janet_indexed_view(argv[1], &rows, &row_count))
            janet_panicf("zelph/set-infix-display: expected an array of [predicate precedence associativity] entries");

        std::vector<network::InfixEntry> ops;
        ops.reserve(static_cast<size_t>(row_count));

        for (int32_t i = 0; i < row_count; ++i)
        {
            const Janet* cells;
            int32_t      cell_count;
            if (!janet_indexed_view(rows[i], &cells, &cell_count) || cell_count < 2)
                janet_panicf("zelph/set-infix-display: entry %d must be [predicate precedence &opt associativity]", i);

            network::InfixEntry entry;

            // Creating the node is intentional: a module registers its
            // operators up front, possibly before any fact mentions them.
            entry.predicate = s_instance->resolve_janet_arg(cells[0]);
            if (!entry.predicate)
                janet_panicf("zelph/set-infix-display: predicate of entry %d could not be resolved", i);

            if (!janet_checktype(cells[1], JANET_NUMBER))
                janet_panicf("zelph/set-infix-display: precedence of entry %d must be a number", i);
            entry.precedence = static_cast<int>(janet_unwrap_number(cells[1]));

            if (cell_count >= 3 && janet_checktype(cells[2], JANET_KEYWORD))
            {
                const std::string a = reinterpret_cast<const char*>(janet_unwrap_keyword(cells[2]));
                if (a == "left")
                    entry.assoc = -1;
                else if (a == "right")
                    entry.assoc = 1;
                else if (a == "none")
                    entry.assoc = 0;
                else
                    janet_panicf("zelph/set-infix-display: associativity of entry %d must be :left, :right or :none", i);
            }

            ops.push_back(entry);
        }

        std::string err;
        try
        {
            s_instance->_n->set_infix_display(scheme, ops);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/set-infix-display: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Register application-form predicates in a scheme: (S P O) is written
    // "S(O)". Shares the scheme's predicate namespace with the infix form.
    static Janet janet_cfun_zelph_set_application_display(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set-application-display", argc, argv, true);

        const std::string name = reinterpret_cast<const char*>(janet_getstring(argv, 0));

        std::size_t scheme = 0;
        if (!s_instance->_n->find_display_scheme(name, scheme))
            janet_panicf("zelph/set-application-display: unknown display scheme '%s' (register it first)", name.c_str());

        const Janet* rows;
        int32_t      row_count;
        if (!janet_indexed_view(argv[1], &rows, &row_count))
            janet_panicf("zelph/set-application-display: expected an array of predicates");

        std::vector<network::Node> preds;
        preds.reserve(static_cast<size_t>(row_count));
        for (int32_t i = 0; i < row_count; ++i)
        {
            // Creating the node is intentional, as in set-infix-display.
            network::Node p = s_instance->resolve_janet_arg(rows[i]);
            if (!p) janet_panicf("zelph/set-application-display: predicate %d could not be resolved", i);
            preds.push_back(p);
        }

        std::string err;
        try
        {
            s_instance->_n->set_application_display(scheme, preds);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/set-application-display: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Register predicates whose self-facts must render verbose. Additive
    // (unlike the replace-the-set semantics of zelph/set-number-digits):
    // arithmetic, symbolic-core and eml load incrementally, and a later
    // module must not clobber an earlier module's registrations.
    static Janet janet_cfun_zelph_no_selffact_sugar(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/no-selffact-sugar", argc, argv, true);

        std::vector<network::Node> preds;
        preds.reserve(static_cast<size_t>(argc));
        for (int32_t i = 0; i < argc; ++i)
        {
            // Creating the node is intentional: modules register their
            // operators up front, possibly before any fact mentions them.
            network::Node p = s_instance->resolve_janet_arg(argv[i]);
            if (!p) janet_panicf("zelph/no-selffact-sugar: argument %d could not be resolved", i);
            preds.push_back(p);
        }

        s_instance->_n->add_verbose_selffact_predicates(preds);
        return janet_wrap_nil();
    }

    // Emit a line through zelph's output handler. Janet's own print writes to
    // raw stdout and bypasses the OutputHandler (REPL redirection, playground,
    // test collectors); this is the pipeline-correct way for scripts to talk
    // to the user -- e.g. import-time notices, which the input-echo
    // suppression inside imports deliberately does not cover.
    static Janet janet_cfun_zelph_out(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        const uint8_t* str = janet_getstring(argv, 0);
        s_instance->_n->out(reinterpret_cast<const char*>(str), true);
        return janet_wrap_nil();
    }

    // Extract the car (first element / subject) of a cons cell.
    // Returns nil if the argument is nil or not a valid cons cell.
    static Janet janet_cfun_zelph_car(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, true);

        network::Node cell = zelph_unwrap_node(argv[0]);
        if (!cell || cell == s_instance->_n->core.Nil)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, false, res);
            return res;
        }

        // Verify this is a cons cell
        if (s_instance->_n->parse_relation(cell) != s_instance->_n->core.Cons)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        network::Node          subject = s_instance->_n->parse_fact(cell, objs, 0);
        if (!subject)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, false, res);
            return res;
        }

        Janet res = zelph_wrap_node(subject);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, false, res);
        return res;
    }

    // Extract the cdr (rest of list / object) of a cons cell.
    // Returns nil-node if the argument is nil or not a valid cons cell.
    static Janet janet_cfun_zelph_cdr(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, true);

        network::Node cell = zelph_unwrap_node(argv[0]);
        if (!cell || cell == s_instance->_n->core.Nil)
        {
            Janet res = zelph_wrap_node(s_instance->_n->core.Nil);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, false, res);
            return res;
        }

        // Verify this is a cons cell
        if (s_instance->_n->parse_relation(cell) != s_instance->_n->core.Cons)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        s_instance->_n->parse_fact(cell, objs, 0);
        if (objs.empty())
        {
            Janet res = zelph_wrap_node(s_instance->_n->core.Nil);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, false, res);
            return res;
        }

        Janet res = zelph_wrap_node(*objs.begin());
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, false, res);
        return res;
    }

    // Mark a fact pattern as negation and return the pattern node.
    // This is the Janet equivalent of (*(pattern) ~ negation) in zelph syntax.
    // The tagged node can then be used as a condition in zelph/rule.
    static Janet janet_cfun_zelph_negate(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, true);

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, false, res);
            return res;
        }

        s_instance->_n->fact(n, s_instance->_n->core.IsA, {s_instance->_n->core.Negation});

        Janet res = zelph_wrap_node(n); // Return the pattern node (like focus *)
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, false, res);
        return res;
    }

    // Create a complete inference rule: conjunction of conditions => consequence(s).
    // First argument: array or tuple of condition fact nodes.
    // Remaining arguments: one or more consequence fact nodes.
    // Returns the condition set node (the rule's identity in the graph).
    //
    // Equivalent zelph syntax:
    //   (*{cond1 cond2 ...} ~ conjunction) => consequence1
    //   (*{cond1 cond2 ...} ~ conjunction) => consequence2
    //
    // Janet usage:
    //   (zelph/rule [cond1 cond2] consequence1 consequence2)
    // The rule already in the graph that `rule` duplicates, or 0.
    //
    // Linear in the number of rules, but a candidate is dismissed by one
    // hash lookup and one integer compare, because each rule's fingerprint
    // is memoized as a 64-bit hash of its shape. A hash collision costs an
    // alpha-equivalence test that then says no -- it can never make the
    // answer wrong, since rules_alpha_equivalent is the decision.
    //
    // The memo needs no invalidation: a node IS its structure, so a rule's
    // shape is fixed for the lifetime of the process, and an entry for a
    // rule that was removed is simply never consulted again -- the scan
    // iterates the LIVE rule set. That set holds nothing but Causes
    // relations, and a non-rule would map to shape 0 and be skipped anyway.
    //
    // All of this runs while a script is being read, never during
    // reasoning.
    network::Node find_duplicate_rule(const network::Node rule)
    {
        const auto fingerprint = [this](const network::Node n) -> std::size_t
        {
            const auto it = _rule_shapes.find(n);
            if (it != _rule_shapes.end()) return it->second;

            const std::string shape = network::rule_shape(_n, n);
            const std::size_t h     = shape.empty() ? 0 : std::hash<std::string>{}(shape);
            _rule_shapes.emplace(n, h);
            return h;
        };

        const std::size_t shape = fingerprint(rule);
        if (shape == 0) return 0; // not a rule

        for (const network::Node candidate : _n->get_left(_n->core.Causes))
        {
            if (candidate == rule) continue;
            if (fingerprint(candidate) != shape) continue;
            if (network::rules_alpha_equivalent(_n, rule, candidate)) return candidate;
        }
        return 0;
    }

    // Build a rule statement, and keep it only if it says something new.
    //
    // The thunk performs the whole construction -- condition patterns, the
    // conjunction set, the => fact. Running it inside a scratch cluster
    // makes that construction undoable: a cluster records exactly the nodes
    // CREATED while it is active, and every part of an alpha-equivalent
    // rule that is not a variable is hash-consed, hence already present and
    // therefore never recorded. Dropping the scratch removes the second
    // copy and nothing else.
    static Janet janet_cfun_zelph_dedup_rule(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();

        JanetFunction* const thunk = janet_getfunction(argv, 0);

        static const std::string scratch  = "__rule";
        const std::string        previous = s_instance->_n->active_cluster_name();

        // A scratch cluster of our own must not swallow the user's: whatever
        // survives is handed back to the cluster that was active, so
        // .cluster-drop still rolls a rule back with the rest of an experiment.
        const auto restore = [&previous]
        {
            if (previous.empty())
                s_instance->_n->deactivate_cluster();
            else
                s_instance->_n->set_active_cluster(previous);
        };

        s_instance->_n->set_active_cluster(scratch);

        // Everything the thunk builds is rule STRUCTURE, not a claim -- see
        // the revocation in janet_cfun_zelph_fact, which must stay out of a
        // rule construction or a second rule mentioning the same ground
        // statement would turn the first one's pattern into data.
        s_instance->_building_rule = true;

        Janet             out    = janet_wrap_nil();
        const JanetSignal signal = pcall_rooted(thunk, 0, nullptr, &out);

        s_instance->_building_rule = false;

        restore();

        if (signal != JANET_SIGNAL_OK)
        {
            s_instance->_n->merge_cluster(scratch, previous); // keep whatever was built
            janet_signalv(static_cast<JanetSignal>(signal), out);
        }

        const network::Node rule = zelph_unwrap_node(out);
        const network::Node twin = rule ? s_instance->find_duplicate_rule(rule) : 0;

        if (twin == 0)
        {
            // What the scratch cluster recorded is exactly what this statement
            // brought into being -- which is how a GROUND pattern can be told
            // from the same statement asserted earlier. Read it before the
            // merge, which drops the bookkeeping.
            const std::vector<network::Node> created = s_instance->_n->cluster_nodes(scratch);
            s_instance->_n->merge_cluster(scratch, previous);
            if (rule) s_instance->_n->mark_rule_patterns(rule, created);
            return out;
        }

        // The scratch drop must not disarm the fact stores: re-entering an
        // existing rule is an ordinary thing to do, and it used to cost the
        // session its genuine-structure store. See drop_scratch_cluster.
        s_instance->_n->drop_scratch_cluster(scratch);
        return zelph_wrap_node(twin);
    }

    static Janet janet_cfun_zelph_rule(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, -1); // At least conditions + 1 consequence
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/rule", argc, argv, true);

        // First argument: indexed collection of condition fact nodes
        const Janet* cond_data;
        int32_t      cond_len;
        if (!janet_indexed_view(argv[0], &cond_data, &cond_len) || cond_len == 0)
        {
            janet_panicf("zelph/rule: first argument must be a non-empty array or tuple of conditions");
            return janet_wrap_nil(); // Unreachable
        }

        // Collect condition nodes
        std::unordered_set<network::Node> condition_nodes;
        for (int32_t i = 0; i < cond_len; ++i)
        {
            network::Node n = zelph_unwrap_node(cond_data[i]);
            if (!n)
                janet_panicf("zelph/rule: condition at index %d is not a valid zelph/node", i);

            // Same reason as in zelph/conjunction: a condition that is not a
            // pattern makes the rule inert, and a generator that builds one
            // has no other way to find out.
            if (!s_instance->is_condition_pattern(n))
                janet_panicf("zelph/rule: condition at index %d is \"%s\", which is not a fact pattern and can never match",
                             i,
                             s_instance->_n->format(n).c_str());

            condition_nodes.insert(n);
        }

        if (condition_nodes.empty())
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/rule", argc, argv, false, res);
            return res;
        }

        // Create condition set and mark as conjunction
        network::Node condition_set = s_instance->_n->collection(condition_nodes);
        s_instance->_n->fact(condition_set, s_instance->_n->core.IsA, {s_instance->_n->core.Conjunction});

        // Link each consequence via =>
        for (int32_t i = 1; i < argc; ++i)
        {
            network::Node consequence = zelph_unwrap_node(argv[i]);
            if (consequence)
                s_instance->_n->fact(condition_set, s_instance->_n->core.Causes, {consequence});
            else
                janet_panicf("zelph/rule: consequence at index %d is not a valid zelph/node", i - 1);
        }

        Janet res = zelph_wrap_node(condition_set);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/rule", argc, argv, false, res);
        return res;
    }

    // Build a cons list from string characters (for compact <abc> syntax).
    // Characters are reversed before list construction so that the last (rightmost)
    // character — the least significant digit in a numeric string — becomes the
    // outermost cons cell. This matches the node-list syntax where the user writes
    // digits in reverse order: <3 2 1> and <123> produce the same internal structure.
    static Janet janet_cfun_zelph_list_chars(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list-chars", argc, argv, true);

        const uint8_t* str   = janet_getstring(argv, 0);
        std::string    raw_s = reinterpret_cast<const char*>(str);

        if (raw_s.empty())
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list-chars", argc, argv, false, res);
            return res; // Empty lists are not supported
        }

        // Split into individual characters, then reverse so the rightmost character
        // (least significant digit) becomes element[0] and thus the outermost cons cell.
        // Example: "123" -> ['3','2','1'] -> list builds 3 cons (2 cons (1 cons nil))
        // This matches the node-list syntax where the user writes <3 2 1> for the number 123.
        std::vector<std::string> elements;
        string::for_each_codepoint(raw_s, [&](const std::string& cp)
                                   { elements.push_back(cp); });
        std::reverse(elements.begin(), elements.end());

        network::Node list_node = s_instance->_n->list(elements);
        Janet         res       = zelph_wrap_node(list_node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list-chars", argc, argv, false, res);
        return res;
    }

    // Build a cons list from existing nodes (for < A B > node-list syntax).
    // The first node in the input becomes the outermost cons cell (= head of the cons list).
    // For numbers, write digits in reverse so that the LSB comes first:
    // <3 2 1> gives 3 as the outermost car (= LSB of "123"), matching the internal
    // structure of the compact <123> syntax.
    static Janet janet_cfun_zelph_list(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list", argc, argv, true);

        std::vector<network::Node> elements;
        elements.reserve(argc);

        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.push_back(n);
        }

        // The empty cons list IS nil -- the same node zelph/set returns for
        // the empty set, and the terminator every non-empty list ends at.
        // Returning Janet's nil instead made `<>` evaluate to nothing at
        // all, so a statement containing it was silently dropped.
        if (elements.empty())
        {
            Janet res = zelph_wrap_node(s_instance->_n->core.Nil);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list", argc, argv, false, res);
            return res;
        }

        network::Node list_node = s_instance->_n->list(elements);
        Janet         res       = zelph_wrap_node(list_node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list", argc, argv, false, res);
        return res;
    }

    static Janet janet_cfun_zelph_set(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set", argc, argv, true);

        std::unordered_set<network::Node> elements;
        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.insert(n);
        }

        network::Node set_node = s_instance->_n->set(elements);
        Janet         res      = zelph_wrap_node(set_node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set", argc, argv, false, res);
        return res;
    }

    static Janet janet_cfun_zelph_collection(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/collection", argc, argv, true);

        std::unordered_set<network::Node> elements;
        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.insert(n);
        }

        network::Node node = s_instance->_n->collection(elements);
        Janet         res  = zelph_wrap_node(node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/collection", argc, argv, false, res);
        return res;
    }

    // Is `n` something a rule may hold as a condition?
    //
    // Unification matches a PATTERN, so a condition that carries no statement
    // matches nothing and the rule containing it can never fire. A nested
    // condition set is the one member that is not itself a fact: the
    // evaluator descends into it.
    bool is_condition_pattern(const network::Node n) const
    {
        if (n == 0) return false;
        if (_n->predicate_of(n) != 0) return true;
        return _n->check_fact(n, _n->core.IsA, {_n->core.Conjunction}).is_known();
    }

    // Every member of a set that has just been tagged `~ conjunction`.
    void check_conditions_are_patterns(const network::Node set) const
    {
        network::adjacency_set members;
        if (!_n->condition_set_members(set, members)) return;

        for (const network::Node m : members)
        {
            if (is_condition_pattern(m)) continue;

            const std::string offender = _n->format(m);
            janet_panicf("\"%s\" is a condition of this rule but not a statement, so the rule can never match", offender.c_str());
        }
    }

    // Build a rule's condition set from the `(cond, cond, ...)` comma list:
    // a collection tagged `~ conjunction`.
    //
    // A member that is not a fact pattern used to be taken as it came, and
    // the resulting rule was inert -- accepted, listed, and unable to fire.
    // The focus operator is the way to write one by accident, because it does
    // exactly what it promises: `(*A p C, C q b)` evaluates its first member
    // to the node A, so the rule's conditions are the node A and one fact.
    // A focus one level down stays legitimate -- `((*A p C) q b, ...)` is the
    // condition `A q b` with a second fact built on the side -- which is why
    // the test is what the member EVALUATES to rather than how it is written.
    static Janet janet_cfun_zelph_conjunction(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/conjunction", argc, argv, true);

        std::unordered_set<network::Node> conditions;
        for (int32_t i = 0; i < argc; ++i)
        {
            const network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (!n) janet_panicf("zelph/conjunction: condition %d is not a node", i + 1);

            if (!s_instance->is_condition_pattern(n))
                janet_panicf("condition %d of the comma list is \"%s\", which is not a statement and can never match. "
                             "A focus makes its statement evaluate to the focused node, so a condition written "
                             "\"*A p C\" is the node A -- write it \"A p C\" instead.",
                             i + 1,
                             s_instance->_n->format(n).c_str());

            conditions.insert(n);
        }

        const network::Node set = s_instance->_n->collection(conditions);
        s_instance->_n->fact(set, s_instance->_n->core.IsA, {s_instance->_n->core.Conjunction});

        Janet res = zelph_wrap_node(set);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/conjunction", argc, argv, false, res);
        return res;
    }

    static Janet janet_cfun_zelph_fact(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, true);

        network::Node s = s_instance->resolve_janet_arg(argv[0]);
        network::Node p = s_instance->resolve_janet_arg(argv[1]);
        if (!s || !p)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        for (int i = 2; i < argc; ++i)
        {
            network::Node o = s_instance->resolve_janet_arg(argv[i]);
            if (o) objs.insert(o);
        }
        if (objs.empty())
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
            return res;
        }

        // Resolving a printed pattern is not claiming it: look the fact up,
        // and answer with the node the graph HAS. Only when the graph has none
        // -- a pattern carrying variables, or a ground one nobody entered --
        // does the construction below run, and then there is nothing to
        // contradict; the scratch cluster the caller holds rolls it back.
        //
        // Asked as is_known, not for the id check_fact hands back either way.
        // That id is the hash of the triple and is meaningful for a fact the
        // graph does NOT hold -- it is what lets .node say "Unknown node"
        // rather than invent one -- but it comes with no edges under it, and a
        // pattern is more than an id to anything that has to MATCH with it:
        // answering with it gave the prune commands a bare number for
        // "(S p O)", which unification then printed as "??" and matched
        // nothing. A caller that wants the id of an absent pattern still gets
        // it, from the construction below, which produces the same hash inside
        // the scratch cluster the caller holds.
        //
        // Exactly the fact that was asked for, too. A WIDER one -- "a p b c"
        // when "a p b" was named -- is what unification matches, and the proof
        // search asks for it in those terms itself (resolve_pattern's
        // `containing` flag); a command that names a pattern must not silently
        // resolve to a fact carrying an object it was not told about, least of
        // all a destructive one.
        if (s_instance->_resolving_pattern)
        {
            const network::Answer known = s_instance->_n->check_fact(s, p, objs);

            if (const network::Node found = known.is_known() ? known.relation() : network::Node{0}; found != 0)
            {
                Janet res = zelph_wrap_node(found);
                if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
                return res;
            }
        }

        network::Node f = s_instance->_n->fact(s, p, objs);

        // The tag is what MAKES a container a rule's condition set, so this is
        // where the explicit spelling `(*{cond cond} ~ conjunction)` says what
        // the comma list says -- and it has to be asked here, because the tag
        // is the only thing that tells a set of conditions from a set of
        // anything else. Asked after the fact exists: condition_set_members
        // reads the tag, and several members mean nothing without it.
        if (f && p == s_instance->_n->core.IsA && objs.count(s_instance->_n->core.Conjunction) == 1)
            s_instance->check_conditions_are_patterns(s);

        // zelph/fact IS the assertion API, so calling it is a CLAIM and
        // revokes the pattern-only status the same statement may have
        // acquired by appearing in a rule -- exactly as a typed statement
        // does. Without this, a ground rule condition asserted from Janet
        // stayed invisible to unification and the rule never fired, while
        // zelph/exists still answered true off the rule's own pattern.
        // Only in a user's Janet block, and not while a rule is being
        // built; see _in_janet_block and _building_rule.
        if (f && s_instance->_in_janet_block && !s_instance->_building_rule)
            s_instance->_n->unmark_rule_pattern(f);

        Janet res = zelph_wrap_node(f);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
        return res;
    }

    // The claim that a fact does NOT hold, which is what `¬(F)` says when it
    // stands on its own line rather than in a rule condition.
    //
    // Same arguments and the same node identity as zelph/fact -- what differs
    // is the probability the fact is created with. zelph has always been able
    // to hold a fact as known-wrong (Answer::is_wrong, and the two refusals in
    // Zelph::fact that keep a graph from claiming both), and that mechanism is
    // what a top-level negation now reaches. It had no spelling before, which
    // is why `¬(a p b)` ASSERTED `a p b`: the operand was built by zelph/fact
    // before the tag was applied to it.
    //
    // Asserting the opposite of something the graph already claims is refused
    // by Zelph::fact rather than silently overwritten, in both directions.
    static Janet janet_cfun_zelph_refute(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, true);

        network::Node s = s_instance->resolve_janet_arg(argv[0]);
        network::Node p = s_instance->resolve_janet_arg(argv[1]);
        if (!s || !p)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        for (int i = 2; i < argc; ++i)
        {
            network::Node o = s_instance->resolve_janet_arg(argv[i]);
            if (o) objs.insert(o);
        }
        if (objs.empty())
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, false, res);
            return res;
        }

        network::Node f = s_instance->_n->fact(s, p, objs, 0.0L);

        // A refutation is a claim about the fact, so it revokes pattern-only
        // status exactly as an assertion does -- the reasoning in
        // janet_cfun_zelph_fact applies unchanged.
        if (f && s_instance->_in_janet_block && !s_instance->_building_rule)
            s_instance->_n->unmark_rule_pattern(f);

        // The probability says what the graph believes; this is what the read
        // surface consults, because a fact's probability sits on an edge and
        // asking for it per candidate would be a lock on the hot path.
        if (f) s_instance->_n->mark_refuted_fact(f);

        Janet res = zelph_wrap_node(f);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, false, res);
        return res;
    }

    // A variable node as a VALUE, so a caller can decide its extent.
    //
    // A variable symbol passed to zelph/fact is scoped to one evaluation of a
    // Janet block, exactly as a variable in zelph syntax is quantified by its
    // statement. That is the right default, but it left a join across blocks
    // inexpressible: 'B in two blocks means two variables, so a conjunction
    // assembled from conditions built separately does not join, it multiplies
    // -- silently, and catastrophically on a large graph.
    //
    // The node returned here is an ordinary zelph/node and travels like any
    // other, so the caller's own binding decides how far it reaches. It is
    // deliberately NOT entered into the scoped-variable map: the handle is
    // the identity, and a symbol of the same spelling in some later block
    // stays the separate variable it has always been.
    static Janet janet_cfun_zelph_var(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 0, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/var", argc, argv, true);

        const network::Node v = s_instance->_n->var();

        if (argc >= 1 && !janet_checktype(argv[0], JANET_NIL))
        {
            std::string name;
            if (janet_checktype(argv[0], JANET_SYMBOL))
                name = reinterpret_cast<const char*>(janet_unwrap_symbol(argv[0]));
            else
                name = reinterpret_cast<const char*>(janet_getstring(argv, 0));

            if (name.empty()) janet_panicf("zelph/var: the display name must not be empty");

            // Display only, and merge_on_conflict off: many variables may
            // carry one name, which is why a variable never takes the name
            // lookup over from an atom.
            s_instance->_n->set_name(v, name, s_instance->_n->lang(), false);
        }

        Janet res = zelph_wrap_node(v);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/var", argc, argv, false, res);
        return res;
    }

    // Resolve a name to a node, optionally in an explicit language.
    // (zelph/resolve "Q5" "wikidata") binds the node to the wikidata language
    // regardless of the current .lang setting.
    static Janet janet_cfun_zelph_resolve(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/resolve", argc, argv, true);

        const uint8_t* str  = janet_getstring(argv, 0);
        std::string    wstr = reinterpret_cast<const char*>(str);

        std::string lang = s_instance->_n->lang();
        if (argc >= 2 && janet_checktype(argv[1], JANET_STRING))
        {
            lang = reinterpret_cast<const char*>(janet_unwrap_string(argv[1]));
        }

        network::Node n   = s_instance->_n->node(wstr, lang);
        Janet         res = zelph_wrap_node(n);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/resolve", argc, argv, false, res);
        return res;
    }

    // Load and execute a script via the .import machinery. This is the way
    // to pull .zph files (facts, rules, arithmetic definitions) into the
    // network from Janet code.
    //
    // .janet files are rejected: run_janet_script drives janet_loop, and a
    // nested janet_loop (script importing a script) is not supported by
    // Janet - and Janet's own module system is the right tool for that job.
    //
    // Main thread only: the import pipeline executes Janet code in the main
    // VM (_janet_env), which must not be entered from other Janet threads.
    static Janet janet_cfun_zelph_import(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/import", argc, argv, true);

        if (std::this_thread::get_id() != s_instance->_main_thread_id)
            janet_panicf("zelph/import: must be called from the main thread, not from ev/spawn-thread (the import pipeline is bound to the main Janet VM)");

        if (!s_instance->_import_handler)
            janet_panicf("zelph/import: no import handler registered (script engine not fully initialized)");

        const std::string path = reinterpret_cast<const char*>(janet_getstring(argv, 0));

        if (std::filesystem::path(path).extension() == ".janet")
            janet_panicf("zelph/import: .janet files are not importable this way - use Janet's own (import ...), (use ...) or (dofile ...) instead");

        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc) - 1);
        for (int32_t i = 1; i < argc; ++i)
            args.emplace_back(reinterpret_cast<const char*>(janet_getstring(argv, i)));

        std::string err;
        try
        {
            s_instance->_import_handler(path, args);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/import: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Shared implementation for zelph/save and zelph/load. Both delegate to
    // the corresponding REPL command (".save"/".load") via the command
    // handler, so they share every check and side effect with the
    // interactive commands: extension validation, partial-load guard,
    // auto-run handling, format detection (.bin vs. Wikidata JSON), and
    // timing diagnostics.
    //
    // Main thread only: the commands manipulate REPL state (auto_run,
    // partial_load_mode), which is owned by the main thread and not
    // synchronized.
    static Janet command_impl(int32_t argc, Janet* argv, const char* name, const char* command)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        if (std::this_thread::get_id() != s_instance->_main_thread_id)
            janet_panicf("%s: must be called from the main thread, not from ev/spawn-thread", name);

        if (!s_instance->_command_handler)
            janet_panicf("%s: no command handler registered (script engine not fully initialized)", name);

        const std::string file = reinterpret_cast<const char*>(janet_getstring(argv, 0));

        std::string err;
        try
        {
            s_instance->_command_handler({command, file});
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("%s: %s", name, err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    static Janet janet_cfun_zelph_save(int32_t argc, Janet* argv)
    {
        return command_impl(argc, argv, "zelph/save", ".save");
    }

    static Janet janet_cfun_zelph_load(int32_t argc, Janet* argv)
    {
        return command_impl(argc, argv, "zelph/load", ".load");
    }

    // Same delegation as command_impl, for the REPL commands that take no
    // argument. Kept separate rather than making the argument optional,
    // because the arity check is what tells a caller which of the two it is.
    static Janet command_noarg_impl(int32_t argc, Janet* argv, const char* name, const char* command)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        if (std::this_thread::get_id() != s_instance->_main_thread_id)
            janet_panicf("%s: must be called from the main thread, not from ev/spawn-thread", name);

        if (!s_instance->_command_handler)
            janet_panicf("%s: no command handler registered (script engine not fully initialized)", name);

        std::string err;
        try
        {
            s_instance->_command_handler({command});
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("%s: %s", name, err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Run the inference engine. Without this, a program that drives zelph as
    // a library can assert facts and define rules from Janet but cannot ask
    // for their consequences: forward chaining is otherwise reached only
    // through the .run command or through auto-run at the end of a Janet
    // block, neither of which exists outside the REPL.
    static Janet janet_cfun_zelph_run(int32_t argc, Janet* argv)
    {
        return command_noarg_impl(argc, argv, "zelph/run", ".run");
    }

    static Janet janet_cfun_zelph_run_once(int32_t argc, Janet* argv)
    {
        return command_noarg_impl(argc, argv, "zelph/run-once", ".run-once");
    }

    static Janet janet_cfun_zelph_run_delta(int32_t argc, Janet* argv)
    {
        return command_noarg_impl(argc, argv, "zelph/run-delta", ".run-delta");
    }

    // --- Clusters: the graph as a workspace rather than a store ---
    //
    // A cluster records the IDs of nodes CREATED while it is active, so
    // dropping it removes exactly those again. That is the only form of
    // retraction a monotonic graph can offer, and without it a program that
    // asserts a fact base, reasons about it and reads the conclusions leaves
    // every question it ever asked in the graph for good. .explain already
    // uses a cluster internally for precisely this reason.
    //
    // These do NOT delegate to the REPL commands the way zelph/save and
    // zelph/run do. .cluster and .cluster-drop each print a status line, and
    // a caller that scopes one question per step of its own loop invokes
    // them thousands of times, where that line is not information but noise
    // on the caller's output channel. Returning the values instead is also
    // what a program wants: the active cluster's name, and how many nodes a
    // drop actually removed.
    //
    // Main thread only, like the commands they correspond to.
    static void cluster_preamble(int32_t argc, Janet* argv, const char* name)
    {
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        if (std::this_thread::get_id() != s_instance->_main_thread_id)
            janet_panicf("%s: must be called from the main thread, not from ev/spawn-thread", name);
    }

    static Janet janet_cfun_zelph_cluster(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 0, 1);
        if (!s_instance) return janet_wrap_nil();
        cluster_preamble(argc, argv, "zelph/cluster");

        if (argc == 1)
        {
            // nil and "default" both mean "no cluster", matching .cluster.
            if (janet_checktype(argv[0], JANET_NIL))
            {
                s_instance->_n->deactivate_cluster();
            }
            else
            {
                const std::string name = reinterpret_cast<const char*>(janet_getstring(argv, 0));
                if (name.empty() || name == "default")
                    s_instance->_n->deactivate_cluster();
                else
                    s_instance->_n->set_active_cluster(name);
            }
        }

        const std::string active = s_instance->_n->active_cluster_name();
        return active.empty() ? janet_wrap_nil() : janet_cstringv(active.c_str());
    }

    static Janet janet_cfun_zelph_cluster_drop(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        cluster_preamble(argc, argv, "zelph/cluster-drop");

        const std::string name = reinterpret_cast<const char*>(janet_getstring(argv, 0));
        if (name.empty() || name == "default")
            janet_panicf("zelph/cluster-drop: the default cluster cannot be dropped");

        return janet_wrap_integer(static_cast<int32_t>(s_instance->_n->drop_cluster(name)));
    }

    static Janet janet_cfun_zelph_clusters(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_nil();
        cluster_preamble(argc, argv, "zelph/clusters");

        const auto     listed = s_instance->_n->list_clusters();
        JanetArray*    out    = janet_array(static_cast<int32_t>(listed.size()));
        for (const auto& [name, count] : listed)
        {
            Janet pair[2] = {janet_cstringv(name.c_str()),
                             janet_wrap_integer(static_cast<int32_t>(count))};
            janet_array_push(out, janet_wrap_tuple(janet_tuple_n(pair, 2)));
        }
        return janet_wrap_array(out);
    }

    // Execute a query: print the pattern and trigger matching via apply_rule.
    // This is the Janet equivalent of entering a zelph statement that contains
    // variables (e.g. "X ~ human"). Takes a single zelph/node argument
    // (typically the return value of a zelph/fact call containing variables).
    static Janet janet_cfun_zelph_query(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/query", argc, argv, true);

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/query", argc, argv, false, res);
            return res;
        }

        // Build inverse mapping: variable Node -> symbol name
        // (must be done before apply_rule clears anything)
        std::map<network::Node, std::string> var_to_name;
        {
            std::lock_guard<std::mutex> lock(s_instance->_state_mutex);
            for (const auto& [name, node] : s_instance->_scoped_variables)
            {
                var_to_name[node] = name;
            }
        }

        // Collect results instead of printing them.
        //
        // This used to run only when the CURRENT statement had created scoped
        // variables, which tied a query to the expression that built its
        // pattern: storing a pattern in a Janet binding and querying it later
        // -- or simply querying the same pattern twice -- silently returned an
        // empty array, indistinguishable from "no matches". The scope is not
        // needed to run the query at all, only to label the bindings, and
        // resolve_janet_arg names every variable node it creates, so the names
        // can be recovered from the graph instead (see below).
        std::vector<std::shared_ptr<network::Variables>> results;

        s_instance->_n->set_query_collector(&results);
        s_instance->_n->apply_rule(0, n);
        s_instance->_n->set_query_collector(nullptr);

        // Reset variable scope for the next query/statement
        s_instance->clear_scoped_variables();

        // Convert results to Janet array of tables:
        // @[@{X <zelph/node ...> Y <zelph/node ...>} ...]
        JanetArray* result_array = janet_array(static_cast<int32_t>(results.size()));

        for (const auto& vars : results)
        {
            JanetTable* entry = janet_table(static_cast<int32_t>(vars->size()));

            for (const auto& [var_node, bound_node] : *vars)
            {
                // Prefer the name the current statement used; fall back to the
                // name the variable node carries in the graph, which is what
                // makes a pattern built in an earlier expression usable.
                auto        it = var_to_name.find(var_node);
                std::string key_name =
                    (it != var_to_name.end())
                        ? it->second
                        : s_instance->_n->get_name(var_node, s_instance->_n->lang(), true);

                if (key_name.empty()) continue;

                Janet key = janet_wrap_symbol(janet_symbol(
                    reinterpret_cast<const uint8_t*>(key_name.c_str()),
                    static_cast<int32_t>(key_name.size())));
                Janet val = zelph_wrap_node(bound_node);
                janet_table_put(entry, key, val);
            }

            janet_array_push(result_array, janet_wrap_table(entry));
        }

        Janet res = janet_wrap_array(result_array);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/query", argc, argv, false, res);
        return res;
    }

    static Janet janet_cfun_zelph_register_keyword(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_nil();

        const bool inline_mode = argc == 3;

        const uint8_t* str     = janet_getstring(argv, 0);
        std::string    keyword = reinterpret_cast<const char*>(str);

        if (keyword.empty() || keyword[0] == '.' || keyword[0] == '%' || keyword[0] == '#')
            janet_panicf("zelph/register-keyword: invalid keyword '%s'", keyword.c_str());
        if (keyword.find_first_of(" \t\r\n") != std::string::npos)
            janet_panicf("zelph/register-keyword: keyword must not contain whitespace");

        std::string close;
        if (inline_mode)
        {
            close = reinterpret_cast<const char*>(janet_getstring(argv, 1));
            if (close.empty())
                janet_panicf("zelph/register-keyword: closing delimiter must not be empty");
            if (close.find_first_of(" \t\r\n") != std::string::npos)
                janet_panicf("zelph/register-keyword: closing delimiter must not contain whitespace");
        }

        const Janet handler = argv[inline_mode ? 2 : 1];
        if (!janet_checktype(handler, JANET_FUNCTION))
            janet_panicf("zelph/register-keyword: handler must be a function");

        auto it = s_instance->_keyword_handlers.find(keyword);
        if (it != s_instance->_keyword_handlers.end())
            janet_gcunroot(it->second.handler);

        janet_gcroot(handler);
        s_instance->_keyword_handlers[keyword] = KeywordEntry{handler, inline_mode, close};
        return janet_wrap_nil();
    }

    // Helper to generate Janet code for a function call with potential focused arguments.
    // func_name: "zelph/fact" or "zelph/set"
    // args: Array of Janet tuples (the AST nodes)
    // A transitive path condition is written by SUFFIXING the predicate:
    // (X P⁺ Y) is one or more P steps, (X P∗ Y) zero or more. The marker is
    // recognised in PREDICATE POSITION ONLY, and only when something is left
    // of the token once it is removed -- which is what keeps it from being a
    // reserved character. A name may still contain ⁺ or ∗ anywhere (`Na⁺` is
    // a Wikidata label), and a predicate that IS the marker stays itself.
    //
    // ASCII `+` deliberately has no such reading: `z+` and `d+` are the
    // addition predicates of the integer and decimal arithmetic modules, and
    // `*` is the focus operator. ⁺ (U+207A) and ∗ (U+2217) are used nowhere
    // in the stdlib, are not in :reserved, and are what a mathematical reader
    // expects for R⁺ and R∗.
    static bool split_path_marker(const std::string& token, std::string& base, std::string& mode)
    {
        static const std::string one_plus  = "\xE2\x81\xBA"; // U+207A SUPERSCRIPT PLUS SIGN
        static const std::string zero_plus = "\xE2\x88\x97"; // U+2217 ASTERISK OPERATOR

        for (const auto& [marker, name] : {std::pair{one_plus, "one-or-more"}, std::pair{zero_plus, "zero-or-more"}})
        {
            if (token.size() > marker.size() && token.compare(token.size() - marker.size(), marker.size(), marker) == 0)
            {
                base = token.substr(0, token.size() - marker.size());
                mode = name;
                return true;
            }
        }
        return false;
    }

    // The predicate of a statement, if it is a plain atom: [:atom "text"].
    static bool atom_text(Janet arg, std::string& text)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(arg, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) != "atom") return false;
        if (!janet_checktype(data[1], JANET_STRING)) return false;
        text = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
        return !(text.size() >= 2 && text.front() == '"'); // a quoted atom is a literal name, never an operator
    }

    // Does this argument stand for a path condition? The marker has no AST
    // node of its own -- it is a suffix on the PREDICATE token -- so the
    // statement has to be decomposed far enough to look at it. Asked of the
    // SYNTAX for the same reason is_negation_ast is: the tag fact the sugar
    // builds is hash-consed and says nothing about which line wrote it.
    static bool is_path_ast(Janet node)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(node, &data, &len) || len < 2) return false;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return false;

        const std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
        if (type != "nested") return false;
        if (len == 2) return is_path_ast(data[1]);

        // [:nested subject predicate object ...] -- the same positions
        // build_smart_call reads.
        std::string token;
        std::string base;
        std::string mode;
        return atom_text(data[2], token) && split_path_marker(token, base, mode);
    }

    std::string build_smart_call(const std::string& func_name, const std::vector<Janet>& args) const
    {
        if (args.empty()) return "nil";

        // Path sugar, before anything else looks at the arguments: the
        // statement becomes the plain one-step fact, tagged.
        if (func_name == "zelph/fact" && args.size() >= 3)
        {
            std::string token;
            std::string base;
            std::string mode;
            if (atom_text(args[1], token) && split_path_marker(token, base, mode))
            {
                // Exactly one object is the only shape a path condition has --
                // evaluate_closure refuses the rest and says so. That is also
                // the shape whose two ends can be guarded, so the guard is
                // emitted for it and the others fall through to the refusal
                // the evaluator gives them.
                if (args.size() == 3)
                {
                    const std::string from = transform_arg(args[0]);
                    const std::string to   = transform_arg(args[2]);

                    // Bound once each: a variable token resolves to a FRESH
                    // variable node per evaluation, so naming the ends twice
                    // would give the guard and the pattern different variables.
                    const std::string head = "(let [$pfrom " + from + " $pto " + to + "]";
                    const std::string body = " (do (zelph/path-guard $pfrom $pto) (zelph/path (zelph/fact $pfrom \"";
                    return head + body + string::escape_atom(base) + "\" $pto) \"" + mode + "\")))";
                }

                std::string call = "(zelph/fact " + transform_arg(args[0]) + " \"" + string::escape_atom(base) + "\"";
                for (size_t i = 2; i < args.size(); ++i)
                    call += " " + transform_arg(args[i]);
                call += ")";
                return "(zelph/path " + call + " \"" + mode + "\")";
            }
        }

        int                      focused_index = -1;
        std::vector<std::string> arg_codes;
        arg_codes.reserve(args.size());

        for (size_t i = 0; i < args.size(); ++i)
        {
            const Janet* data;
            int32_t      len;
            if (!janet_indexed_view(args[i], &data, &len)) return "nil";

            std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));

            if (type == "focused")
            {
                if (focused_index != -1)
                {
                    // Error: Multiple foci (handled by returning nil or could throw)
                    // "Only one element... may have a star"
                    return "(error \"Zelph: Multiple focus markers (*) in one statement\")";
                }
                focused_index = (int)i;
                // Recursively transform the actual value inside the focus tag
                // [:focused val] -> data[1] is val
                arg_codes.push_back(transform_arg(data[1]));
            }
            else
            {
                arg_codes.push_back(transform_arg(args[i]));
            }
        }

        if (focused_index == -1)
        {
            // Simple case: No focus, just call the function
            std::string call = "(" + func_name;
            for (const auto& code : arg_codes)
                call += " " + code;
            call += ")";
            return call;
        }
        else
        {
            // Focused case: Use `let` to evaluate args, create side-effect, return focused arg.
            // (let [$0 arg0 $1 arg1 ... _ (func $0 $1 ...)] $focused_index)
            std::string let_block = "(let [";
            for (size_t i = 0; i < arg_codes.size(); ++i)
            {
                let_block += "$" + std::to_string(i) + " " + arg_codes[i] + " ";
            }
            let_block += "_ (" + func_name;
            for (size_t i = 0; i < arg_codes.size(); ++i)
            {
                let_block += " $" + std::to_string(i);
            }
            let_block += ")] $" + std::to_string(focused_index) + ")";
            return let_block;
        }
    }

    // Convert PEG-AST tuple to Janet Source Code String
    std::string transform_arg(Janet arg_tuple) const
    {
        if (!janet_checktype(arg_tuple, JANET_TUPLE) && !janet_checktype(arg_tuple, JANET_ARRAY)) return "nil";

        const Janet* data;
        int32_t      len;
        janet_indexed_view(arg_tuple, &data, &len);

        if (len == 1 && janet_checktype(data[0], JANET_KEYWORD))
        {
            // An EMPTY container still denotes something: both `<>` and `{}`
            // are nil -- the empty cons list is the terminator every list
            // ends at, and Zelph::set() has always answered nil for the
            // empty set. Falling through to "nil" (Janet's, not the node)
            // made the whole statement evaluate to nothing, silently.
            const std::string tag = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));
            if (tag == "set") return "(zelph/set)";
            if (tag == "collection") return "(zelph/collection)";
            if (tag == "list-nodes") return "(zelph/list)";
        }

        if (len < 2) return "nil"; // Minimum [:type value...]

        std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));

        if (type == "focused")
        {
            // If transform_arg is called directly on a focused node (e.g. root level single item),
            // just return the inner transformation. The focus has no effect if there's no surrounding operation.
            return transform_arg(data[1]);
        }
        else if (type == "nested")
        {
            // [:nested val1 val2 ...]
            // A single value in parentheses is plain grouping, not a fact.
            // Required for self-fact sugar in nested positions, e.g. a rule
            // consequence (:isprime N) or a subject ((:simplify T) = S).
            if (len == 2) return transform_arg(data[1]);

            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);
            return build_smart_call("zelph/fact", args);
        }
        else if (type == "set")
        {
            // [:set val1 val2 ...]
            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);
            return build_smart_call("zelph/set", args);
        }
        else if (type == "collection")
        {
            // [:collection val1 val2 ...]
            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);
            return build_smart_call("zelph/collection", args);
        }
        else if (type == "conjunction")
        {
            // [:conjunction [:condition v1 v2 ...] [:condition v3 v4 ...] ...]
            // Build a set of condition facts, mark as conjunction, return the set node.
            // This is the desugared form of: (*{cond1 cond2 ...} ~ conjunction)
            std::vector<std::string> cond_codes;
            for (int32_t i = 1; i < len; ++i)
            {
                const Janet* cond_data;
                int32_t      cond_len;
                if (!janet_indexed_view(data[i], &cond_data, &cond_len) || cond_len < 2) continue;

                int cond_val_count = cond_len - 1; // excluding :condition tag
                if (cond_val_count == 1)
                {
                    // Single value (e.g. a nested conjunction or negated pattern)
                    cond_codes.push_back(transform_arg(cond_data[1]));
                }
                else
                {
                    // Multiple values: treat as fact (S P O...), supports * focus
                    std::vector<Janet> args;
                    for (int32_t j = 1; j < cond_len; ++j)
                        args.push_back(cond_data[j]);
                    cond_codes.push_back(build_smart_call("zelph/fact", args));
                }
            }

            if (cond_codes.empty()) return "nil";
            if (cond_codes.size() == 1) return cond_codes[0]; // Safety: shouldn't happen with PEG

            // (zelph/conjunction code0 code1 ...) -- the members are evaluated
            // left to right, as they were when this built the collection and
            // its tag inline. What the call adds is the refusal of a member
            // that is not a fact pattern.
            std::string call = "(zelph/conjunction";
            for (const auto& code : cond_codes)
                call += " " + code;
            call += ")";
            return call;
        }
        else if (type == "negation")
        {
            // [:negation inner]
            // Desugars ¬X to (zelph/negate X)
            // which tags the pattern node with core.Negation and returns it.
            //
            // "¬¬(F)" tags the SAME pattern node twice -- the tag is a fact
            // ABOUT the node -- so the second operator was a no-op and the
            // statement silently meant "¬(F)". That is the opposite of what
            // it says: a double negation is the plain condition, and the
            // rule fired exactly when it should not have. Reading it that
            // way needs a second negation stratum, which is the parked
            // feature "¬(A, B)" is refused for; until then the operator says
            // so rather than dropping one of itself.
            if (is_negation_ast(data[1]))
            {
                throw std::runtime_error(
                    "\"¬\" does not nest: \"¬¬(F)\" would tag the same pattern twice "
                    "and mean \"¬(F)\", not \"F\". Write the plain pattern for the "
                    "positive condition.");
            }

            return "(zelph/negate " + transform_arg(data[1]) + ")";
        }
        else if (type == "approx")
        {
            // [:approx net-name inner] -- desugars ≈net(pattern) to
            // (zelph/approx pattern "net"): tags the pattern with the fact
            // (pattern nn net) and returns the pattern node, so it can serve
            // as a rule condition like any other pattern.
            if (len < 3) return "nil";
            std::string net;
            if (janet_checktype(data[1], JANET_STRING))
                net = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
            return "(zelph/approx " + transform_arg(data[2])
                 + " \"" + string::replace_all_copy(net, "\"", "\\\"") + "\")";
        }
        else if (type == "selffact")
        {
            // [:selffact pred-token value]
            // Desugars ":pred X" to the self-fact (X pred X). The operand is
            // evaluated exactly once and used as both subject and object, so
            // side effects (focus, fact creation) happen once and both sides
            // are guaranteed to be the same node. A variable token as
            // predicate (e.g. :R) keeps variable semantics, matching
            // ordinary fact parsing.
            if (len < 3) return "nil";

            std::string pred;
            if (janet_checktype(data[1], JANET_STRING))
                pred = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
            else if (janet_checktype(data[1], JANET_BUFFER))
            {
                JanetBuffer* b = janet_unwrap_buffer(data[1]);
                pred           = std::string(reinterpret_cast<const char*>(b->data), b->count);
            }
            if (pred.empty()) return "nil";

            const std::string pred_code = string::is_var(pred)
                                            ? "'" + pred
                                            : "\"" + string::replace_all_copy(pred, "\"", "\\\"") + "\"";

            return "(let [$sf " + transform_arg(data[2]) + "] (zelph/fact $sf " + pred_code + " $sf))";
        }
        else if (type == "list-nodes")
        {
            // [:list-nodes val1 val2 ...] — node list < A B C >
            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);
            return build_smart_call("zelph/list", args);
        }

        // Handle leaf nodes (atom, var, list-compact, unquote)
        // These expect data[1] to be the content string/buffer
        std::string val_str;
        if (janet_checktype(data[1], JANET_STRING))
        {
            val_str = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));
        }
        else if (janet_checktype(data[1], JANET_BUFFER))
        {
            val_str = reinterpret_cast<const char*>(janet_unwrap_buffer(data[1]));
        }

        if (type == "unquote")
        {
            // Janet variable reference: emit the variable name directly.
            // At runtime, resolve_janet_arg handles both string values
            // (resolved as node names) and zelph/node abstract values.
            return val_str;
        }
        else if (type == "var")
        {
            // Variables are symbols in Janet (e.g. X, _V).
            // IMPORTANT: We must quote them (e.g. 'A), otherwise Janet tries
            // to evaluate 'A' as a bound variable and fails if it's not defined.
            return "'" + val_str;
        }
        else if (type == "atom")
        {
            // Atoms are strings in Janet -- but the capture is zelph TEXT,
            // and handing it to Janet unchanged put zelph names through
            // Janet's escape set: a node written `a\b` came out named
            // `a<backspace>`. Decode zelph's own two escapes here, then
            // re-encode for Janet, which spells those same two the same way.
            std::string name = val_str;
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = string::unescape_atom(name.substr(1, name.size() - 2));

            return "\"" + string::escape_atom(name) + "\"";
        }
        else if (type == "list-compact")
        {
            // Convert <123> content to (zelph/list-chars "123").
            // janet_cfun_zelph_list_chars reverses the characters internally
            // so the LSB (rightmost char) becomes the outermost cons cell.
            // The content is not quoted in zelph, so it is a name already.
            std::string content = "\"" + string::escape_atom(val_str) + "\"";
            return "(zelph/list-chars " + content + ")";
        }
        else if (type == "number")
        {
            // &-literal: delegate the representation to the (redefinable)
            // Janet function zelph/number. Validation happens there too.
            std::string content = "\"" + string::replace_all_copy(val_str, "\"", "\\\"") + "\"";
            return "(zelph/number " + content + ")";
        }

        return "nil";
    }
};

// --- Static Helper Functions for Janet/zelph Bridge ---

int ScriptEngine::zelph_node_compare(void* p1, void* p2)
{
    network::Node n1 = *static_cast<network::Node*>(p1);
    network::Node n2 = *static_cast<network::Node*>(p2);
    return (n1 > n2) ? 1 : ((n1 < n2) ? -1 : 0);
}

int ScriptEngine::zelph_node_hash(void* p, size_t size)
{
    (void)size;
    network::Node n = *static_cast<network::Node*>(p);
    return static_cast<int32_t>(n ^ (n >> 32));
}

void ScriptEngine::zelph_node_tostring(void* p, JanetBuffer* buffer)
{
    network::Node n = *static_cast<network::Node*>(p);
    std::string   s = (Impl::s_instance && Impl::s_instance->_n) ? Impl::s_instance->_n->format(n) : ("<zelph/node " + std::to_string(n) + ">");
    janet_buffer_push_bytes(buffer, (const uint8_t*)s.c_str(), (int32_t)s.size());
}

const JanetAbstractType ScriptEngine::zelph_node_type = {
    "zelph/node",
    nullptr,             // gc
    nullptr,             // gcmark
    nullptr,             // get
    nullptr,             // put
    nullptr,             // marshal
    nullptr,             // unmarshal
    zelph_node_tostring, // tostring
    zelph_node_compare,  // compare
    zelph_node_hash,     // hash
    nullptr,             // next
    nullptr,             // call
    nullptr,             // length
    nullptr,             // bytes
};

Janet ScriptEngine::zelph_wrap_node(network::Node n)
{
    network::Node* ptr = (network::Node*)janet_abstract(&zelph_node_type, sizeof(network::Node));
    *ptr               = n;
    return janet_wrap_abstract(ptr);
}

network::Node ScriptEngine::zelph_unwrap_node(Janet val)
{
    if (janet_checktype(val, JANET_ABSTRACT))
    {
        void* abstract = janet_unwrap_abstract(val);
        if (janet_abstract_type(abstract) == &zelph_node_type)
        {
            return *static_cast<network::Node*>(abstract);
        }
    }
    if (janet_checktype(val, JANET_NUMBER))
    {
        return (network::Node)janet_unwrap_number(val);
    }
    return 0;
}

ScriptEngine::Impl* ScriptEngine::Impl::s_instance = nullptr;

ScriptEngine::ScriptEngine(network::Reasoning* reasoning)
    : _pImpl(new Impl(reasoning))
{
}

ScriptEngine::~ScriptEngine()
{
    delete _pImpl;
}

void ScriptEngine::initialize()
{
    _pImpl->init();
}

std::string ScriptEngine::get_janet_version()
{
    return JANET_VERSION;
}

void ScriptEngine::toggle_janet_logging()
{
    _pImpl->_log_janet_functions = !_pImpl->_log_janet_functions;
}

std::string ScriptEngine::get_janet_logging_status() const
{
    return _pImpl->_log_janet_functions ? "enabled" : "disabled";
}

std::string ScriptEngine::parse_zelph_to_janet(const std::string& input) const
{
    const std::string expanded = _pImpl->expand_inline_keywords(input);

    JanetSymbol      match_sym = janet_csymbol("zelph-safe-parse");
    Janet            match_fun_out;
    JanetBindingType bt = janet_resolve(_pImpl->_janet_env, match_sym, &match_fun_out);

    if (bt != JANET_BINDING_DEF) return "";

    JanetFunction* match_fun = janet_unwrap_function(match_fun_out);
    Janet          args[2]   = {_pImpl->_zelph_peg, janet_cstringv(expanded.c_str())};
    Janet          result;

    if (Impl::pcall_rooted(match_fun, 2, args, &result) != JANET_SIGNAL_OK)
    {
        return "";
    }
    if (janet_checktype(result, JANET_NIL))
    {
        return "";
    }

    JanetArray* tree = janet_unwrap_array(result);
    if (tree->count < 1) return "";

    const Janet* root_data;
    int32_t      root_len;
    if (!janet_indexed_view(tree->data[0], &root_data, &root_len)) return "";

    std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(root_data[0]));
    if (type == "root")
    {
        // Check how many items we have
        // root_data[0] is :root tag
        // root_data[1..n] are the values
        int val_count = root_len - 1;

        if (val_count == 0) return "";

        if (val_count == 1)
        {
            // A bare parenthesized fact like (A rel B) at the top level is a syntax error:
            // nested facts are only valid as arguments inside a larger statement, not standalone.
            const Janet* val_data;
            int32_t      val_len;
            if (janet_indexed_view(root_data[1], &val_data, &val_len) && val_len > 0
                && janet_checktype(val_data[0], JANET_KEYWORD))
            {
                std::string val_type = reinterpret_cast<const char*>(janet_unwrap_keyword(val_data[0]));
                if (val_type == "nested")
                    return ""; // Syntax error: (fact) at top level is not a valid statement

                // A statement of fewer than three values is incomplete, and an
                // empty result is how this function says so -- the REPL then
                // buffers the line and waits for the rest of it. That reading
                // is right for "a p" and wrong for "≈net(a p b)", which is not
                // the beginning of anything: `≈` asks what a network believes,
                // which a condition may read and a line cannot assert. The
                // symptom was silence, and the next line being swallowed with
                // it. Its two siblings are refused a few lines below and in
                // zelph/path-guard; this is the same refusal.
                if (val_type == "approx")
                    throw std::runtime_error(
                        "\"≈\" is a condition operator and has no meaning in a plain statement: "
                        "it asks what a network believes, which a rule condition can read and a "
                        "statement cannot claim. Use it in a rule condition.");

                // `¬(F)` on its own line is the one condition operator that
                // has a reading outside a condition, and it is not the one it
                // used to get. The sugar builds its operand with zelph/fact
                // and tags the result, so the line ASSERTED F and then marked
                // the very node it had just claimed as negated -- the graph
                // said the fact holds and that it does not. It now means what
                // it says: F is entered as known-wrong, through the
                // probability argument Zelph::fact has always had.
                if (val_type == "negation" && val_len >= 2)
                {
                    // The reading below is for a FACT, and the other two
                    // condition operators are not one. They are refused when
                    // they stand alone, and a `¬` in front used to get past
                    // every one of those refusals, because a negated line is
                    // routed through zelph/refute and the path sugar is keyed
                    // on zelph/fact. "¬(a p⁺ b)" therefore built a fact whose
                    // predicate is a node NAMED "p⁺" -- an invented predicate,
                    // marked refuted, from a line about reachability -- while
                    // "a p⁺ b" was refused as it should be. `≈` failed the
                    // other way: it reached the message below and was told it
                    // needs a fact, which it had.
                    if (Impl::is_path_ast(val_data[1]))
                        throw std::runtime_error(
                            "\"⁺\" and \"∗\" are condition operators, and a \"¬\" in front does not "
                            "change that: reachability is what the engine WALKS, so there is no "
                            "claim of it to deny. Use the path condition in a rule, under \"¬\" if "
                            "what you want is the absence of a path.");

                    if (is_approx_ast(val_data[1]))
                        throw std::runtime_error(
                            "\"≈\" is a condition operator, and a \"¬\" in front does not change "
                            "that: it asks what a network believes, which a rule condition can read "
                            "and a statement can neither claim nor deny. Use it in a rule "
                            "condition, under \"¬\" for the case the net does not confirm.");

                    const Janet* inner_data;
                    int32_t      inner_len;
                    if (janet_indexed_view(val_data[1], &inner_data, &inner_len)
                        && inner_len > 3
                        && janet_checktype(inner_data[0], JANET_KEYWORD)
                        && std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(inner_data[0]))) == "nested")
                    {
                        std::vector<Janet> inner_args;
                        for (int32_t i = 1; i < inner_len; ++i)
                            inner_args.push_back(inner_data[i]);
                        return _pImpl->build_smart_call("zelph/refute", inner_args);
                    }

                    throw std::runtime_error(
                        "\"¬\" on its own line says that a FACT does not hold, so it needs one -- "
                        "write \"¬(a p b)\". Inside a rule condition it is the "
                        "negation-as-failure operator and applies to a pattern.");
                }
            }
            return _pImpl->transform_arg(root_data[1]);
        }
        else
        {
            // Fact (S P O...)
            std::vector<Janet> fact_args;
            for (int i = 1; i < root_len; ++i)
            {
                fact_args.push_back(root_data[i]);
            }
            // `¬` is a CONDITION operator. As a consequence it used to be
            // ignored outright: "(A p B) => ¬(A q B)" derived (x q y) -- the
            // exact opposite of what the rule says, without a word. What a
            // derived negation should mean is a separate question (zelph can
            // hold a fact as known-wrong, via the probability argument of
            // fact()); until it is answered, saying so beats guessing.
            if (is_atom(fact_args[1], "=>"))
            {
                for (std::size_t i = 2; i < fact_args.size(); ++i)
                {
                    if (is_negation_ast(fact_args[i]))
                        throw std::runtime_error(
                            "\"¬\" is a condition operator and has no meaning as a consequence: "
                            "a rule derives what holds, not what does not. To say that the two "
                            "may not hold together, write a contradiction rule -- "
                            "\"(A p B, A q B) => !\".");

                    // The other two condition operators reached the consequence
                    // slot unchecked and each failed its own way: a path
                    // consequence was listed by .list-rules and derived nothing
                    // at all, and both would have written their tag fact -- and
                    // with it the one-step fact underneath it -- into the graph
                    // as a claim nobody made.
                    if (Impl::is_path_ast(fact_args[i]))
                        throw std::runtime_error(
                            "\"⁺\" and \"∗\" are condition operators and have no meaning as a "
                            "consequence: reachability is what the engine WALKS, not what a rule "
                            "asserts. Derive the one-step fact instead, and let the path condition "
                            "read it.");

                    if (is_approx_ast(fact_args[i]))
                        throw std::runtime_error(
                            "\"≈\" is a condition operator and has no meaning as a consequence: "
                            "a rule cannot assert what a network believes. Consult the net in a "
                            "condition and derive an ordinary fact from it.");
                }
            }
            else
            {
                // Outside a rule there is no condition slot, so `¬` has no
                // reading below the top level either -- and it was not
                // reported, it was DROPPED. The sugar builds its operand with
                // `zelph/fact` and tags the result, so a statement that denies
                // a fact came away claiming it.
                //
                // Same shape as the consequence slot, and as `¬(F)` on its own
                // line before df49661 gave that one a reading. The residue is
                // the same too: a negation tag on a node no rule negates,
                // which `.node` then reports as "Negated by a rule: yes".
                //
                // The whole statement is asked, predicate included: the guard
                // must sit ahead of `build_smart_call`, because by the time
                // `zelph/negate` receives its argument the fact underneath it
                // has been asserted.
                for (const Janet& arg : fact_args)
                    if (contains_negation_ast(arg))
                        throw std::runtime_error(
                            "\"¬\" is a condition operator and has no meaning inside a plain "
                            "statement: it succeeds when a pattern is ABSENT, which only a rule "
                            "condition can ask. On its own line \"¬(a p b)\" says that the fact "
                            "does not hold.");
            }

            // A path marker standing alone is refused too, but not here: only
            // the GROUND form asserts anything, and whether the ends are
            // variables is decided when they are resolved, not by the syntax.
            // "S P279⁺ Q3" is a legitimate question and answers one. See
            // janet_cfun_zelph_path.

            const std::string call = _pImpl->build_smart_call("zelph/fact", fact_args);

            // A rule statement is wrapped so that the WHOLE construction --
            // condition patterns, conjunction set, => fact -- happens where
            // it can be undone if the rule turns out to be one the graph
            // already has. See janet_cfun_zelph_dedup_rule. Recognising the
            // statement here, at the one place that knows it is a top-level
            // `S => O`, keeps the check off every other input line.
            if (is_atom(fact_args[1], "=>")) return "(zelph/dedup-rule (fn [] " + call + "))";

            return call;
        }
    }
    else if (type == "conjunction")
    {
        // Direktes Komma-Conjunction am Top-Level
        return _pImpl->transform_arg(tree->data[0]);
    }

    return "";
}

void ScriptEngine::process_janet(const std::string& code, bool is_zelph_ast)
{
    if (_pImpl->_scoped_vars_preloaded)
        _pImpl->_scoped_vars_preloaded = false; // scope prepared by inline-keyword expansion
    else
        _pImpl->_scoped_variables.clear();

    // Parser-generated code builds a statement's SUBTERMS with the same
    // zelph/fact calls as its top level, and a subterm is not claimed by the
    // statement that names it: "x documents ((a p b) => (c q d))" says
    // nothing about `a p b`. So the revocation for a parsed statement stays
    // with the node the statement EVALUATES to (below), while the one inside
    // zelph/fact belongs to a user's Janet block -- which this is when
    // is_zelph_ast is false.
    Impl::BlockScope block(_pImpl->_in_janet_block, !is_zelph_ast);

    Janet out;
    int   status = janet_dostring(_pImpl->_janet_env, code.c_str(), "zelph-script", &out);

    if (status != JANET_SIGNAL_OK)
    {
        // Throw a C++ exception so the error propagates correctly through import
        // chains and other nested call contexts (e.g. .import, process_file).
        std::string err = "Janet error";
        if (janet_checktype(out, JANET_STRING))
            err = reinterpret_cast<const char*>(janet_unwrap_string(out));
        else if (janet_checktype(out, JANET_BUFFER))
        {
            JanetBuffer* b = janet_unwrap_buffer(out);
            err            = std::string(reinterpret_cast<const char*>(b->data), b->count);
        }
        throw std::runtime_error(err);
    }
    else
    {
        if (is_zelph_ast)
        {
            network::Node n = zelph_unwrap_node(out);
            if (n)
            {
                // A statement typed at the top level is a CLAIM, and a claim
                // revokes the pattern-only status the same statement may have
                // acquired by appearing in a rule. Nothing else can revoke it:
                // the rule construction itself runs through the same fact()
                // calls, so only the top level knows that this one was meant.
                if (!_pImpl->has_scoped_variables()) _pImpl->_n->unmark_rule_pattern(n);

                if (_pImpl->echo_enabled())
                {
                    std::string output;
                    string::node_to_string(_pImpl->_n, output, _pImpl->_n->lang(), n, 3);
                    if (!output.empty() && output != "??") _pImpl->_n->out(string::unmark_identifiers(output), true);
                }

                if (_pImpl->has_scoped_variables())
                {
                    _pImpl->_n->apply_rule(0, n);
                }
            }
        }
        else
        {
            if (_pImpl->echo_enabled() && !janet_checktype(out, JANET_NIL))
            {
                _pImpl->_n->out(Impl::format_janet(out), true);
            }
        }
    }
}

void ScriptEngine::run_janet_script(const std::string& path, const std::vector<std::string>& args)
{
    _pImpl->clear_scoped_variables();

    Janet runner;
    if (janet_resolve(_pImpl->_janet_env, janet_csymbol("zelph/run-script"), &runner) != JANET_BINDING_DEF
        || !janet_checktype(runner, JANET_FUNCTION))
    {
        throw std::runtime_error("Internal error: zelph/run-script is not initialized");
    }
    JanetFunction* fn = janet_unwrap_function(runner);

    // Block the GC while assembling the call: neither the freshly created
    // strings nor the fiber are rooted yet, and any janet allocation could
    // otherwise trigger a collection.
    const int gc_handle = janet_gclock();

    std::vector<Janet> jargs;
    jargs.reserve(args.size() + 1);
    jargs.push_back(janet_cstringv(path.c_str()));
    for (const auto& a : args)
        jargs.push_back(janet_cstringv(a.c_str()));

    JanetFiber* fiber = janet_fiber(fn, 64, static_cast<int32_t>(jargs.size()), jargs.data());
    if (!fiber)
    {
        janet_gcunlock(gc_handle);
        throw std::runtime_error("Internal error: could not create fiber for zelph/run-script");
    }
    fiber->env = _pImpl->_janet_env;
    janet_gcroot(janet_wrap_fiber(fiber));
    janet_gcunlock(gc_handle);

    bool  failed = false;
    Janet out    = janet_wrap_nil();

#ifdef JANET_EV
    // Run the script as the root task of the Janet event loop - the same way
    // the janet CLI runs scripts (see janet's shell.c). This is what makes
    // ev/... usable: ev/spawn-thread, thread channels, timers. janet_loop
    // returns once the loop has drained, i.e. the root fiber has finished
    // AND all spawned threads/tasks are done.
    janet_schedule(fiber, janet_wrap_nil());
    janet_loop();

    if (janet_fiber_status(fiber) == JANET_STATUS_ERROR)
    {
        // The event loop has already printed the stacktrace to stderr;
        // propagate a concise error to the REPL/import chain. last_value is
        // the public JanetFiber field backing the fiber/last-value builtin:
        // after completion it holds the return value or the error payload.
        failed = true;
        out    = fiber->last_value;
    }
#else
    // No Janet event loop on this platform (e.g. Emscripten): run the script
    // synchronously; ev/... is not available here.
    JanetSignal sig = janet_continue(fiber, janet_wrap_nil(), &out);
    if (sig != JANET_SIGNAL_OK)
    {
        janet_stacktrace(fiber, out);
        failed = true;
    }
#endif

    // Extract the error text BEFORE unrooting the fiber: 'out' is only
    // reachable through the rooted fiber (last_value).
    std::string err;
    if (failed)
    {
        err = "Janet error";
        if (janet_checktype(out, JANET_STRING))
            err = reinterpret_cast<const char*>(janet_unwrap_string(out));
        else if (janet_checktype(out, JANET_BUFFER))
        {
            JanetBuffer* b = janet_unwrap_buffer(out);
            err            = std::string(reinterpret_cast<const char*>(b->data), b->count);
        }
        else
        {
            err = Impl::format_janet(out);
        }
    }

    janet_gcunroot(janet_wrap_fiber(fiber));

    if (failed)
        throw std::runtime_error("Script '" + path + "' failed: " + err);
}

// Helper function to evaluate a Janet expression and return a Node (used by prune commands)
network::Node ScriptEngine::evaluate_expression(const std::string& janet_code, const bool quiet, const bool resolving_pattern)
{
    Impl::BlockScope resolving(_pImpl->_resolving_pattern, resolving_pattern);

    if (_pImpl->_scoped_vars_preloaded)
        _pImpl->_scoped_vars_preloaded = false; // scope prepared by inline-keyword expansion
    else
        _pImpl->_scoped_variables.clear();

    // janet_dostring prints the stack trace itself, through janet_eprintf,
    // BEFORE it hands the error status back. Redirecting the :err dyn to a
    // buffer is the only way to keep a speculative evaluation silent; the
    // buffer is discarded, the error still travels via the exception below.
    struct ErrRedirect
    {
        explicit ErrRedirect(const bool on)
            : _on(on)
        {
            if (!_on) return;
            _saved = janet_dyn("err");
            janet_setdyn("err", janet_wrap_buffer(janet_buffer(256)));
        }
        ~ErrRedirect()
        {
            if (_on) janet_setdyn("err", _saved);
        }
        const bool _on;
        Janet      _saved{};
    } err_redirect(quiet);

    Janet out;
    int   status = janet_dostring(_pImpl->_janet_env, janet_code.c_str(), "eval_expr", &out);
    if (status != JANET_SIGNAL_OK)
    {
        std::string err = "Janet error";
        if (janet_checktype(out, JANET_STRING))
            err = reinterpret_cast<const char*>(janet_unwrap_string(out));
        else if (janet_checktype(out, JANET_BUFFER))
        {
            JanetBuffer* b = janet_unwrap_buffer(out);
            err            = std::string(reinterpret_cast<const char*>(b->data), b->count);
        }
        throw std::runtime_error(err);
    }
    return zelph_unwrap_node(out);
}

void ScriptEngine::set_script_args(const std::vector<std::string>& args)
{
    JanetArray* jargs = janet_array(static_cast<int32_t>(args.size()));
    for (const auto& arg : args)
    {
        janet_array_push(jargs, janet_cstringv(arg.c_str()));
    }
    janet_table_put(_pImpl->_janet_env, janet_ckeywordv("args"), janet_wrap_array(jargs));
}

void ScriptEngine::set_import_handler(ImportHandler handler)
{
    _pImpl->_import_handler = std::move(handler);
}

void ScriptEngine::set_command_handler(CommandHandler handler)
{
    _pImpl->_command_handler = std::move(handler);
}

void ScriptEngine::set_echo_predicate(EchoPredicate predicate)
{
    _pImpl->_echo_predicate = std::move(predicate);
}

bool ScriptEngine::has_keyword(const std::string& keyword) const
{
    // Block keywords only: inline keywords are detected inside statements
    // by expand_inline_keywords, never as a line-starting token.
    const auto it = _pImpl->_keyword_handlers.find(keyword);
    return it != _pImpl->_keyword_handlers.end() && !it->second.inline_mode;
}

bool ScriptEngine::invoke_keyword(const std::string& keyword, const std::string& text, const bool force)
{
    auto it = _pImpl->_keyword_handlers.find(keyword);
    if (it == _pImpl->_keyword_handlers.end() || it->second.inline_mode)
        throw std::runtime_error("No handler registered for keyword '" + keyword + "'");

    _pImpl->_scoped_variables.clear();

    Janet result;
    if (_pImpl->call_keyword_handler(keyword, it->second.handler, text, force, result) == Impl::HandlerCall::Incomplete)
        return false;

    // String results are emitted verbatim, line by line, through the output
    // handler (so they reach OutputCollector in tests and the REPL alike).
    // Other non-nil results are emitted via their Janet description.
    if (janet_checktype(result, JANET_STRING) || janet_checktype(result, JANET_BUFFER))
    {
        std::string text;
        if (janet_checktype(result, JANET_STRING))
            text = reinterpret_cast<const char*>(janet_unwrap_string(result));
        else
        {
            JanetBuffer* b = janet_unwrap_buffer(result);
            text           = std::string(reinterpret_cast<const char*>(b->data), b->count);
        }
        std::istringstream iss(text);
        for (std::string l; std::getline(iss, l);)
            _pImpl->_n->out(l, true);
    }
    else if (!janet_checktype(result, JANET_NIL))
    {
        _pImpl->_n->out(Impl::format_janet(result), true);
    }

    return true;
}

bool ScriptEngine::is_expression_complete(const std::string& code)
{
    int  depth      = 0;
    bool in_string  = false;
    bool escape     = false;
    bool in_comment = false;

    for (char c : code)
    {
        if (in_comment)
        {
            if (c == '\n') in_comment = false;
            continue;
        }

        if (escape)
        {
            escape = false;
            continue;
        }

        if (in_string)
        {
            if (c == '\\')
            {
                escape = true;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }

        if (c == '#')
        {
            in_comment = true;
            continue;
        }
        if (c == '"')
        {
            in_string = true;
            continue;
        }

        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
    }

    return depth <= 0;
}

// Determine whether a zelph statement is complete.
// A statement is complete when:
//   - All parentheses and braces are balanced
//   - The top-level token count indicates a full triple (>= 3),
//     OR it is exactly 1 token that is NOT a parenthesized group.
//     (A single paren group like "(*{...} ~ conj)" is a subject waiting for P and O.)
//     A bare atom, set {}, or list <> with count == 1 is a valid standalone expression.
//   - "Top-level token" = a contiguous non-whitespace chunk at paren/brace depth 0.
//     Note: < > (list delimiters) and all other chars are treated as plain characters
//     for token boundaries; only () and {} affect depth.
bool ScriptEngine::is_zelph_complete(const std::string& code)
{
    int  depth      = 0;
    bool in_string  = false;
    bool escape     = false;
    bool in_comment = false;

    int  top_tokens         = 0;
    bool in_top_token       = false;
    char second_token_first = '\0';

    for (char c : code)
    {
        if (in_comment)
        {
            if (c == '\n') in_comment = false;
            continue;
        }

        if (escape)
        {
            escape = false;
            continue;
        }

        if (in_string)
        {
            if (c == '\\')
            {
                escape = true;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }

        if (c == '#')
        {
            in_comment = true;
            continue;
        }
        if (c == '"')
        {
            in_string = true;
            if (depth == 0 && !in_top_token)
            {
                top_tokens++;
                in_top_token = true;
                if (top_tokens == 2 && second_token_first == '\0') second_token_first = c;
            }
            continue;
        }

        bool is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');

        if (c == '(' || c == '{')
        {
            if (depth == 0 && !in_top_token)
            {
                top_tokens++;
                in_top_token = true;
                if (top_tokens == 2 && second_token_first == '\0') second_token_first = c;
            }
            depth++;
        }
        else if (c == ')' || c == '}')
        {
            depth--;
            if (depth == 0) in_top_token = false;
        }
        else if (is_ws)
        {
            if (depth == 0) in_top_token = false;
        }
        else
        {
            if (depth == 0 && !in_top_token)
            {
                top_tokens++;
                in_top_token = true;
                if (top_tokens == 2 && second_token_first == '\0') second_token_first = c;
            }
        }
    }

    if (depth != 0 || in_string) return false;
    if (top_tokens == 0) return false;

    const size_t first_char_idx = code.find_first_not_of(" \t\r\n\v\f");
    const char   first_c        = first_char_idx == std::string::npos ? '\0' : code[first_char_idx];

    // Result-query prefix "? <statement>": '?' counts as the first
    // top-level token; the remainder must itself look complete -- a full
    // triple ("? S P O"), a self-fact with operand ("? :testprime &53"),
    // or a single bracketed value ("? (S P O)", "? $( ... )"). A lone
    // "? :pred" keeps accumulating like the native self-fact sugar.
    if (first_c == '?'
        && (first_char_idx + 1 >= code.size()
            || code[first_char_idx + 1] == ' ' || code[first_char_idx + 1] == '\t'
            || code[first_char_idx + 1] == '\n' || code[first_char_idx + 1] == '('
            || code[first_char_idx + 1] == '$'))
    {
        // Written WITHOUT the space -- "?(S P O)", "?$( ... ) ≡ $( ... )" --
        // the bracket opens inside the token the '?' started, so the whole
        // request counted as ONE token less than the spaced form and none of
        // the counts below could match: the statement looked unfinished
        // forever, although the character after '?' is one this very
        // condition accepts. Split it here and the two spellings are the same
        // question again. Balanced brackets are guaranteed (depth == 0 above).
        if (first_char_idx + 1 < code.size()
            && (code[first_char_idx + 1] == '(' || code[first_char_idx + 1] == '$'))
        {
            ++top_tokens;
            second_token_first = code[first_char_idx + 1];
        }

        if (top_tokens >= 4) return true;
        if (top_tokens == 3 && second_token_first == ':') return true;
        if (top_tokens == 2
            && (second_token_first == '(' || second_token_first == '$'
                || second_token_first == '{' || second_token_first == '<'
                || second_token_first == '\xC2'))
            return true;
        return false;
    }

    if (top_tokens <= 2)
    {
        size_t first_char_idx = code.find_first_not_of(" \t\r\n\v\f");
        if (first_char_idx != std::string::npos)
        {
            char c = code[first_char_idx];
            // '$' admits a lone inline-keyword island ($( ... )) as a
            // complete statement -- the calculator idiom. '\xC2' is the first
            // byte of "¬", which leads a complete statement of its own.
            if (top_tokens == 1 && (c == '{' || c == '<' || c == '*' || c == '\xC2' || c == '$'))
            {
                return true;
            }

            // "≈net(a p b)" is one top-level token too, and it is not the
            // beginning of anything: `≈` is a condition operator, and the
            // transform refuses it in a plain statement. Without this the line
            // was never handed to the transform at all -- it looked unfinished,
            // so it was buffered and SWALLOWED THE NEXT LINE, and a script
            // containing one silently ran a statement nobody wrote. Tested on
            // the whole three-byte sequence rather than the lead byte, which
            // "≡", "⁺" and "∗" share.
            if (top_tokens == 1 && code.compare(first_char_idx, 3, "≈") == 0)
            {
                return true;
            }
            // Self-fact sugar ":pred X" is a complete statement of exactly
            // two top-level tokens (the operand doubles as subject and
            // object). A lone ":pred" keeps accumulating, so the operand
            // may follow on the next line.
            if (top_tokens == 2 && c == ':')
            {
                return true;
            }
        }
        return false;
    }
    return true;
}
