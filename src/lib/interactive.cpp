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

#include "interactive.hpp"

#include "network.hpp"
#include "platform_utils.hpp"
#include "reasoning.hpp"
#include "stopwatch.hpp"
#include "string_utils.hpp"
#include "wikidata.hpp"
#include "wikidata_text_compressor.hpp"

#include <janet.h>

#include <boost/algorithm/string.hpp>
#include <boost/bimap.hpp>
#include <boost/tokenizer.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>

#ifdef _WIN32
    #include <fcntl.h> // for _O_U16TEXT
    #include <io.h>    // for _setmode
    #include <stdio.h> // for _fileno
#endif

using namespace zelph;
using boost::escaped_list_separator;
using boost::tokenizer;

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
    // Simple hash for uint64
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
    // Fallback for small numbers (e.g., literals in the script such as 0 or 1)
    if (janet_checktype(val, JANET_NUMBER))
    {
        return (network::Node)janet_unwrap_number(val);
    }
    return 0;
}

class console::Interactive::Impl
{
public:
    static Impl* s_instance;

    Impl(Interactive* enclosing)
        : _n(new network::Reasoning([](const std::wstring& str, const bool)
                                    {
#ifdef _WIN32
                                        std::wcout << str << std::endl;
#else
                                        std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
                                    }))
        , _interactive(enclosing)
    {
        s_instance = this;

#ifdef _WIN32
        _setmode(_fileno(stdout), _O_U16TEXT);
#endif

        _n->set_lang("zelph");

        _n->register_core_node(_n->core.RelationTypeCategory, L"->");
        _n->register_core_node(_n->core.Causes, L"=>");
        _n->register_core_node(_n->core.And, L",");
        _n->register_core_node(_n->core.IsA, L"~");
        _n->register_core_node(_n->core.Unequal, L"!=");
        _n->register_core_node(_n->core.Contradiction, L"!");
        _n->register_core_node(_n->core.FollowedBy, L"..");
        _n->register_core_node(_n->core.PartOf, L"in");

        janet_init();
        _janet_env = janet_core_env(NULL);
        register_zelph_functions();

        std::string peg_setup = R"(
            (def zelph-grammar
              ~{:ws (set " \t\r\f\n\0\v")
                :s* (any :ws)
                :s+ (some :ws)
                :symchars (+ (range "09" "AZ" "az" "\x80\xFF") (set "!$%&*+-./:<?@^_") (set "=><,"))

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
        process_janet_setup(peg_setup);

        Janet out;
        janet_dostring(_janet_env, "(def zelph-peg (peg/compile zelph-grammar))", "init", &out);
        _zelph_peg = out;
        janet_gcroot(_zelph_peg);
    }

    ~Impl()
    {
        if (s_instance == this) s_instance = nullptr;
        janet_gcunroot(_zelph_peg);
        janet_deinit();
    }

    void                       import_file(const std::wstring& file) const;
    void                       process_command(const std::vector<std::wstring>& cmd);
    void                       display_node_details(network::Node nd, bool resolved_from_name, int depth, int max_neighbors) const;
    static bool                is_var(std::wstring token);
    void                       list_predicate_usage(size_t limit);
    void                       list_predicate_value_usage(const std::wstring& pred_arg, size_t limit);
    network::Node              resolve_node(const std::wstring& arg, std::string lang) const;
    network::Node              resolve_single_node(const std::wstring& arg, bool prioritize_id = false) const;
    std::vector<network::Node> resolve_nodes_by_name(const std::wstring& name) const;
    void                       generate_and_print_mermaid_link(network::Node nd, int depth, int max_neighbors) const;

    std::string transform_subtree(Janet subtree) const;
    std::string transform_token(Janet tok) const;

    std::string   parse_zelph_to_janet(const std::string& input) const;
    void          process_janet(const std::string& code, bool is_zelph_ast);
    network::Node evaluate_janet_expression(const std::string& janet_code);
    void          process_janet_setup(const std::string& code) const;

    void process_zelph_file(const std::string& path, const std::vector<std::string>& args = {}) const;

    std::shared_ptr<DataManager> _data_manager;
    network::Reasoning* const    _n;

    std::map<std::string, network::Node> _scoped_variables;

    JanetTable* _janet_env = nullptr;
    Janet       _zelph_peg;

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

private:
    const Interactive* _interactive;

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
};

console::Interactive::Impl* console::Interactive::Impl::s_instance = nullptr;

console::Interactive::Interactive()
    : _pImpl(new Impl(this))
{
}

console::Interactive::~Interactive()
{
    delete _pImpl;
}

void console::Interactive::Impl::import_file(const std::wstring& file) const
{
    std::clog << "Importing file " << string::unicode::to_utf8(file) << "..." << std::endl;
    std::wifstream stream(string::unicode::to_utf8(file));

    if (stream.fail()) throw std::runtime_error("Could not open file '" + string::unicode::to_utf8(file) + "'");

    for (std::wstring line; std::getline(stream, line);)
    {
        _interactive->process(line);
    }
}

std::string console::Interactive::Impl::parse_zelph_to_janet(const std::string& input) const
{
    JanetSymbol      match_sym = janet_csymbol("zelph-safe-parse");
    Janet            match_fun_out;
    JanetBindingType bt = janet_resolve(_janet_env, match_sym, &match_fun_out);

    if (bt != JANET_BINDING_DEF) return "";

    JanetFunction* match_fun = janet_unwrap_function(match_fun_out);

    Janet args[2] = {_zelph_peg, janet_cstringv(input.c_str())};
    Janet result;

    if (janet_pcall(match_fun, 2, args, &result, NULL) != JANET_SIGNAL_OK)
    {
        return "";
    }

    if (janet_checktype(result, JANET_NIL)) return "";

    JanetArray* tree = janet_unwrap_array(result);
    if (tree->count < 1) return "";

    const uint8_t* type_str = janet_unwrap_string(tree->data[0]);
    std::string    type     = reinterpret_cast<const char*>(type_str);

    if (type == "fact")
    {
        std::string subj = transform_token(tree->data[1]);
        std::string pred = transform_token(tree->data[2]);
        std::string objs = "";
        for (int32_t i = 3; i < tree->count; ++i)
        {
            objs += transform_token(tree->data[i]) + " ";
        }
        return "(zelph/fact " + subj + " " + pred + " " + objs + ")";
    }
    else if (type == "rule")
    {
        std::string conds  = transform_subtree(tree->data[1]);
        std::string deducs = transform_subtree(tree->data[2]);
        return "(zelph/rule " + conds + " " + deducs + ")";
    }

    return "";
}

std::string console::Interactive::Impl::transform_token(Janet tok) const
{
    if (janet_checktype(tok, JANET_STRING))
    {
        const uint8_t* tok_str = janet_unwrap_string(tok);
        std::string    tok_s   = reinterpret_cast<const char*>(tok_str);
        std::wstring   wtok    = string::unicode::from_utf8(tok_s);

        if (is_var(wtok))
        {
            // Variables must be quoted so that Janet passes them as symbols and does not attempt to resolve them immediately
            return "'" + tok_s;
        }
        else
        {
            // inner quotes must be escaped, then surround the string with quotes
            return "\"" + boost::replace_all_copy(tok_s, "\"", "\\\"") + "\"";
        }
    }
    return "";
}

std::string console::Interactive::Impl::transform_subtree(Janet subtree) const
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

void console::Interactive::Impl::process_janet(const std::string& code, bool is_zelph_ast)
{
    _scoped_variables.clear();

    Janet out;
    int   status = janet_dostring(_janet_env, code.c_str(), "zelph-script", &out);

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
                _n->format_fact(output, _n->lang(), n, 3);
                if (!output.empty() && output != L"??") _n->print(output, false);

                if (!_scoped_variables.empty())
                {
                    _n->apply_rule(0, n);
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
network::Node console::Interactive::Impl::evaluate_janet_expression(const std::string& janet_code)
{
    Janet out;
    int   status = janet_dostring(_janet_env, janet_code.c_str(), "eval_expr", &out);
    if (status != JANET_SIGNAL_OK)
    {
        janet_stacktrace(NULL, out);
        throw std::runtime_error("Error evaluating janet expression");
    }
    return zelph_unwrap_node(out);
}

void console::Interactive::Impl::process_zelph_file(const std::string& path, const std::vector<std::string>& args) const
{
    // Setze args in Janet-Env
    JanetArray* jargs = janet_array(args.size());
    for (const auto& arg : args)
    {
        janet_array_push(jargs, janet_cstringv(arg.c_str()));
    }
    janet_table_put(_janet_env, janet_ckeywordv("args"), janet_wrap_array(jargs));

    std::ifstream file(path);
    if (!file) throw std::runtime_error("Konnte Datei nicht öffnen: " + path);
    std::string line;
    while (std::getline(file, line))
    {
        boost::trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::wstring wline = string::unicode::from_utf8(line);
        _interactive->process(wline);
    }
}

// Neu: Public Wrapper für process_file
void console::Interactive::process_file(const std::wstring& file, const std::vector<std::string>& args) const
{
    _pImpl->process_zelph_file(string::unicode::to_utf8(file), args);
}

std::string console::Interactive::get_version() const
{
    return network::Zelph::get_version();
}

void console::Interactive::process(std::wstring line) const
{
    _pImpl->_n->set_print([](const std::wstring& str, bool)
                          {
#ifdef _WIN32
                              std::wcout << str << std::endl;
#else
                              std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
                          });

    try
    {
        if (boost::starts_with(line, "#")) return; // comment

        size_t first_char_pos = line.find_first_not_of(L" \t");
        if (first_char_pos != std::wstring::npos && line[first_char_pos] == L'.')
        {
            // Command processing logic deliberately does not use PEG
            tokenizer<escaped_list_separator<wchar_t>, std::wstring::const_iterator, std::wstring> tok(line, escaped_list_separator<wchar_t>(L"\\", L" \t", L"\""));
            auto                                                                                   it = tok.begin();
            while (it != tok.end() && it->empty())
                ++it;

            if (it != tok.end() && (*it)[0] == L'.')
            {
                std::vector<std::wstring> cmd;
                while (it != tok.end())
                {
                    if (!it->empty()) cmd.push_back(*it);
                    ++it;
                }
                _pImpl->process_command(cmd);
                return;
            }
        }

        std::string utf8_line   = string::unicode::to_utf8(line);
        std::string transformed = _pImpl->parse_zelph_to_janet(utf8_line);

        // 1. Zelph syntax parsed via PEG
        if (!transformed.empty())
        {
            _pImpl->process_janet(transformed, true);
            return;
        }

        // 2. Native Janet syntax
        size_t u_first = utf8_line.find_first_not_of(" \t");
        if (u_first != std::string::npos && utf8_line[u_first] == '(')
        {
            _pImpl->process_janet(utf8_line, false);
            return;
        }

        // 3. Syntax Error
        if (u_first != std::string::npos) // Line is not empty
        {
            throw std::runtime_error("Syntax error: Could not parse line.");
        }
    }
    catch (std::exception& ex)
    {
        throw std::runtime_error("Error in line \"" + string::unicode::to_utf8(line) + "\": " + ex.what());
    }
}

void console::Interactive::import_file(const std::wstring& file) const
{
    _pImpl->import_file(file);
}

void console::Interactive::Impl::display_node_details(network::Node nd, bool resolved_from_name, int depth, int max_neighbors) const
{
    if (resolved_from_name)
    {
        std::clog << "Resolved to node ID: " << nd << std::endl;
    }

    std::clog << "Node ID: " << nd << std::endl;

    {
        std::wstring core_name = _n->get_core_name(nd);
        if (!core_name.empty())
        {
            std::clog << "  Core node: " << string::unicode::to_utf8(core_name) << std::endl;
        }
    }

    if (network::Network::is_var(nd))
    {
        std::clog << "  Variable: yes" << std::endl;
    }
    else
    {
        std::clog << "  Variable: no" << std::endl;
    }

    bool         has_wikidata = false;
    std::wstring wikidata_name;

    bool has_any_name = false;
    for (const std::string& lang : _n->get_languages())
    {
        std::wstring name = _n->get_name(nd, lang, false);
        if (!name.empty())
        {
            has_any_name = true;
            std::clog << "  Name in language '" << lang << "': '"
                      << string::unicode::to_utf8(name) << "'" << std::endl;

            if (lang == "wikidata")
            {
                has_wikidata  = true;
                wikidata_name = name;
            }
        }
    }

    if (!has_any_name)
    {
        std::clog << "  (No names in any language)" << std::endl;
    }

    if (has_wikidata)
    {
        std::string prefix = (wikidata_name[0] == L'P') ? "Property:" : "";
        std::string url    = "https://www.wikidata.org/wiki/" + prefix + string::unicode::to_utf8(wikidata_name);

        const std::string OSC_START = "\033]8;;";
        const char        OSC_SEP   = '\a';
        const std::string OSC_END   = "\033]8;;\a";

        std::clog << "  Wikidata URL: " << OSC_START << url << OSC_SEP << url << OSC_END << std::endl;
    }

    if (depth > 0)
    {
        generate_and_print_mermaid_link(nd, depth, max_neighbors);
    }

    auto format_node = [this, max_neighbors](network::Node node) -> std::string
    {
        std::wstring node_str  = std::to_wstring(node);
        std::wstring node_name = _n->get_name(node, _n->lang(), true); // fallback active

        if (node_str == node_name || node_name.empty())
        {
            std::wstring fact_repr;
            _n->format_fact(fact_repr, _n->lang(), node, max_neighbors);
            if (!fact_repr.empty() && fact_repr != L"??")
            {
                return string::unicode::to_utf8(fact_repr) + " (ID " + std::to_string(node) + ")";
            }
            else
            {
                return "ID " + std::to_string(node);
            }
        }
        else
        {
            return string::unicode::to_utf8(node_name) + " (ID " + std::to_string(node) + ")";
        }
    };

    auto display_connections = [&](const network::adjacency_set& conns, const std::string& header)
    {
        if (conns.empty())
        {
            return;
        }

        std::clog << "  " << header << ":" << std::endl;

        if (conns.size() <= max_neighbors)
        {
            for (network::Node node : conns)
            {
                std::clog << "    - " << format_node(node) << std::endl;
            }
        }
        else
        {
            std::clog << "    (" << conns.size() << " connections)" << std::endl;
        }
    };

    const network::adjacency_set& incoming = _n->get_left(nd);
    const network::adjacency_set& outgoing = _n->get_right(nd);

    display_connections(incoming, "Incoming connections from");
    display_connections(outgoing, "Outgoing connections to");

    std::wstring fact_repr;
    _n->format_fact(fact_repr, _n->lang(), nd, max_neighbors);
    if (!fact_repr.empty() && fact_repr != L"??")
    {
        std::clog << "  Representation: " << string::unicode::to_utf8(fact_repr) << std::endl;
    }

    std::clog << "------------------------" << std::endl;
}

void console::Interactive::Impl::process_command(const std::vector<std::wstring>& cmd)
{
    static const std::vector<std::wstring> general_help_lines = {
        L"zelph Interactive Help",
        L"",
        L"Basic Syntax",
        L"────────────",
        L"Facts:    <subject> <predicate> <object>",
        L"          Predicates with spaces must be quoted on first use.",
        L"          Example: peter \"is father of\" paul",
        L"          → «peter» «is father of» «paul»",
        L"          Subsequent use: peter is father of paul",
        L"          → «peter» «is father of» «paul»",
        L"",
        L"Rules:    <condition1>, <condition2>, ... => <deduction1>, <deduction2>, ...",
        L"          Rules are stored but not automatically applied.",
        L"          Use .run to perform inference and see deductions.",
        L"",
        L"Queries:  Statements containing variables (A-Z or starting with _).",
        L"          Queries are answered immediately (no .run needed).",
        L"          Example: A is father of paul",
        L"          → Answer: «peter» «is father of» «paul»",
        L"",
        L"Examples",
        L"Berlin \"is capital of\" Germany",
        L"Germany \"is located in\" Europe",
        L"X is capital of Y, Y is located in Z => X is located in Z",
        L".run",
        L"→ «Berlin» «is located in» «Europe» ⇐ («Germany» «is located in» «Europe»), («Berlin» «is capital of» «Germany»)",
        L"",
        L"Available Commands",
        L"──────────────────",
        L".help [command]             – Show this help or detailed help for a specific command",
        L".exit                       – Exit interactive mode",
        L".lang [code]                – Show or set current language",
        L".name <node|id> <new_name>         – Set name in current language",
        L".name <node|id> <lang> <new_name>  – Set name in specific language",
        L".delname <node|id> [lang]          – Delete name in current language (or specified language)",
        L".node <name|id>                    – Show detailed node information (names, connections, representation, Wikidata URL)",
        L".list <count>                      – List first N existing nodes (internal map order, with details)",
        L".clist <count>                     – List first N nodes named in current language (sorted by ID if reasonable size, otherwise map order)",
        L".out <name|id> [count]             – List details of outgoing connected nodes (default 20)",
        L".in <name|id> [count]              – List details of incoming connected nodes (default 20)",
        L".mermaid <node_name> [max_depth]   – Generate Mermaid HTML file for a node",
        L".run                        – Run full inference",
        L".run-once                   – Run a single inference pass",
        L".run-md <subdir>            – Run inference and export results as Markdown",
        L".run-file <file>            – Run inference, write deduced facts (reversed order) to <file> (encoded if lang=wikidata)",
        L".decode <file>              – Decode an encoded/plain file and print readable facts",
        L".list-rules                 – List all defined inference rules",
        L".list-predicate-usage [max] – Show predicate usage statistics (top N most frequent predicates)",
        L".list-predicate-value-usage <pred> [max] – Show object/value usage statistics for a specific predicate (top N most frequent values)",
        L".remove-rules               – Remove all inference rules",
        L".remove <name|id>           – Remove a node (destructive: disconnects all edges and cleans names)",
        L".import <file.zph>          – Load and execute a zelph script file",
        L".load <file>                – Load a saved network (.bin) or import Wikidata JSON dump (creates .bin cache)",
        L".save <file.bin>            – Save the current network to a binary file",
        L".prune-facts <pattern>      – Remove all facts matching the query pattern (only statements)",
        L".prune-nodes <pattern>      – Remove matching facts AND all involved subject/object nodes",
        L".cleanup                    – Remove isolated nodes and clean name mappings",
        L".stat                       – Show network statistics (nodes, RAM usage, name entries, languages, rules)",
        L".wikidata-constraints <json> <dir> – Export constraints to a directory",
        L"",
        L"Type \".help <command>\" for detailed information about a specific command."};

    static const std::map<std::wstring, std::wstring> detailed_help = {
        {L".help", L".help [command]\n"
                   L"Without argument: shows this general help text with syntax and command overview.\n"
                   L"With argument: shows detailed help for the specified command."},

        {L".exit", L".exit\n"
                   L"Exits the interactive REPL session."},

        {L".lang", L".lang [language_code]\n"
                   L"Without argument: displays the current language used for node names.\n"
                   L"With argument: sets the language (e.g., 'zelph', 'en', 'de', 'wikidata')."},

        {L".name", L".name <node|id> <new_name>\n"
                   L"Sets the name of the node in the current language.\n"
                   L".name <node|id> <lang> <new_name>\n"
                   L"Sets the name in the specified language.\n"
                   L"The <node|id> can be a name (in current language) or numeric node ID.\n"
                   L"Empty <new_name> is not allowed – use .delname to remove a name."},

        {L".delname", L".delname <node|id> [lang]\n"
                      L"Removes the name of the node in the current language (or the specified language if provided).\n"
                      L"The <node|id> can be a name (in current language) or numeric node ID.\n"
                      L"If the node had no name in the target language, nothing happens."},

        {L".list", L".list <count>\n"
                   L"Lists the first N existing nodes in the network (in internal map iteration order).\n"
                   L"For each node: ID, non-empty names in all languages, connection counts, representation, and Wikidata URL if available."},

        {L".clist", L".clist <count>\n"
                    L"Lists the first N nodes that have a name in the current language.\n"
                    L"If the language has a reasonable number of entries (≤ ~50k), nodes are sorted by ID.\n"
                    L"For very large languages (e.g. 'wikidata'), order follows the internal map (fast, no full sort)."},

        {L".out", L".out <name|id> [count]\n"
                  L"Lists detailed information for up to <count> nodes reachable via outgoing connections\n"
                  L"from the given node (default 20, sorted by node ID)."},

        {L".in", L".in <name|id> [count]\n"
                 L"Lists detailed information for up to <count> nodes that have outgoing connections\n"
                 L"to the given node (default 20, sorted by node ID)."},

        {L".node", L".node <name_or_id>\n"
                   L"Displays details for a single node: its ID, non-empty names in all languages,\n"
                   L"incoming/outgoing connection counts, and a clickable Wikidata URL if it has a Wikidata ID.\n"
                   L"The argument can be a name (in current language) or a numeric node ID."},

        {L".nodes", L".nodes <count>\n"
                    L"Lists the first N named nodes (nodes that have at least one name in any language),\n"
                    L"sorted by node ID. For each node: ID, non-empty names in all languages,\n"
                    L"incoming/outgoing connection counts, and Wikidata URL if available."},

        {L".clist", L".clist <count>\n"
                    L"Lists the first N nodes that have a name in the current language, sorted by node ID.\n"
                    L"Output format is identical to .list (names in all languages, connection counts, Wikidata URL)."},

        {L".mermaid", L".mermaid <node_name> [max_depth]\n"
                      L"Generates a Mermaid HTML file visualizing the specified node and its connections\n"
                      L"up to the given depth (default 3). The file is named <node_name>.html in the system temp dir.\n"
                      L"Outputs a clickable file:// link to the generated HTML."},

        {L".run", L".run\n"
                  L"Performs full inference: repeatedly applies all rules until no new facts are derived.\n"
                  L"Deductions are printed as they are found."},

        {L".run-once", L".run-once\n"
                       L"Performs a single inference pass."},

        {L".run-md", L".run-md <subdir>\n"
                     L"Runs full inference and exports all deductions and contradictions as Markdown files\n"
                     L"in the directory mkdocs/docs/<subdir> for use with MkDocs."},

        {L".run-file", L".run-file <file>\n"
                       L"Performs full inference. Deduced facts (positive conclusions and contradictions) are written to <file>\n"
                       L"in reversed order (reasons first, then ⇒ conclusion), without any brackets or markup.\n"
                       L"Console output remains unchanged (original order with ⇐ explanations).\n"
                       L"If the current language is 'wikidata' (set via .lang wikidata), Wikidata identifiers are heavily\n"
                       L"compressed for minimal file size. Otherwise the file contains plain readable text."},

        {L".decode", L".decode <file>\n"
                     L"Reads a file created by .run-file (encoded or plain) and prints the decoded facts\n"
                     L"in readable form to standard output."},

        {L".list-rules", L".list-rules\n"
                         L"Lists all currently defined inference rules in readable format."},

        {L".list-predicate-usage", L".list-predicate-usage [max_entries]\n"
                                   L"Shows how often each predicate (relation type) is used, sorted by frequency.\n"
                                   L"If <max_entries> is specified, only the top N most frequent predicates are shown.\n"
                                   L"If Wikidata language is active, Wikidata IDs are shown alongside names."},

        {L".list-predicate-value-usage", L".list-predicate-value-usage <predicate> [max_entries]\n"
                                         L"Shows how often each object (value) is used with the specified predicate, sorted by frequency.\n"
                                         L"The <predicate> can be a name (in the current language) or a numeric node ID.\n"
                                         L"If <max_entries> is specified, only the top N most frequent values are shown.\n"
                                         L"If the Wikidata language is available and active, Wikidata IDs are shown alongside names."},

        {L".remove-rules", L".remove-rules\n"
                           L"Deletes all inference rules from the network."},

        {L".remove", L".remove <name_or_id>\n"
                     L"Removes the specified node from the network, disconnecting all its edges\n"
                     L"and cleaning all name mappings. The argument can be a node name (looked up in the current language)\n"
                     L"or a numeric node ID.\n"
                     L"WARNING: This operation is destructive and irreversible!"},

        {L".import", L".import <file.zph>\n"
                     L"Loads and immediately executes a zelph script file."},

        {L".load", L".load <file>\n"
                   L"Loads a previously saved network state.\n"
                   L"- If <file> ends with '.bin': loads the serialized network directly (fast).\n"
                   L"- If <file> ends with '.json' (Wikidata dump): imports the data and automatically creates a '.bin' cache file\n"
                   L"  in the same directory for faster future loads."},

        {L".save", L".save <file.bin>\n"
                   L"Saves the current network state to a binary file.\n"
                   L"The filename must end with '.bin'."},

        {L".prune-facts", L".prune-facts <pattern>\n"
                          L"Removes only the matching facts (statement nodes).\n"
                          L"The pattern may contain variables in any position.\n"
                          L"Reports how many facts were removed."},

        {L".prune-nodes", L".prune-nodes <pattern>\n"
                          L"Removes all matching facts AND all nodes that appear as subject or object in these facts.\n"
                          L"Requirements:\n"
                          L"- The relation (predicate) must be fixed (no variable allowed in predicate position)\n"
                          L"- Variables are allowed in subject and/or object positions\n"
                          L"WARNING: This is highly destructive! It removes ALL connections of the affected nodes.\n"
                          L"The relation node itself becomes isolated and can be removed with .cleanup.\n"
                          L"Reports removed facts and nodes."},

        {L".cleanup", L".cleanup\n"
                      L"Removes all nodes that have no connections (isolated nodes).\n"
                      L"Also cleans up associated entries in name mappings."},

        {L".stat", L".stat\n"
                   L"Shows current network statistics:\n"
                   L"- Number of nodes\n"
                   L"- RAM usage (in GiB, if available)\n"
                   L"- Total entries in name-of-node mappings\n"
                   L"- Total entries in node-of-name mappings\n"
                   L"- Number of languages\n"
                   L"- Number of rules"},

        {L".wikidata-constraints", L".wikidata-constraints <json_file> <output_dir>\n"
                                   L"Processes the Wikidata dump and exports constraint scripts\n"
                                   L"to the specified output directory."}};

    if (cmd[0] == L".help")
    {
        if (cmd.size() == 1)
        {
            for (const auto& l : general_help_lines)
                _n->print(l, true);
        }
        else if (cmd.size() == 2)
        {
            auto it = detailed_help.find(cmd[1]);
            if (it != detailed_help.end())
            {
                _n->print(it->second, true);
            }
            else
            {
                _n->print(L"Unknown command: " + cmd[1] + L". Use \".help\" for a list of all commands.", true);
            }
        }
        else
        {
            throw std::runtime_error("Usage: .help [command]");
        }
        return;
    }
    else if (cmd[0] == L".lang")
    {
        if (cmd.size() < 2)
        {
            std::clog << "The current language is '" << _n->get_lang() << "'" << std::endl;
        }
        else
        {
            _n->set_lang(string::unicode::to_utf8(cmd[1]));
        }
    }
    else if (cmd[0] == L".name")
    {
        if (cmd.size() < 3 || cmd.size() > 4)
            throw std::runtime_error("Command .name: Invalid arguments. Usage: .name <node> <new_name>  or  .name <node> <lang> <new_name>");

        const std::wstring& name_in_current_lang = cmd[1];
        const std::wstring& name_in_target_lang  = cmd.size() == 3 ? cmd[2] : cmd[3];
        std::string         current_lang         = _n->get_lang();
        std::string         target_lang          = cmd.size() == 3 ? _n->lang() : string::unicode::to_utf8(cmd[2]);

        network::Node node_in_current_lang = resolve_node(name_in_current_lang, current_lang);
        network::Node node_in_target_lang  = resolve_node(name_in_target_lang, target_lang);

        if (current_lang == target_lang)
        {
            // In this case, name_in_current_lang is strictly interpreted as the old name that we use to reference
            // the existing node. It does not make sense to support creating a new node in this mode.
            if (node_in_current_lang == 0)
            {
                throw std::runtime_error("Node '" + string::unicode::to_utf8(name_in_current_lang) + "' does not exist");
            }
            else if (node_in_target_lang != 0)
            {
                throw std::runtime_error("Name '" + string::unicode::to_utf8(name_in_target_lang) + "' is already in use by node " + std::to_string(node_in_target_lang));
            }
            else
            {
                _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
            }
        }
        else if (node_in_current_lang == 0)
        {
            if (node_in_target_lang == 0)
            {
                node_in_current_lang = _n->node(name_in_current_lang);
                _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
                _n->print(L"Node '" + name_in_current_lang + L"' ('" + string::unicode::from_utf8(current_lang) + L"') / '" + name_in_target_lang + L"' ('" + string::unicode::from_utf8(target_lang) + L"') does not exist yet in either language => created it.", true);
            }
            else
            {
                _n->set_name(node_in_target_lang, name_in_current_lang, current_lang, true);
                _n->print(L"Node '" + name_in_target_lang + L"' ('" + string::unicode::from_utf8(target_lang) + L"') exists, assigned name '" + name_in_current_lang + L"' in '" + string::unicode::from_utf8(current_lang) + L"'.", true);
            }
        }
        else if (node_in_target_lang == 0)
        {
            _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
            _n->print(L"Node '" + name_in_current_lang + L"' ('" + string::unicode::from_utf8(current_lang) + L"') exists, assigned name '" + name_in_target_lang + L"' in '" + string::unicode::from_utf8(target_lang) + L"'.", true);
        }
        else if (name_in_current_lang == _n->get_name(node_in_current_lang, current_lang, false) && name_in_target_lang == _n->get_name(node_in_target_lang, target_lang, false))
        {
            _n->print(L"Node '" + name_in_current_lang + L"' ('" + string::unicode::from_utf8(current_lang) + L"') / '" + name_in_target_lang + L"' ('" + string::unicode::from_utf8(target_lang) + L"') have the requested names, but are different nodes => Merging them.", true);
            _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
        }
        else
        {
            throw std::runtime_error("Node '" + string::unicode::to_utf8(name_in_current_lang) + "' ('" + current_lang + "') / '" + string::unicode::to_utf8(name_in_target_lang) + "' ('" + target_lang + "') exists in both languages as different nodes => did not do anything)");
        }
    }
    else if (cmd[0] == L".delname")
    {
        if (cmd.size() < 2 || cmd.size() > 3)
            throw std::runtime_error("Command .delname: Invalid arguments. Usage: .delname <node|id> [lang]");

        network::Node nd = resolve_single_node(cmd[1], true); // prioritize ID

        std::string target_lang = _n->lang();
        if (cmd.size() == 3)
        {
            target_lang = string::unicode::to_utf8(cmd[2]);
        }

        _n->remove_name(nd, target_lang);

        _n->print(L"Removed name of node " + std::to_wstring(nd) + L" in language '" + string::unicode::from_utf8(target_lang) + L"' (if it existed).", true);
    }
    else if (cmd[0] == L".node")
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .node: Exactly one argument required");

        std::wstring arg = cmd[1];

        std::vector<network::Node> nodes;

        try
        {
            // Try single resolve (non-destructive: ID last)
            network::Node single = resolve_single_node(arg, false);
            nodes.push_back(single);
        }
        catch (...)
        {
            // Not a single node/ID → try name search (multiple possible)
            nodes = resolve_nodes_by_name(arg);
            if (nodes.empty())
            {
                throw std::runtime_error("No node found with name '" + string::unicode::to_utf8(arg) + "' in current language '" + _n->lang() + "'");
            }
        }

        if (nodes.size() == 1)
        {
            bool resolved_from_name = !_n->get_name(nodes[0], _n->lang(), false).empty() || std::all_of(arg.begin(), arg.end(), ::iswdigit);
            display_node_details(nodes[0], resolved_from_name && nodes.size() == 1, 3, 3);
        }
        else
        {
            std::clog << "Found " << nodes.size() << " nodes with name '" << string::unicode::to_utf8(arg)
                      << "' in current language '" << _n->lang() << "':" << std::endl;
            std::clog << "------------------------" << std::endl;

            // Sort by ID for consistent output
            std::sort(nodes.begin(), nodes.end());

            for (network::Node nd : nodes)
            {
                display_node_details(nd, true, 3, 3);
            }
        }
    }
    else if (cmd[0] == L".list")
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .list: Missing count parameter");

        size_t count = string::parse_count(cmd[1]);

        auto view = _n->get_all_nodes_view();

        std::clog << "Listing " << count << " nodes:" << std::endl;
        std::clog << "------------------------" << std::endl;

        size_t displayed = 0;
        for (auto it = view.begin(); it != view.end() && displayed < count; ++it, ++displayed)
        {
            display_node_details(it->first, false, 3, 3);
        }

        std::clog << "Displayed " << displayed << " nodes." << std::endl;
    }

    else if (cmd[0] == L".clist")
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .clist: Missing count parameter");

        size_t count = string::parse_count(cmd[1]);

        auto view = _n->get_lang_nodes_view(_n->lang());

        std::clog << "Listing first " << count << " nodes named in current language '" << _n->lang() << "'" << std::endl;
        std::clog << "------------------------" << std::endl;

        size_t displayed = 0;
        for (auto it = view.begin(); it != view.end() && displayed < count; ++it, ++displayed)
        {
            display_node_details(it->second, false, 3, 3);
        }
    }
    else if (cmd[0] == L".out" || cmd[0] == L".in")
    {
        bool outgoing = (cmd[0] == L".out");

        if (cmd.size() < 2) throw std::runtime_error(std::string("Command ") + string::unicode::to_utf8(cmd[0]) + ": Missing node argument");

        std::wstring  arg     = cmd[1];
        network::Node base_nd = resolve_node(arg, _n->lang()); // same resolve logic as .node/.remove

        if (base_nd == 0)
        {
            throw std::runtime_error("Unknown node");
        }

        size_t max_count = 20; // default
        if (cmd.size() >= 3)
        {
            max_count = string::parse_count(cmd[2]);
        }

        network::adjacency_set neighbors = outgoing ? _n->get_right(base_nd) : _n->get_left(base_nd);

        std::vector<network::Node> vec(neighbors.begin(), neighbors.end());
        std::sort(vec.begin(), vec.end());

        size_t to_display = std::min(max_count, vec.size());

        std::clog << (outgoing ? "Outgoing" : "Incoming")
                  << " connected nodes of " << base_nd
                  << " (first " << to_display << " of " << vec.size() << ", sorted by ID):" << std::endl;
        std::clog << "------------------------" << std::endl;

        for (size_t i = 0; i < to_display; ++i)
        {
            display_node_details(vec[i], false, 3, 3);
        }
    }
    else if (cmd[0] == L".remove")
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .remove requires exactly one argument: name or ID");

        std::wstring  arg = cmd[1];
        network::Node nd  = resolve_single_node(arg, true); // prioritize ID

        if (nd == 0)
        {
            try
            {
                size_t pos = 0;
                nd         = std::stoull(arg, &pos);
                if (pos != arg.length())
                {
                    throw std::exception();
                }
            }
            catch (const std::exception&)
            {
                throw std::runtime_error("Command .remove: Unknown node '" + string::unicode::to_utf8(arg) + "' in current language '" + _n->lang() + "'");
            }

            if (!_n->exists(nd))
            {
                throw std::runtime_error("Command .remove: Node '" + std::to_string(nd) + "' does not exist");
            }
        }

        _n->remove_node(nd);
        _n->print(L"Removed node " + std::to_wstring(nd) + L" (all edges disconnected, name mappings cleaned).", true);
        _n->print(L"Consider running .cleanup afterwards if needed.", true);
    }
    else if (cmd[0] == L".mermaid")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .mermaid: Missing node name to visualise");
        std::wstring  arg = cmd[1];
        network::Node nd  = resolve_single_node(arg, true);
        if (nd == 0) throw std::runtime_error("Command .mermaid: Unknown node '" + string::unicode::to_utf8(arg) + "'");
        int max_depth = 3; // Default
        if (cmd.size() == 3)
        {
            max_depth = std::stoi(string::unicode::to_utf8(cmd[2]));
            if (max_depth < 2) throw std::runtime_error("Command .mermaid: Maximum depth must be greater than 1");
        }
        generate_and_print_mermaid_link(nd, max_depth, 3);
    }
    else if (cmd[0] == L".run")
    {
        _n->run(true, false, false);
        _n->print(L"Ready.", true);
    }
    else if (cmd[0] == L".run-once")
    {
        _n->run(true, false, true);
        _n->print(L"Ready.", true);
    }
    else if (cmd[0] == L".run-md")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .run-md: Missing subdirectory parameter (e.g., '.run-md tree')");
        std::string subdir = string::unicode::to_utf8(cmd[1]);
        _n->set_markdown_subdir(subdir);
        _n->print(L"Running with markdown export...", true);
        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        _n->run(false, true, false);
    }
    else if (cmd[0] == L".run-file")
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .run-file requires exactly one argument: the output file path");

        std::string   outfile = string::unicode::to_utf8(cmd[1]);
        std::ofstream out(outfile);
        if (!out.is_open())
            throw std::runtime_error("Command .run-file: Cannot open output file '" + outfile + "'");

        zelph::WikidataTextCompressor compressor({U' ', U'\t', U'\n', U','});

        bool is_wikidata = (_n->get_lang() == "wikidata");

        auto normal_print = [](const std::wstring& str, bool)
        {
#ifdef _WIN32
            std::wcout << str << std::endl;
#else
            std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
        };

        std::function<void(const std::wstring&, bool)> encode_print =
            [&compressor, &out, normal_print, is_wikidata](const std::wstring& str, bool important)
        {
            normal_print(str, important);

            size_t pos = str.find(L" ⇐ ");
            if (pos == std::wstring::npos)
                return;

            std::wstring deduction = str.substr(0, pos);
            boost::trim(deduction);

            std::wstring reasons = str.substr(pos + 3);
            boost::trim(reasons);

            if (!reasons.empty() && reasons.front() == L'(')
                reasons.erase(0, 1);
            if (!reasons.empty() && reasons.back() == L')')
                reasons.erase(reasons.size() - 1);
            boost::trim(reasons);

            boost::replace_all(reasons, L"(", L"");
            boost::replace_all(reasons, L")", L"");

            boost::replace_all(deduction, L"«", L"");
            boost::replace_all(deduction, L"»", L"");
            boost::replace_all(reasons, L"«", L"");
            boost::replace_all(reasons, L"»", L"");

            std::wstring line_for_file;
            if (!reasons.empty())
                line_for_file = reasons + L" ⇒ " + deduction;
            else
                line_for_file = deduction; // Fallback (sehr selten)

            boost::trim(line_for_file);

            std::string utf8_line = string::unicode::to_utf8(line_for_file);

            if (is_wikidata)
            {
                std::string encoded = compressor.encode(utf8_line);
                out << encoded << '\n';
            }
            else
            {
                out << utf8_line << '\n';
            }
        };

        _n->print(L"Starting full inference in encode mode – deduced facts (reversed order, no brackets/markup) will be written to " + cmd[1] + (is_wikidata ? L" (with Wikidata token encoding)." : L" (plain text)."), true);

        _n->set_print(encode_print);

        _n->run(true, false, false);

        _n->set_print(normal_print);

        _n->print(L"Ready.", true);
    }
    else if (cmd[0] == L".decode")
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .decode requires exactly one argument: the input file path");

        std::string   infile = string::unicode::to_utf8(cmd[1]);
        std::ifstream in(infile);
        if (!in.is_open())
            throw std::runtime_error("Command .decode: Cannot open input file '" + infile + "'");

        zelph::WikidataTextCompressor compressor({U' ', U'\t', U'\n', U','});

        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty())
            {
                std::string decoded = compressor.decode(line);
                std::cout << decoded << std::endl;
            }
        }
    }
    else if (cmd[0] == L".load")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .load: Missing bin or json file name");
        if (cmd.size() > 2) throw std::runtime_error("Command .load: Unknown argument after file name");

        std::ofstream log("load.log");
        _n->set_print([&](const std::wstring& str, bool o)
                      {
          log << string::unicode::to_utf8(str) << std::endl;

          if (o)
          {
#ifdef _WIN32
              std::wcout << str << std::endl;
#else
            std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
          } });

        if (cmd.size() == 2)
        {
            network::StopWatch watch;
            watch.start();

            // This detects if it's Wikidata (json/bz2 OR bin with source) or Generic (bin only)
            _data_manager = DataManager::create(_n, cmd[1]);
            _data_manager->load();

            watch.stop();
            _n->print(L" Time needed for loading/importing: " + string::unicode::from_utf8(watch.format()), true);
        }
        else
        {
            throw std::runtime_error("Command .load: You need to specify one argument: the *.bin or *.json file to import");
        }
    }
    else if (cmd[0] == L".wikidata-constraints")
    {
        if (cmd.size() < 3) throw std::runtime_error("Command .wikidata-constraints: Missing json file name or directory name");
        if (cmd.size() > 3) throw std::runtime_error("Command .wikidata-constraints: Unknown argument after directory name");

        network::StopWatch watch;
        watch.start();

        std::string           dir        = string::unicode::to_utf8(cmd[2]);
        std::filesystem::path input_path = cmd[1];

        // Specific Logic: This command strictly requires Wikidata capability.
        // We update the global manager to reflect this load context.
        _data_manager = DataManager::create(_n, input_path);

        // Dynamic cast to check if the factory returned a Wikidata manager
        auto wikidata_mgr = std::dynamic_pointer_cast<Wikidata>(_data_manager);

        if (wikidata_mgr)
        {
            wikidata_mgr->import_all(dir);
        }
        else
        {
            // Fallback: If create() returned Generic (e.g. user pointed to a bin file without source),
            // but user wants constraints. This implies user error (missing source) or misuse.
            // But if user supplied JSON, create() definitely returns Wikidata.
            // If user supplied BIN, create() checks for source. If no source, it returns Generic.
            // If Generic, we can't export constraints.
            throw std::runtime_error("Cannot export constraints: Original Wikidata source file not found or invalid format.");
        }

        _n->print(L" Time needed for exporting constraints: " + std::to_wstring(static_cast<double>(watch.duration()) / 1000) + L"s", true);
    }
    else if (cmd[0] == L".list-rules")
    {
        // Get all nodes that are subjects of a core.Causes relation
        network::adjacency_set rule_nodes = _n->get_rules();
        if (rule_nodes.empty())
        {
            _n->print(L"No rules found.", true);
            return;
        }

        _n->print(L"Listing all rules:", true);
        _n->print(L"------------------------", true);

        for (const auto& rule : rule_nodes)
        {
            std::wstring output;
            // Format the rule for printing
            _n->format_fact(output, _n->lang(), rule, 3);
            _n->print(output, true);
        }
        _n->print(L"------------------------", true);
    }
    else if (cmd[0] == L".list-predicate-usage")
    {
        size_t limit = 0;
        if (cmd.size() > 2) throw std::runtime_error("Command .list-predicate-usage accepts at most one optional argument (max entries)");
        if (cmd.size() == 2)
        {
            try
            {
                size_t pos = 0;
                limit      = std::stoull(cmd[1], &pos);
                if (pos != cmd[1].length() || limit == 0)
                    throw std::runtime_error("Could not parse max entries argument");
            }
            catch (...)
            {
                throw std::runtime_error("Invalid max entries argument");
            }
        }
        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        list_predicate_usage(limit);
        if (_data_manager)
        {
            _data_manager->set_logging(true);
        }
    }
    else if (cmd[0] == L".list-predicate-value-usage")
    {
        if (cmd.size() < 2 || cmd.size() > 3)
            throw std::runtime_error("Command .list-predicate-value-usage requires one required argument (<predicate>) and one optional (max entries)");

        size_t       limit    = 0;
        std::wstring pred_arg = cmd[1];
        if (cmd.size() == 3)
        {
            try
            {
                size_t pos = 0;
                limit      = std::stoull(cmd[2], &pos);
                if (pos != cmd[2].length() || limit == 0)
                    throw std::runtime_error("Could not parse max entries argument");
            }
            catch (...)
            {
                throw std::runtime_error("Invalid max entries argument");
            }
        }
        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        list_predicate_value_usage(pred_arg, limit);
        if (_data_manager)
        {
            _data_manager->set_logging(true);
        }
    }

    else if (cmd[0] == L".remove-rules")
    {
        _n->remove_rules();
        _n->print(L"All rules removed.", true);
    }
    else if (cmd[0] == L".prune-facts" || cmd[0] == L".prune-nodes")
    {
        if (cmd.size() < 2)
            throw std::runtime_error("Command requires a pattern");

        // Reconstruct the pattern string from arguments to feed into the parser
        std::wstring pattern_str;
        for (size_t i = 1; i < cmd.size(); ++i)
        {
            std::wstring token = cmd[i];
            // If it's a variable, keep as is (A). If not, quote it ("is") so PEG treats it as value.
            // Note: Since cmd is already tokenized by escaped_list_separator, quotes were stripped.
            // We re-add them for non-vars to be safe for parse_zelph_to_janet.
            if (is_var(token))
                pattern_str += token + L" ";
            else
                pattern_str += L"\"" + token + L"\" ";
        }

        std::string utf8_pattern = string::unicode::to_utf8(pattern_str);
        std::string janet_code   = parse_zelph_to_janet(utf8_pattern);

        if (janet_code.empty())
            throw std::runtime_error("Could not parse pattern");

        // Clear variables scope before eval to correctly capture new vars in pattern
        _scoped_variables.clear();
        network::Node pattern_fact = evaluate_janet_expression(janet_code);

        if (pattern_fact == 0)
            throw std::runtime_error("Invalid pattern");

        if (cmd[0] == L".prune-facts")
        {
            if (_scoped_variables.empty())
                throw std::runtime_error("Command .prune-facts: pattern must contain at least one variable");

            size_t removed = 0;
            _n->prune_facts(pattern_fact, removed);
            _n->print(L"Pruned " + std::to_wstring(removed) + L" matching facts.", true);
            if (removed > 0) _n->print(L"Consider running .cleanup.", true);
        }
        else // .prune-nodes
        {
            network::Node relation = _n->parse_relation(pattern_fact);
            if (network::Network::is_var(relation))
            {
                throw std::runtime_error("Command .prune-nodes: relation (predicate) must be fixed");
            }

            size_t removed_facts = 0;
            size_t removed_nodes = 0;
            _n->prune_nodes(pattern_fact, removed_facts, removed_nodes);
            _n->print(L"Pruned " + std::to_wstring(removed_facts) + L" matching facts and " + std::to_wstring(removed_nodes) + L" nodes.", true);
            if (removed_facts > 0 || removed_nodes > 0)
            {
                _n->print(L"Consider running .cleanup.", true);
            }
        }
    }
    else if (cmd[0] == L".cleanup")
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .cleanup takes no arguments");

        size_t removed_facts = 0;
        size_t removed_preds = 0;

        _n->print(L"Scanning for unused predicates and zombie facts...", true);

        _n->purge_unused_predicates(removed_facts, removed_preds);

        _n->print(L"Purged " + std::to_wstring(removed_facts) + L" zombie facts.", true);
        _n->print(L"Removed " + std::to_wstring(removed_preds) + L" unused predicates.", true);

        _n->print(L"Cleaning up isolated nodes...", true);

        size_t cleanup_count = 0;
        _n->cleanup_isolated(cleanup_count);
        _n->print(L"Cleanup: removed " + std::to_wstring(cleanup_count) + L" isolated nodes/names.", true);

        _n->print(L"Cleaning up name mappings...", true);
        size_t names_removed = _n->cleanup_names();
        _n->print(L"Removed " + std::to_wstring(names_removed) + L" dangling name entries.", true);
    }
    else if (cmd[0] == L".stat")
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .stat takes no arguments");

        std::clog << "Network Statistics:" << std::endl;
        std::clog << "------------------------" << std::endl;

        std::clog << "Nodes: " << _n->count() << std::endl;

        size_t ram_usage = zelph::platform::get_process_memory_usage();
        if (ram_usage > 0)
        {
            std::clog << "RAM Usage: " << std::fixed << std::setprecision(1)
                      << (static_cast<double>(ram_usage) / (1024 * 1024 * 1024)) << " GiB" << std::endl;
        }

        if (_n->language_count() > 0)
        {
            std::clog << "Name-of-Node Entries by language:" << std::endl;
            for (const std::string& lang : _n->get_languages())
            {
                std::clog << "  " << lang << ": " << _n->get_name_of_node_size(lang) << std::endl;
            }

            std::clog << "Name-of-Node Entries by language:" << std::endl;
            for (const std::string& lang : _n->get_languages())
            {
                std::clog << "  " << lang << ": " << _n->get_node_of_name_size(lang) << std::endl;
            }
        }

        std::clog << "Languages: " << _n->language_count() << std::endl;
        std::clog << "Rules: " << _n->rule_count() << std::endl;

        std::clog << "------------------------" << std::endl;
    }
    else if (cmd[0] == L".save")
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .save requires exactly one argument: the output file (must end with .bin)");

        std::wstring file = cmd[1];
        if (!boost::algorithm::ends_with(file, L".bin"))
            throw std::runtime_error("Command .save: filename must end with '.bin'");

        std::string utf8_file = string::unicode::to_utf8(file);
        _n->save_to_file(utf8_file);
        _n->print(L"Saved network to " + file, true);
    }
    else if (cmd[0] == L".import")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .import: Missing script path");
        std::wstring path = cmd[1];
        if (!boost::algorithm::ends_with(path, L".zph")) throw std::runtime_error("Command .import: Script must end with .zph");
        import_file(path);
    }
    else
    {
        throw std::runtime_error(string::unicode::to_utf8(L"Unknown command " + cmd[0]));
    }
}

