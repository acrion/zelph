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
#include "reasoning.hpp"
#include "string_utils.hpp"

#include <janet.h>

#include <boost/algorithm/string.hpp>

#include <map>
#include <unordered_set>
#include <vector>

using namespace zelph;

// --- Static Helper Functions for Janet/Zelph Bridge ---

static int zelph_node_compare(void* p1, void* p2)
{
    network::Node n1 = *static_cast<network::Node*>(p1);
    network::Node n2 = *static_cast<network::Node*>(p2);
    return (n1 > n2) ? 1 : ((n1 < n2) ? -1 : 0);
}

static int zelph_node_hash(void* p, size_t size)
{
    (void)size;
    network::Node n = *static_cast<network::Node*>(p);
    return static_cast<int32_t>(n ^ (n >> 32));
}

static void zelph_node_tostring(void* p, JanetBuffer* buffer)
{
    network::Node n = *static_cast<network::Node*>(p);
    std::string   s = "<zelph/node " + std::to_string(n) + ">";
    janet_buffer_push_bytes(buffer, (const uint8_t*)s.c_str(), (int32_t)s.size());
}

static const JanetAbstractType zelph_node_type = {
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

static Janet zelph_wrap_node(network::Node n)
{
    network::Node* ptr = (network::Node*)janet_abstract(&zelph_node_type, sizeof(network::Node));
    *ptr               = n;
    return janet_wrap_abstract(ptr);
}

static network::Node zelph_unwrap_node(Janet val)
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

// --- Implementation Class ---

class ScriptEngine::Impl
{
public:
    static Impl* s_instance; // Required for static Janet C-function callbacks

    network::Reasoning* _n;
    JanetTable*         _janet_env = nullptr;
    Janet               _zelph_peg{};

    // Track variables used in the current scope/statement
    std::map<std::string, network::Node> _scoped_variables;

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
            janet_gcunroot(_zelph_peg);
            janet_deinit();
        }
    }

    void init()
    {
        janet_init();
        _janet_env = janet_core_env(nullptr);
        register_zelph_functions();
        setup_peg();
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

        janet_def(_janet_env, "zelph/seq", wrap((JanetCFunction)janet_cfun_zelph_seq), "(zelph/seq nodes...)\nCreate sequence from nodes.");

        janet_def(_janet_env, "zelph/seq-chars", wrap((JanetCFunction)janet_cfun_zelph_seq_chars), "(zelph/seq-chars str)\nCreate sequence from string characters.");
        janet_def(_janet_env, "zelph/set", wrap((JanetCFunction)janet_cfun_zelph_set), "(zelph/set nodes...)\nCreate set node from elements.");

        janet_def(_janet_env, "zelph/resolve", wrap((JanetCFunction)janet_cfun_zelph_resolve), "(zelph/resolve name)\nResolve a string to its node in the current language, creating it if needed.");

        janet_def(_janet_env, "zelph/query", wrap((JanetCFunction)janet_cfun_zelph_query), "(zelph/query node)\nExecute a query and return results as an array of tables.\nEach table maps variable symbols to their bound zelph/node values.\nTakes a zelph/fact containing variables.");

        janet_def(_janet_env, "zelph/exists", wrap((JanetCFunction)janet_cfun_zelph_exists), "(zelph/exists s p o)\nCheck whether a fact exists without creating it. Returns boolean.");

        janet_def(_janet_env, "zelph/name", wrap((JanetCFunction)janet_cfun_zelph_name), "(zelph/name node &opt lang)\nReturn the name of a node as a string, or nil if unnamed.");

        janet_def(_janet_env, "zelph/sources", wrap((JanetCFunction)janet_cfun_zelph_sources), "(zelph/sources predicate target)\nFind all subjects connected to target via predicate. Read-only.");

        janet_def(_janet_env, "zelph/targets", wrap((JanetCFunction)janet_cfun_zelph_targets), "(zelph/targets subject predicate)\nFind all objects connected from subject via predicate. Read-only.");
    }

    void setup_peg()
    {
        // zelph Grammar:
        // 1. :atom -> Alphanumeric or Symbols (excluding reserved)
        // 2. :sequence -> < ... >
        // 3. :set -> { ... }
        // 4. :nested -> ( ... ) Recursive statements inside ( ... )
        // 5. :quoted -> "..."
        // 6. :focused -> *Element (Returns the element instead of the container)
        // 7. :unquote -> ,identifier (Reference to a Janet variable)
        // Returns tagged tuples like [:atom "val"], [:seq "val"] or [:nested sub-stmt...] for C++ processing
        std::string peg_setup = R"zph(
            (def zelph-grammar
              ~{:ws (set " \t\r\f\n\0\v")
                :s* (any :ws)
                :s+ (some :ws)

                # > and < are reserved to act as delimiters.
                # , is reserved for unquoting Janet variables.
                # To use them as atoms, we define specific rules below.
                :reserved (set " \t\r\n\0\v<\"(){}*>,")

                # Identifiers
                :symchars (if-not :reserved 1)
                :var-underscore (* "_" (any :symchars))
                :var-uppercase  (* (range "AZ") (not :symchars))

                # A variable must start with underscore or be a single uppercase letter
                :var-token (choice :var-underscore :var-uppercase)

                # Atoms
                :quoted (capture (* "\"" (any (if-not "\"" 1)) "\""))

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

                # Atom Definition Order:
                # 1. Quoted (always safe)
                # 2. Multi-char arrows (e.g. "=>"). Must be before raw-atom because "=" is a symchar.
                # 3. Raw atoms (e.g. "abc", "=")
                # 4. Single ops (e.g. ">"). Checked last to prefer longer matches or delimiters.
                :tag-atom   (group (* (constant :atom) (choice :quoted :arrow-multi :raw-atom :op-single)))

                :star-atom  (group (* (constant :atom) (capture "*")))

                # 1. Compact Sequence: <abc>
                :tag-seq-compact (group (* (constant :seq-compact) (* "<" (capture (some (if-not (set "> \t\r\n") 1))) ">")))

                # Recursive definitions need forward declaration in PEG if simple recursive descent isn't enough,
                # but Janet PEG handles this via the :val-any choice reference.

                # Focused Value: *Value (e.g. *A or *{...} or *(...))
                # Returns [:focused value-node]
                :tag-focused (group (* (constant :focused) "*" :val-any))

                # Nested Facts: ( A B C )
                :tag-nested (group (* (constant :nested) "(" :s* :stmt-any :s* ")"))

                # Sets: { A B C }
                :set-content (any (sequence :s* :val-any))
                :tag-set    (group (* (constant :set) "{" :set-content :s* "}"))

                # 2. Node Sequence: < a b >
                # The loop (if-not ">" :val-any) ensures we don't consume the closing delimiter.
                # This works for "=>" because the first char "=" passes the check,
                # and then :arrow-multi consumes the whole "=>".
                # An unquoted ">" atom inside a sequence is impossible here (it acts as delimiter).
                :seq-content (any (sequence :s* (if-not ">" :val-any)))
                :tag-seq-nodes (group (* (constant :seq-nodes) (* "<" :seq-content :s* ">")))

                # Value order:
                # Check sequences first so "<" starts a sequence if possible.
                :val-any (choice :tag-focused :tag-var :tag-unquote :tag-seq-compact :tag-seq-nodes :tag-atom :star-atom :tag-nested :tag-set)

                # A statement is a sequence of values separated by whitespace
                # Used inside ( ... ) and at top level for facts
                :stmt-any (sequence :val-any (any (sequence :s+ :val-any)))

                # Top Level Parsing
                # Everything is captured into a :root group.
                # C++ logic decides if it's a single value or a fact (S P O) based on element count.
                :main (sequence :s* (group (* (constant :root) :stmt-any)) :s*)})

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
            std::wstring   wstr = string::unicode::from_utf8(reinterpret_cast<const char*>(str));
            return _n->node(wstr, _n->lang());
        }
        else if (janet_checktype(arg, JANET_SYMBOL))
        {
            // It's a Variable
            const uint8_t* sym   = janet_unwrap_symbol(arg);
            std::string    s_sym = reinterpret_cast<const char*>(sym);
            if (_scoped_variables.count(s_sym)) return _scoped_variables[s_sym];

            network::Node v = _n->var();
            _n->set_name(v, string::unicode::from_utf8(s_sym), _n->lang(), false);
            _scoped_variables[s_sym] = v;
            return v;
        }
        return 0;
    }

    // Read-only variant: resolves strings to existing nodes without creating new ones.
    // Returns 0 if the node does not exist. Used by zelph/exists, zelph/sources, zelph/targets.
    network::Node resolve_janet_arg_no_create(Janet arg)
    {
        if (janet_checktype(arg, JANET_ABSTRACT) || janet_checktype(arg, JANET_NUMBER))
        {
            return zelph_unwrap_node(arg);
        }
        else if (janet_checktype(arg, JANET_STRING))
        {
            const uint8_t* str  = janet_unwrap_string(arg);
            std::wstring   wstr = string::unicode::from_utf8(reinterpret_cast<const char*>(str));

            // Check regular named nodes
            network::Node n = _n->get_node(wstr, _n->lang());
            if (n) return n;

            // Check core nodes (e.g. "~", "=>", "in", "..")
            return _n->get_core_node(wstr);
        }
        return 0;
    }

    // --- Add these static functions after janet_cfun_zelph_query ---

    // Check whether a fact exists in the graph without creating it.
    // Returns true if the fact (subject predicate object...) is known.
    static Janet janet_cfun_zelph_exists(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_boolean(0);

        network::Node s = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node p = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!s || !p) return janet_wrap_boolean(0);

        network::adjacency_set objs;
        for (int32_t i = 2; i < argc; ++i)
        {
            network::Node o = s_instance->resolve_janet_arg_no_create(argv[i]);
            if (!o) return janet_wrap_boolean(0);
            objs.insert(o);
        }

        network::Answer ans = s_instance->_n->check_fact(s, p, objs);
        return janet_wrap_boolean(ans.is_known() ? 1 : 0);
    }

    // Return the name of a node as a string, or nil if unnamed.
    // Optional second argument specifies the language (defaults to current).
    static Janet janet_cfun_zelph_name(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n) return janet_wrap_nil();

        std::string lang = s_instance->_n->lang();
        if (argc >= 2 && janet_checktype(argv[1], JANET_STRING))
        {
            lang = reinterpret_cast<const char*>(janet_unwrap_string(argv[1]));
        }

        std::wstring name = s_instance->_n->get_name(n, lang, true);
        if (name.empty()) return janet_wrap_nil();

        std::string utf8 = string::unicode::to_utf8(name);
        return janet_cstringv(utf8.c_str());
    }

    // Find all subjects connected to target via predicate.
    // (zelph/sources "in" seq-node) → elements of the sequence/set
    // (zelph/sources "~" concept)   → instances of that concept
    static Janet janet_cfun_zelph_sources(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_array(janet_array(0));

        network::Node predicate = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node target    = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!predicate || !target) return janet_wrap_array(janet_array(0));

        network::adjacency_set sources = s_instance->_n->get_sources(predicate, target);

        JanetArray* result = janet_array(static_cast<int32_t>(sources.size()));
        for (network::Node src : sources)
        {
            janet_array_push(result, zelph_wrap_node(src));
        }
        return janet_wrap_array(result);
    }

    // Find all objects connected from subject via predicate.
    // (zelph/targets elem-node "..") → successor in sequence
    // (zelph/targets inst-node "~")  → concept node
    // (zelph/targets node "in")      → container (set/sequence)
    static Janet janet_cfun_zelph_targets(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_array(janet_array(0));

        network::Node subject   = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node predicate = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!subject || !predicate) return janet_wrap_array(janet_array(0));

        network::adjacency_set targets;

        // Traverse: subject → relation_nodes → find objects
        // Topology: subject <-> relation_node (bidirectional), object -> relation_node, relation_node -> predicate
        for (network::Node rel : s_instance->_n->get_right(subject))
        {
            network::adjacency_set rel_right = s_instance->_n->get_right(rel);
            network::adjacency_set rel_left  = s_instance->_n->get_left(rel);

            // Validate: predicate in right, subject bidirectional (in both left and right)
            if (rel_right.count(predicate) && rel_right.count(subject) && rel_left.count(subject))
            {
                // Objects: in left but NOT in right (unidirectional connection)
                for (network::Node obj : rel_left)
                {
                    if (obj != subject && rel_right.count(obj) == 0)
                    {
                        targets.insert(obj);
                    }
                }
            }
        }

        JanetArray* result = janet_array(static_cast<int32_t>(targets.size()));
        for (network::Node nd : targets)
        {
            janet_array_push(result, zelph_wrap_node(nd));
        }
        return janet_wrap_array(result);
    }

    // Split string into chars (for compact <abc> syntax)
    static Janet janet_cfun_zelph_seq_chars(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();

        const uint8_t* str   = janet_getstring(argv, 0);
        std::string    raw_s = reinterpret_cast<const char*>(str);
        std::wstring   wstr  = string::unicode::from_utf8(raw_s);

        // Compact mode always splits by character
        std::vector<std::wstring> elements;
        elements.reserve(wstr.length());
        for (wchar_t c : wstr)
        {
            elements.emplace_back(1, c);
        }

        if (elements.empty()) return janet_wrap_nil(); // Empty sequences are not supported

        network::Node seq_node = s_instance->_n->sequence(elements);
        return zelph_wrap_node(seq_node);
    }

    // Sequence of Nodes (for < A B > syntax)
    static Janet janet_cfun_zelph_seq(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();

        std::vector<network::Node> elements;
        elements.reserve(argc);

        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.push_back(n);
        }

        if (elements.empty()) return janet_wrap_nil();

        network::Node seq_node = s_instance->_n->sequence(elements);
        return zelph_wrap_node(seq_node);
    }

    static Janet janet_cfun_zelph_set(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();

        std::unordered_set<network::Node> elements;
        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.insert(n);
        }

        network::Node set_node = s_instance->_n->set(elements);
        return zelph_wrap_node(set_node);
    }

    static Janet janet_cfun_zelph_fact(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_nil();

        network::Node s = s_instance->resolve_janet_arg(argv[0]);
        network::Node p = s_instance->resolve_janet_arg(argv[1]);
        if (!s || !p) return janet_wrap_nil();

        network::adjacency_set objs;
        for (int i = 2; i < argc; ++i)
        {
            network::Node o = s_instance->resolve_janet_arg(argv[i]);
            if (o) objs.insert(o);
        }
        if (objs.empty()) return janet_wrap_nil();

        network::Node f = s_instance->_n->fact(s, p, objs);
        return zelph_wrap_node(f);
    }

    // Resolve a name to a node in the current language (convenience for Janet code)
    static Janet janet_cfun_zelph_resolve(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();

        const uint8_t* str  = janet_getstring(argv, 0);
        std::wstring   wstr = string::unicode::from_utf8(reinterpret_cast<const char*>(str));
        network::Node  n    = s_instance->_n->node(wstr, s_instance->_n->lang());
        return zelph_wrap_node(n);
    }

    // Execute a query: print the pattern and trigger matching via apply_rule.
    // This is the Janet equivalent of entering a zelph statement that contains
    // variables (e.g. "X ~ human"). Takes a single zelph/node argument
    // (typically the return value of a zelph/fact call containing variables).
    static Janet janet_cfun_zelph_query(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n) return janet_wrap_nil();

        // Build inverse mapping: variable Node -> symbol name
        // (must be done before apply_rule clears anything)
        std::map<network::Node, std::string> var_to_name;
        for (const auto& [name, node] : s_instance->_scoped_variables)
        {
            var_to_name[node] = name;
        }

        // Collect results instead of printing them
        std::vector<std::shared_ptr<network::Variables>> results;

        if (!s_instance->_scoped_variables.empty())
        {
            s_instance->_n->set_query_collector(&results);
            s_instance->_n->apply_rule(0, n);
            s_instance->_n->set_query_collector(nullptr);
        }

        // Reset variable scope for the next query/statement
        s_instance->_scoped_variables.clear();

        // Convert results to Janet array of tables:
        // @[@{X <zelph/node ...> Y <zelph/node ...>} ...]
        JanetArray* result_array = janet_array(static_cast<int32_t>(results.size()));

        for (const auto& vars : results)
        {
            JanetTable* entry = janet_table(static_cast<int32_t>(var_to_name.size()));

            for (const auto& [var_node, bound_node] : *vars)
            {
                auto it = var_to_name.find(var_node);
                if (it != var_to_name.end())
                {
                    Janet key = janet_wrap_symbol(janet_symbol(
                        reinterpret_cast<const uint8_t*>(it->second.c_str()),
                        static_cast<int32_t>(it->second.size())));
                    Janet val = zelph_wrap_node(bound_node);
                    janet_table_put(entry, key, val);
                }
            }

            janet_array_push(result_array, janet_wrap_table(entry));
        }

        return janet_wrap_array(result_array);
    }

    // Helper to generate Janet code for a function call with potential focused arguments.
    // func_name: "zelph/fact" or "zelph/set"
    // args: Array of Janet tuples (the AST nodes)
    std::string build_smart_call(const std::string& func_name, const std::vector<Janet>& args) const
    {
        if (args.empty()) return "nil";

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
        else if (type == "seq-nodes")
        {
            std::vector<Janet> args;
            for (int32_t i = 1; i < len; ++i)
                args.push_back(data[i]);
            return build_smart_call("zelph/seq", args);
        }

        // Handle leaf nodes (atom, var, seq, unquote)
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
            // Atoms are strings in Janet.
            if (val_str.size() >= 2 && val_str.front() == '"' && val_str.back() == '"')
            {
                return val_str; // Already quoted
            }
            else
            {
                // Wrap in quotes and escape
                return "\"" + boost::replace_all_copy(val_str, "\"", "\\\"") + "\"";
            }
        }
        else if (type == "seq-compact")
        {
            // Convert <123> content to (zelph/seq "123")
            // Escape the content string for Janet
            std::string content = "\"" + boost::replace_all_copy(val_str, "\"", "\\\"") + "\"";
            return "(zelph/seq-chars " + content + ")";
        }

        return "nil";
    }
};

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

