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
#include "network.hpp"
#include "reasoning.hpp"
#include "string_utils.hpp"

#include <janet.h>

#include <boost/algorithm/string.hpp>

#include <iostream>
#include <map>

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
    NULL,                // gc
    NULL,                // gcmark
    NULL,                // get
    NULL,                // put
    NULL,                // marshal
    NULL,                // unmarshal
    zelph_node_tostring, // tostring
    zelph_node_compare,  // compare
    zelph_node_hash,     // hash
    NULL,                // next
    NULL,                // call
    NULL,                // length
    NULL,                // bytes
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
    Janet               _zelph_peg;

    // Track variables used in the current scope/statement
    std::map<std::string, network::Node> _scoped_variables;

    Impl(network::Reasoning* n)
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
        _janet_env = janet_core_env(NULL);
        register_zelph_functions();
        setup_peg();
    }

    void register_zelph_functions()
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
        janet_def(_janet_env, "zelph/fact", wrap((JanetCFunction)janet_cfun_zelph_fact), "(zelph/fact subject predicate object)\nCreate fact in zelph.");
        janet_def(_janet_env, "zelph/rule", wrap((JanetCFunction)janet_cfun_zelph_rule), "(zelph/rule conds deducs)\nCreate rule in zelph.");
    }

    void setup_peg()
    {
        std::string peg_setup = R"(
            (def zelph-grammar
              ~{:ws (set " \t\r\f\n\0\v")
                :s* (any :ws)
                :s+ (some :ws)
                :symchars (+ (range "09" "AZ" "az" "\x80\xFF") (set "!$%&*+-./:<>?@^_~") (set "=><,"))

                :sep-rule "=>"
                :sep-cond ","

                :var (capture (choice (range "AZ") (sequence "_" (any :symchars))))
                :quoted (capture (* "\"" (any (if-not "\"" 1)) "\""))
                :raw-tok (capture (some :symchars))

                :val-any (choice :quoted :var :raw-tok)

                # Safe value excludes => and , to prevent consuming delimiters in rules
                :val-safe (if-not (+ :sep-rule :sep-cond) :val-any)

                :stmt-safe (sequence :val-safe :s+ :val-safe (some (sequence :s+ :val-safe)))
                :grp-stmt-safe (group :stmt-safe)

                # Conditions list: stmt , stmt , stmt
                :cond-list (sequence :grp-stmt-safe (any (sequence :s* :sep-cond :s* :grp-stmt-safe)))

                :rule-def (sequence (constant "rule")
                                    (group :cond-list)
                                    :s* :sep-rule :s*
                                    (group :cond-list))

                :stmt-any (sequence :val-any :s+ :val-any (some (sequence :s+ :val-any)))
                :fact-def (sequence (constant "fact") :stmt-any)

                :main (sequence :s* (choice :rule-def :fact-def) :s*)})

            (defn zelph-safe-parse [peg text]
               (peg/match peg text))
        )";

        Janet out;
        int   status = janet_dostring(_janet_env, peg_setup.c_str(), "setup", &out);
        if (status != JANET_SIGNAL_OK) janet_stacktrace(NULL, out);

        janet_dostring(_janet_env, "(def zelph-peg (peg/compile zelph-grammar))", "init", &out);
        _zelph_peg = out;
        janet_gcroot(_zelph_peg);
    }

    network::Node resolve_janet_arg(Janet arg)
    {
        if (janet_checktype(arg, JANET_ABSTRACT) || janet_checktype(arg, JANET_NUMBER))
        {
            return zelph_unwrap_node(arg);
        }
        else if (janet_checktype(arg, JANET_STRING))
        {
            const uint8_t* str  = janet_unwrap_string(arg);
            std::wstring   wstr = string::unicode::from_utf8(reinterpret_cast<const char*>(str));

            // Check if it matches a core node name (unquoted in source)
            network::Node core_node = _n->get_core_node(wstr);
            if (core_node != 0)
            {
                return core_node;
            }
            else if (wstr.size() >= 2 && wstr.front() == L'"' && wstr.back() == L'"')
            {
                // It was quoted in the source, so it's a single Node
                wstr = wstr.substr(1, wstr.size() - 2);
                return _n->node(wstr, _n->lang());
            }
            else
            {
                // It was unquoted in the source, so it's a sequence of characters
                std::vector<std::wstring> chars;
                chars.reserve(wstr.length());
                for (wchar_t c : wstr)
                {
                    chars.push_back(std::wstring(1, c));
                }
                return _n->sequence(chars);
            }
        }
        else if (janet_checktype(arg, JANET_SYMBOL))
        {
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

    static Janet janet_cfun_zelph_rule(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        auto extract_nodes = [](Janet val, network::adjacency_set& target)
        {
            const Janet* arr;
            int32_t      len;
            if (janet_indexed_view(val, &arr, &len))
            {
                for (int i = 0; i < len; ++i)
                {
                    // Use standard resolution to handle Numbers, Strings (Sequences), and Symbols (Vars)
                    network::Node n = s_instance->resolve_janet_arg(arr[i]);
                    if (n) target.insert(n);
                }
            }
        };

        network::adjacency_set conds, deducs;
        extract_nodes(argv[0], conds);
        extract_nodes(argv[1], deducs);

        if (conds.empty() || deducs.empty()) return janet_wrap_nil();

        network::Node cond_node = (conds.size() == 1) ? *conds.begin()
                                                      : s_instance->_n->condition(s_instance->_n->core.And, conds);

        network::Node rule = s_instance->_n->fact(cond_node, s_instance->_n->core.Causes, deducs);
        return zelph_wrap_node(rule);
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

    std::string transform_token(Janet tok) const
    {
        if (janet_checktype(tok, JANET_STRING))
        {
            const uint8_t* tok_str = janet_unwrap_string(tok);
            std::string    tok_s   = reinterpret_cast<const char*>(tok_str);
            std::wstring   wtok    = string::unicode::from_utf8(tok_s);

            if (is_var(wtok))
            {
                return "'" + tok_s;
            }
            else
            {
                return "\"" + boost::replace_all_copy(tok_s, "\"", "\\\"") + "\"";
            }
        }
        return "";
    }

    std::string transform_subtree(Janet subtree) const
    {
        if (!janet_checktype(subtree, JANET_ARRAY) && !janet_checktype(subtree, JANET_TUPLE)) return "";

        const Janet* data;
        int32_t      len;
        janet_indexed_view(subtree, &data, &len);

        std::string result = "[";
        for (int32_t i = 0; i < len; ++i)
        {
            if (janet_checktype(data[i], JANET_ARRAY) || janet_checktype(data[i], JANET_TUPLE))
            {
                const Janet* fact_data;
                int32_t      fact_len;
                janet_indexed_view(data[i], &fact_data, &fact_len);

                if (fact_len < 3) continue;

                std::string subj = transform_token(fact_data[0]);
                std::string pred = transform_token(fact_data[1]);
                std::string objs = "";
                for (int32_t j = 2; j < fact_len; ++j)
                {
                    objs += transform_token(fact_data[j]) + " ";
                }
                result += "(zelph/fact " + subj + " " + pred + " " + objs + ") ";
            }
        }
        return result + "]";
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

    if (janet_pcall(match_fun, 2, args, &result, NULL) != JANET_SIGNAL_OK)
    {
        return "";
    }
    if (janet_checktype(result, JANET_NIL))
    {
        return "";
    }

    JanetArray* tree = janet_unwrap_array(result);
    if (tree->count < 1) return "";

    const uint8_t* type_str = janet_unwrap_string(tree->data[0]);
    std::string    type     = reinterpret_cast<const char*>(type_str);

    if (type == "fact")
    {
        std::string subj = _pImpl->transform_token(tree->data[1]);
        std::string pred = _pImpl->transform_token(tree->data[2]);
        std::string objs = "";
        for (int32_t i = 3; i < tree->count; ++i)
        {
            objs += _pImpl->transform_token(tree->data[i]) + " ";
        }
        return "(zelph/fact " + subj + " " + pred + " " + objs + ")";
    }
    else if (type == "rule")
    {
        std::string conds  = _pImpl->transform_subtree(tree->data[1]);
        std::string deducs = _pImpl->transform_subtree(tree->data[2]);
        return "(zelph/rule " + conds + " " + deducs + ")";
    }
    return "";
}

void ScriptEngine::process_janet(const std::string& code, bool is_zelph_ast)
{
    _pImpl->_scoped_variables.clear();

    Janet out;
    int   status = janet_dostring(_pImpl->_janet_env, code.c_str(), "zelph-script", &out);

    if (status != JANET_SIGNAL_OK)
    {
        janet_stacktrace(NULL, out);
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
        janet_stacktrace(NULL, out);
        throw std::runtime_error("Error evaluating janet expression");
    }
    return zelph_unwrap_node(out);
}

void ScriptEngine::set_script_args(const std::vector<std::string>& args)
{
    JanetArray* jargs = janet_array(args.size());
    for (const auto& arg : args)
    {
        janet_array_push(jargs, janet_cstringv(arg.c_str()));
    }
    janet_table_put(_pImpl->_janet_env, janet_ckeywordv("args"), janet_wrap_array(jargs));
}

bool ScriptEngine::is_var(std::wstring token)
{
    static const std::wstring variable_names(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ_");

    switch (token.size())
    {
    case 0:
        return false;
    case 1:
        return variable_names.find(*token.begin()) != std::wstring::npos;
    default:
        return *token.begin() == L'_';
    }
}