void console::Interactive::Impl::list_predicate_usage(size_t limit)
{
    // Map to store predicate node and its usage count
    std::map<network::Node, size_t> predicate_usage_counts;

    // Get all predicates directly: nodes that IsA RelationTypeCategory
    auto predicates = _n->get_sources(_n->core.IsA, _n->core.RelationTypeCategory, true);

    for (const auto& pred : predicates)
    {
        // Get all facts where this node is used as a relation type
        const auto& facts_using_predicate = _n->get_left(pred);
        predicate_usage_counts[pred]      = facts_using_predicate.size();
    }

    // Convert map to vector for sorting
    std::vector<std::pair<network::Node, size_t>> sorted_predicates(predicate_usage_counts.begin(), predicate_usage_counts.end());

    // Sort the predicates by usage count in ascending order
    std::sort(sorted_predicates.begin(), sorted_predicates.end(), [](const auto& a, const auto& b)
              {
                  return a.second < b.second; // Sort by count, ascending
              });

    // Determine if wikidata language is available for three-column output
    bool has_wikidata_lang = _n->has_language("wikidata");

    _n->print(L"Predicate Usage:", true);
    _n->print(L"------------------------", true);

    size_t total           = sorted_predicates.size();
    size_t entries_to_show = limit ? std::min(limit, total) : total;
    size_t start_idx       = (limit && limit < total) ? total - entries_to_show : 0;

    for (size_t i = start_idx; i < total; ++i)
    {
        const auto&  entry          = sorted_predicates[i];
        std::wstring predicate_name = _n->get_name(entry.first, "", true); // Current language, with fallback
        std::wstring line_output;

        if (has_wikidata_lang && _n->get_lang() != "wikidata")
        {
            // Three columns: current lang name \t wikidata name \t count
            // For the first column, `lang` is an empty string to use the current language.
            // For the second column (wikidata name), `lang` is "wikidata" and `fallback` is `false`.
            std::wstring wikidata_name = _n->get_name(entry.first, "wikidata", false);
            line_output                = predicate_name + L"\t" + wikidata_name + L"\t" + std::to_wstring(entry.second);
        }
        else
        {
            // Two columns: current lang name \t count
            // `lang` is an empty string to use the current language, `fallback` is `true`.
            line_output = predicate_name + L"\t" + std::to_wstring(entry.second);
        }
        _n->print(line_output, true);
    }
    _n->print(L"------------------------", true);
    if (limit && limit < total)
        _n->print(L"Showing top " + std::to_wstring(limit) + L" of " + std::to_wstring(total) + L" predicates.", true);
}