std::string ScriptEngine::parse_zelph_to_janet(const std::string& input) const
{
    JanetSymbol      match_sym = janet_csymbol("zelph-safe-parse");
    Janet            match_fun_out;
    JanetBindingType bt = janet_resolve(_pImpl->_janet_env, match_sym, &match_fun_out);

    if (bt != JANET_BINDING_DEF) return "";

    JanetFunction* match_fun = janet_unwrap_function(match_fun_out);
    Janet          args[2]   = {_pImpl->_zelph_peg, janet_cstringv(input.c_str())};
    Janet          result;

    if (janet_pcall(match_fun, 2, args, &result, nullptr) != JANET_SIGNAL_OK)
    {
        return "";
    }
    if (janet_checktype(result, JANET_NIL))
    {
        return "";
    }

    JanetArray* tree = janet_unwrap_array(result);
    if (tree->count < 1) return "";

    // The PEG now returns `[:root val1 val2 ...]`
    const Janet* root_data;
    int32_t      root_len;
    if (!janet_indexed_view(tree->data[0], &root_data, &root_len)) return "";

    std::string type = reinterpret_cast<const char*>(janet_unwrap_keyword(root_data[0]));
    if (type != "root") return "";

    // Check how many items we have
    // root_data[0] is :root tag
    // root_data[1..n] are the values
    int val_count = root_len - 1;

    if (val_count == 0) return "";

    if (val_count == 1)
    {
        // Single term (e.g. { ... } or just "Atom")
        // Just return the term itself to be evaluated/printed
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
        return _pImpl->build_smart_call("zelph/fact", fact_args);
    }
}