void console::Interactive::Impl::list_predicate_value_usage(const std::wstring& pred_arg, size_t limit /*= 0*/)
{
    // Resolve the predicate node (accept name in current language or raw numeric ID)
    network::Node pred = _n->get_node(pred_arg);
    if (pred == 0)
    {
        try
        {
            size_t pos = 0;
            pred       = std::stoull(pred_arg, &pos);
            if (pos != pred_arg.length())
                throw std::runtime_error("");
        }
        catch (...)
        {
            throw std::runtime_error("Unknown predicate '" + string::unicode::to_utf8(pred_arg) + "' in current language '" + _n->lang() + "'");
        }
    }

    std::wstring pred_display = _n->get_name(pred, _n->lang(), true);
    if (pred_display.empty())
        pred_display = pred_arg;

    _n->print(L"Value Usage for predicate " + pred_display + L":", true);
    _n->print(L"------------------------", true);

    ankerl::unordered_dense::map<network::Node, size_t> value_counts;

    // All fact nodes that use this predicate: fact --> pred
    network::adjacency_set facts = _n->get_left(pred);

    for (network::Node fact : facts)
    {
        network::adjacency_set incoming = _n->get_left(fact); // subject(s) + object(s) --> fact

        // Objects are incoming nodes where fact does NOT point back to them
        for (network::Node cand : incoming)
        {
            if (!_n->has_right_edge(fact, cand))
            {
                value_counts[cand]++;
            }
        }
    }

    // Sort by count ascending
    std::vector<std::pair<size_t, network::Node>> sorted;
    sorted.reserve(value_counts.size());
    for (const auto& p : value_counts)
    {
        sorted.emplace_back(p.second, p.first);
    }
    std::sort(sorted.begin(), sorted.end());

    bool        has_wikidata_lang = _n->has_language("wikidata");
    std::string curr_lang         = _n->get_lang();

    size_t total           = sorted.size();
    size_t entries_to_show = limit ? std::min(limit, total) : total;
    size_t start_idx       = (limit && limit < total) ? total - entries_to_show : 0;

    for (size_t i = start_idx; i < total; ++i)
    {
        const auto&  entry      = sorted[i];
        std::wstring value_name = _n->get_name(entry.second, "", true); // current language with fallback

        std::wstring line;
        if (has_wikidata_lang && curr_lang != "wikidata")
        {
            std::wstring wikidata_name = _n->get_name(entry.second, "wikidata", false);
            if (wikidata_name.empty())
                wikidata_name = L"(no ID)";
            line = value_name + L"\t" + wikidata_name + L"\t" + std::to_wstring(entry.first);
        }
        else
        {
            line = value_name + L"\t" + std::to_wstring(entry.first);
        }
        _n->print(line, true);
    }

    _n->print(L"------------------------", true);
    _n->print(L"Total unique values: " + std::to_wstring(total), true);
    if (limit && limit < total)
        _n->print(L"Showing top " + std::to_wstring(limit) + L" of " + std::to_wstring(total) + L" values.", true);
    if (total == 0)
    {
        _n->print(L"(No values found for this predicate)", true);
    }
}