void ScriptEngine::process_janet(const std::string& code, bool is_zelph_ast)
{
    _pImpl->_scoped_variables.clear();

    Janet out;
    int   status = janet_dostring(_pImpl->_janet_env, code.c_str(), "zelph-script", &out);

    if (status != JANET_SIGNAL_OK)
    {
        janet_stacktrace(nullptr, out);
    }
    else
    {
        if (is_zelph_ast)
        {
            network::Node n = zelph_unwrap_node(out);
            if (n)
            {
                std::wstring output;
                _pImpl->_n->format_fact(output, _pImpl->_n->lang(), n, 3);
                if (!output.empty() && output != L"??") _pImpl->_n->print(output, false);

                if (!_pImpl->_scoped_variables.empty())
                {
                    _pImpl->_n->apply_rule(0, n);
                }
            }
        }
        else
        {
            if (!janet_checktype(out, JANET_NIL))
            {
                janet_eprintf("%v\n", out);
            }
        }
    }
}

// Helper function to evaluate a Janet expression and return a Node (used by prune commands)
network::Node ScriptEngine::evaluate_expression(const std::string& janet_code)
{
    _pImpl->_scoped_variables.clear(); // Reset scopes for new evaluation context
    Janet out;
    int   status = janet_dostring(_pImpl->_janet_env, janet_code.c_str(), "eval_expr", &out);
    if (status != JANET_SIGNAL_OK)
    {
        janet_stacktrace(nullptr, out);
        throw std::runtime_error("Error evaluating janet expression");
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

bool ScriptEngine::is_expression_complete(const std::string& code) const
{
    int  depth      = 0;
    bool in_string  = false;
    bool escape     = false;
    bool in_comment = false;

    for (size_t i = 0; i < code.size(); ++i)
    {
        char c = code[i];

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

bool ScriptEngine::is_var(std::wstring token)
{
    // Legacy helper, might still be useful outside of PEG context
    static const std::wstring variable_names(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ_");
    if (token.empty()) return false;
    if (token.size() == 1) return variable_names.find(*token.begin()) != std::wstring::npos;
    return *token.begin() == L'_';
}