network::Node console::Interactive::Impl::resolve_node(const std::wstring& arg, std::string lang) const
{
    network::Node nd = _n->get_node(arg, lang);
    if (nd == 0)
    {
        try
        {
            size_t pos = 0;
            nd         = std::stoull(arg, &pos);
            if (pos != arg.length())
            {
                nd = 0; // arg is no unsigned integer number => we interpret it as a string and return 0, denoting there is no node with this name in lang
            }
            else if (!_n->exists(nd))
            {
                throw std::runtime_error("Node does not exist"); // arg is a number => throw, because node numbers must only be created by class Network
            }
        }
        catch (...)
        {
            nd = 0; // arg is no number => return 0, denoting there is no node with this name in lang
        }
    }
    return nd;
}

network::Node console::Interactive::Impl::resolve_single_node(const std::wstring& arg, bool prioritize_id) const
{
    // prioritize_id = true für destructive commands (.delname, .remove)

    bool is_numeric = true;
    for (wchar_t c : arg)
    {
        if (!std::iswdigit(c))
        {
            is_numeric = false;
            break;
        }
    }

    if (is_numeric && prioritize_id)
    {
        try
        {
            size_t        pos = 0;
            network::Node nd  = std::stoull(arg, &pos);
            if (pos == arg.length() && _n->exists(nd))
            {
                return nd;
            }
        }
        catch (...)
        {
        }
    }

    network::Node nd = _n->get_node(arg);
    if (nd != 0)
    {
        return nd;
    }

    if (is_numeric && !prioritize_id)
    {
        try
        {
            size_t        pos   = 0;
            network::Node nd_id = std::stoull(arg, &pos);
            if (pos == arg.length() && _n->exists(nd_id))
            {
                return nd_id;
            }
        }
        catch (...)
        {
        }
    }

    throw std::runtime_error("Unknown node '" + string::unicode::to_utf8(arg) + "'");
}

std::vector<network::Node> console::Interactive::Impl::resolve_nodes_by_name(const std::wstring& name) const
{
    return _n->resolve_nodes_by_name(name);
}

void console::Interactive::Impl::generate_and_print_mermaid_link(network::Node nd, int depth, int max_neighbors) const
{
    std::filesystem::path temp_dir  = std::filesystem::temp_directory_path();
    std::wstring          hex_name  = string::unicode::from_utf8(_n->get_name_hex(nd, false, max_neighbors));
    std::wstring          safe_name = string::sanitize_filename(hex_name);
    std::filesystem::path html_path = temp_dir / (safe_name + L".html");

    _n->gen_mermaid_html(nd, html_path.string(), depth, max_neighbors);

    std::string abs_path = html_path.string();
    std::string file_url = "file://" + abs_path;

    const std::string OSC_START = "\033]8;;";
    const char        OSC_SEP   = '\a';
    const std::string OSC_END   = "\033]8;;\a";

    std::clog << "  Mermaid HTML: " << OSC_START << file_url << OSC_SEP << file_url << OSC_END << std::endl;
}

void console::Interactive::run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition) const
{
    _pImpl->_n->run(print_deductions, generate_markdown, suppress_repetition);
}

std::string console::Interactive::get_lang() const
{
    return _pImpl->_n->get_lang();
}

bool console::Interactive::Impl::is_var(std::wstring token)
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

void console::Interactive::Impl::process_janet_setup(const std::string& code) const
{
    Janet out;
    int   status = janet_dostring(_janet_env, code.c_str(), "setup", &out);
    if (status != JANET_SIGNAL_OK) janet_stacktrace(NULL, out);
}

#ifdef PROVIDE_C_INTERFACE
console::Interactive interactive;

extern "C" void zelph_process_c(const char* line, size_t len)
{
    if (len > 0)
    {
        std::string l(line, 0, len);
        interactive.process(string::unicode::from_utf8(l));
    }
}

extern "C" void zelph_run()
{
    interactive.run(true, false, false);
}
#endif
