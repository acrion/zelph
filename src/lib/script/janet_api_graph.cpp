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

#include "network/fact_structure.hpp"
#include "network/reasoning.hpp"

#include <janet.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace zelph
{
    // Check whether a fact exists in the graph without creating it.
    // Returns true if the fact (subject predicate object...) is known.
    // Shared by zelph/exists and zelph/mentioned. The two ask different
    // questions about the same node: whether the statement was CLAIMED --
    // asserted or derived -- and whether the node is present at all, which a
    // rule's ground pattern is without anybody having claimed it.
    Janet ScriptEngine::Impl::fact_probe(const char* name, const bool asserted_only, int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_boolean(0);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        // A trailing `:exact` demands the fact precisely as provided,
        // excluding the expansion beneath: a single hash probe, also on a
        // miss, whereas the expansion traverses every relation the subject
        // is involved in -- at a hub within a large graph, encompassing
        // hundreds of thousands of entries. A keyword as the final argument
        // serves as an option, never as a node: `:exact` is the sole
        // permitted one, and any alternative keyword is refused rather than
        // read as an object no one can name, which returned false without
        // comment.
        bool exact = false;
        if (janet_checktype(argv[argc - 1], JANET_KEYWORD))
        {
            if (janet_cstrcmp(janet_unwrap_keyword(argv[argc - 1]), "exact") != 0)
                janet_panicf("%s: %v is no option; the only option is a trailing :exact", name, argv[argc - 1]);
            if (argc < 4) janet_panicf("%s: :exact follows the subject, the predicate and at least one object", name);
            exact = true;
        }
        const int32_t parts = exact ? argc - 1 : argc;

        const std::string lang = s_instance->_n->published_lang();
        network::Node     s    = s_instance->resolve_janet_arg_no_create(argv[0], lang);
        network::Node     p    = s_instance->resolve_janet_arg_no_create(argv[1], lang);
        if (!s || !p)
        {
            Janet res = janet_wrap_boolean(0);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        for (int32_t i = 2; i < parts; ++i)
        {
            network::Node o = s_instance->resolve_janet_arg_no_create(argv[i], lang);
            if (!o)
            {
                Janet res = janet_wrap_boolean(0);
                if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, false, res);
                return res;
            }
            objs.insert(o);
        }

        const network::Answer ans   = s_instance->_n->check_fact(s, p, objs);
        const network::Node   node  = ans.relation();
        bool                  known = ans.is_known();

        // The engine's bookkeeping is not there for zelph/exists, just as
        // it is not there for a ground rule condition: a record of a
        // contradiction was answered as true, yet
        // `(<record> ~ refuted) => ...` failed to fire.
        const network::Zelph* const z = s_instance->_n;
        if (known && asserted_only)
            known = z->is_asserted_fact(node) && !((p == z->core.IsA || p == z->core.PartOf) && z->is_bookkeeping(node));

        if (!known && !exact)
        {
            // A fact carrying FURTHER objects satisfies this one: `a p b`
            // holds when the graph says `a p b c`. That is what unification
            // matches, what a rule with exactly this condition fires on, and
            // what `¬` refuses to succeed against -- only the exact hash
            // could not see it. The SPARQL layer asks its ground triples
            // through here, so it answered "no" to a triple its own
            // variable patterns answer "yes" to.
            if (asserted_only)
            {
                // The question is the one a ground rule condition asks, and
                // the matcher of conditions provides the response: a statement
                // within the arguments is stated by a fact that includes
                // additional objects, as the rule `((a p c) bad k) => (x ok y)`
                // fires alongside `(a p b c) bad k`, and a claimed broader
                // fact counts also where the exact node exists only as part
                // of a rule's pattern. Only a fact containing more than one
                // object can make the answer differ from the exact one
                // (Zelph::may_hold_wider_statements).
                known = z->may_hold_wider_statements() && network::ground_condition_matches(s_instance->_n, s, p, objs);
            }
            else if (network::containing_fact(s_instance->_n, s, p, objs) != 0)
            {
                known = true;
            }
        }

        Janet res = janet_wrap_boolean(known ? 1 : 0);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, false, res);
        return res;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_exists(int32_t argc, Janet* argv)
    {
        return fact_probe("zelph/exists", true, argc, argv);
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_mentioned(int32_t argc, Janet* argv)
    {
        return fact_probe("zelph/mentioned", false, argc, argv);
    }

    // Return the name of a node as a string, or nil if unnamed.
    // Optional second argument specifies the language (defaults to current).
    //
    // When a name in that language is unavailable, the response defaults
    // to a name in a different language, which is right for showing a
    // node but inappropriate for asking whether the language names it: a
    // node named solely in `wikidata` answered with its identifier when
    // queried about its English name. A trailing :strict asks the second
    // question.
    Janet ScriptEngine::Impl::janet_cfun_zelph_name(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 3);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/name", argc, argv, true);

        bool strict = false;
        if (argc >= 2 && janet_checktype(argv[argc - 1], JANET_KEYWORD))
        {
            if (janet_cstrcmp(janet_unwrap_keyword(argv[argc - 1]), "strict") != 0)
                janet_panicf("zelph/name: %v is no option; the only option is a trailing :strict", argv[argc - 1]);
            strict = true;
        }
        else if (argc == 3)
        {
            janet_panicf("zelph/name: the third argument can only be :strict, got %v", argv[2]);
        }

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/name", argc, argv, false, res);
            return res;
        }

        std::string lang = s_instance->_n->published_lang();
        if (argc >= 2 && janet_checktype(argv[1], JANET_STRING))
        {
            lang = reinterpret_cast<const char*>(janet_unwrap_string(argv[1]));
        }

        std::string name = s_instance->_n->get_name(n, lang, !strict);
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

    // The node associated with a name, or nil: the zelph/resolve operation
    // without the creation, ensuring that asking whether a term refers to
    // an entity preserves the graph's state. A variable holding a name is
    // not the node of that name, just as in Zelph::node, and a core
    // spelling names the core node across every language. It only reads,
    // allowing any thread to inquire.
    Janet ScriptEngine::Impl::janet_cfun_zelph_lookup(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/lookup", argc, argv, true);

        const uint8_t* const name_arg = janet_getstring(argv, 0);
        const uint8_t* const lang_arg = janet_optstring(argv, argc, 1, nullptr);

        const std::string   lang = lang_arg ? reinterpret_cast<const char*>(lang_arg) : s_instance->_n->published_lang();
        const network::Node n    = s_instance->_n->get_node(reinterpret_cast<const char*>(name_arg), lang, true);

        Janet res = n == 0 ? janet_wrap_nil() : zelph_wrap_node(n);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/lookup", argc, argv, false, res);
        return res;
    }

    // Each node bearing a name in a specific language (Zelph::nodes_named),
    // arranged in the order in which the name map elects its holder. When
    // multiple nodes share a label, it resolves to a single one, and a
    // front end accessing the graph via labels must perceive the remaining
    // options to make a selection. :incomplete rather than a part of the
    // answer while the name sections were loaded partially.
    Janet ScriptEngine::Impl::janet_cfun_zelph_lookup_all(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_tuple(janet_tuple_n(nullptr, 0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/lookup-all", argc, argv, true);

        const uint8_t* const name_arg = janet_getstring(argv, 0);
        const uint8_t* const lang_arg = janet_optstring(argv, argc, 1, nullptr);

        Janet res;
        {
            const auto carriers = s_instance->_n->nodes_named(reinterpret_cast<const char*>(name_arg), lang_arg ? reinterpret_cast<const char*>(lang_arg) : s_instance->_n->published_lang());
            if (!carriers)
            {
                res = janet_ckeywordv("incomplete");
            }
            else
            {
                Janet* const nodes = janet_tuple_begin(static_cast<int32_t>(carriers->size()));
                for (std::size_t i = 0; i < carriers->size(); ++i)
                    nodes[i] = zelph_wrap_node((*carriers)[i]);
                res = janet_wrap_tuple(janet_tuple_end(nodes));
            }
        }
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/lookup-all", argc, argv, false, res);
        return res;
    }

    // The language in which names are accessed and modified, which `.lang`
    // both retrieves and establishes. When provided with an argument, it
    // sets the language and returns the prior value, enabling a caller to
    // operate within a distinct language context and subsequently restore
    // the session's language. Exclusive to the main thread: the language
    // constitutes part of the engine's state, determining the resolution of
    // every name on the main thread.
    Janet ScriptEngine::Impl::janet_cfun_zelph_lang(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 0, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/lang", argc, argv, true);

        require_main_thread("zelph/lang");

        const uint8_t* const next = argc == 1 ? janet_getstring(argv, 0) : nullptr;
        if (next != nullptr && next[0] == 0) janet_panicf("zelph/lang: the language must not be empty");

        Janet res = janet_cstringv(s_instance->_n->lang().c_str());
        if (next != nullptr) s_instance->_n->set_lang(reinterpret_cast<const char*>(next));

        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/lang", argc, argv, false, res);
        return res;
    }

    // Zelph::content_epoch: a value that shifts whenever the significance of
    // node ids or labels stored in a Janet cache could have become outdated
    // -- due to a new engine, a load that replaced the graph, or a dump
    // imported into it. Any thread is allowed to read it.
    Janet ScriptEngine::Impl::janet_cfun_zelph_epoch(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/epoch", argc, argv, true);

        Janet res = janet_wrap_number(static_cast<double>(network::Zelph::content_epoch()));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/epoch", argc, argv, false, res);
        return res;
    }

    // The number of edges connected to a node is determined by the size of
    // its adjacency, not by examining the elements within it: the expense
    // remains constant regardless of the node, whereas zelph/sources
    // duplicates an entire adjacency set. :in tallies incoming edges, :out
    // counts outgoing ones, and omitting a side includes both. Any thread
    // is permitted to ask.
    Janet ScriptEngine::Impl::janet_cfun_zelph_degree(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_number(0);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/degree", argc, argv, true);

        bool                 incoming = true;
        bool                 outgoing = true;
        const uint8_t* const side     = janet_optkeyword(argv, argc, 1, nullptr);
        if (side != nullptr)
        {
            if (janet_cstrcmp(side, "in") == 0)
                outgoing = false;
            else if (janet_cstrcmp(side, "out") == 0)
                incoming = false;
            else
                janet_panicf("zelph/degree: the side is :in or :out, got %v", argv[1]);
        }

        const network::Node n      = s_instance->resolve_janet_arg_no_create(argv[0]);
        std::size_t         degree = 0;
        if (n != 0)
        {
            if (incoming) degree += s_instance->_n->left_count(n);
            if (outgoing) degree += s_instance->_n->right_count(n);
        }

        Janet res = janet_wrap_number(static_cast<double>(degree));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/degree", argc, argv, false, res);
        return res;
    }

    // Node annotations (Zelph::annotate): textual data that a front end
    // stores alongside a node, like the sentence responsible for asserting a
    // fact, and that no query or rule sees. The node refers to
    // a node value, an identifier, or a name, which is retrieved through
    // lookup and never created. Writes are permitted on the main thread
    // only; reading is permitted on any thread.
    namespace
    {
        std::string janet_bytes(const uint8_t* const s)
        {
            return {reinterpret_cast<const char*>(s), static_cast<std::size_t>(janet_string_length(s))};
        }

        Janet janet_strings(const std::vector<std::string>& values)
        {
            Janet* const tuple = janet_tuple_begin(static_cast<int32_t>(values.size()));
            for (std::size_t i = 0; i < values.size(); ++i)
                tuple[i] = janet_stringv(reinterpret_cast<const uint8_t*>(values[i].data()), static_cast<int32_t>(values[i].size()));
            return janet_wrap_tuple(janet_tuple_end(tuple));
        }
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_annotate(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 3);
        if (!s_instance) return janet_wrap_false();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/annotate", argc, argv, true);

        require_main_thread("zelph/annotate");

        const uint8_t* const key   = janet_getstring(argv, 1);
        const uint8_t* const value = janet_getstring(argv, 2);
        const network::Node  n     = s_instance->resolve_janet_arg_no_create(argv[0]);
        if (n == 0 || !s_instance->_n->exists(n)) janet_panicf("zelph/annotate: %v names no node of the graph", argv[0]);

        Janet res = janet_wrap_boolean(s_instance->_n->annotate(n, janet_bytes(key), janet_bytes(value)));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/annotate", argc, argv, false, res);
        return res;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_annotations(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/annotations", argc, argv, true);

        const uint8_t* const key = janet_optstring(argv, argc, 1, nullptr);
        const network::Node  n   = s_instance->resolve_janet_arg_no_create(argv[0]);

        Janet res;
        {
            const network::Zelph::node_annotations annotations = n == 0 ? network::Zelph::node_annotations{} : s_instance->_n->annotations(n);
            if (key != nullptr)
            {
                const auto it = annotations.find(std::string_view(reinterpret_cast<const char*>(key), static_cast<std::size_t>(janet_string_length(key))));
                res           = janet_strings(it == annotations.end() ? std::vector<std::string>{} : it->second);
            }
            else
            {
                JanetKV* const table = janet_struct_begin(static_cast<int32_t>(annotations.size()));
                for (const auto& [k, values] : annotations)
                    janet_struct_put(table, janet_stringv(reinterpret_cast<const uint8_t*>(k.data()), static_cast<int32_t>(k.size())), janet_strings(values));
                res = janet_wrap_struct(janet_struct_end(table));
            }
        }
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/annotations", argc, argv, false, res);
        return res;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_unannotate(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_number(0);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/unannotate", argc, argv, true);

        require_main_thread("zelph/unannotate");

        const uint8_t* const key   = janet_getstring(argv, 1);
        const uint8_t* const value = janet_optstring(argv, argc, 2, nullptr);
        const network::Node  n     = s_instance->resolve_janet_arg_no_create(argv[0]);

        std::size_t removed = 0;
        if (n != 0)
        {
            const std::string one = value != nullptr ? janet_bytes(value) : std::string{};
            removed               = s_instance->_n->unannotate(n, janet_bytes(key), value != nullptr ? &one : nullptr);
        }

        Janet res = janet_wrap_number(static_cast<double>(removed));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/unannotate", argc, argv, false, res);
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_sources(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_array(janet_array(0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/sources", argc, argv, true);

        const std::string lang      = s_instance->_n->published_lang();
        network::Node     predicate = s_instance->resolve_janet_arg_no_create(argv[0], lang);
        network::Node     target    = s_instance->resolve_janet_arg_no_create(argv[1], lang);
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_targets(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_array(janet_array(0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/targets", argc, argv, true);

        const std::string lang      = s_instance->_n->published_lang();
        network::Node     subject   = s_instance->resolve_janet_arg_no_create(argv[0], lang);
        network::Node     predicate = s_instance->resolve_janet_arg_no_create(argv[1], lang);
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
    Janet ScriptEngine::Impl::closure_impl(int32_t argc, Janet* argv, const char* name, bool forward)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_array(janet_array(0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        const std::string lang      = s_instance->_n->published_lang();
        network::Node     anchor    = s_instance->resolve_janet_arg_no_create(argv[0], lang);
        network::Node     predicate = s_instance->resolve_janet_arg_no_create(argv[1], lang);
        bool              include   = argc >= 3 && janet_truthy(argv[2]);

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

    Janet ScriptEngine::Impl::janet_cfun_zelph_closure(int32_t argc, Janet* argv)
    {
        return closure_impl(argc, argv, "zelph/closure", true);
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_closure_sources(int32_t argc, Janet* argv)
    {
        return closure_impl(argc, argv, "zelph/closure-sources", false);
    }

    // Tag a fact pattern as a neural condition and return the TAG FACT
    // (pattern nn <net>) -- not the pattern. The tag fact itself becomes
    // the rule condition, structurally analogous to a != guard, so the
    // pattern can additionally appear as an ordinary (binding) condition
    // in the same rule without the two collapsing into one node.
    Janet ScriptEngine::Impl::janet_cfun_zelph_approx(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/approx");
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_path(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/path");
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
    // The determination of whether the ends are variables occurs when the
    // tokens are RESOLVED, and the parser lacks visibility into all of it: an
    // `,unquote` or an end that an island returns remains opaque to it. It
    // reads a line containing a variable as a query, where a ground path is a
    // test and gets no guard; in a line it reads as one that asserts, the
    // guard is emitted. It cannot live in zelph/path either: by that stage the
    // operand FACT has already been built, which is the half that causes harm.
    // Thus, the sugar emits this guard prior to construction, with both ends
    // bound exactly once.
    //
    // A Janet block is exempt: there the caller is using the API directly and
    // may well be assembling a condition for zelph/rule by hand.
    //
    // Main thread only: the outcome hinges on the statement currently
    // being evaluated by the main thread (_in_janet_block,
    // _building_rule), and the path condition it guards cannot be built
    // on any other thread anyway.
    Janet ScriptEngine::Impl::janet_cfun_zelph_path_guard(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/path-guard");

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
                     "the path as a condition -- in a rule, or in a query (a line with a variable).");
    }

    // Emit a line through zelph's output handler. Janet's own print writes to
    // raw stdout and bypasses the OutputHandler (REPL redirection, playground,
    // test collectors); this is the pipeline-correct way for scripts to talk
    // to the user -- e.g. import-time notices, which the input-echo
    // suppression inside imports deliberately does not cover.
    Janet ScriptEngine::Impl::janet_cfun_zelph_out(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        const uint8_t* str = janet_getstring(argv, 0);
        s_instance->_n->out(reinterpret_cast<const char*>(str), true);
        return janet_wrap_nil();
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_var(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 0, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/var");
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_resolve(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/resolve", argc, argv, true);

        const uint8_t* str  = janet_getstring(argv, 0);
        std::string    wstr = reinterpret_cast<const char*>(str);

        std::string lang = s_instance->_n->published_lang();
        if (argc >= 2 && janet_checktype(argv[1], JANET_STRING))
        {
            lang = reinterpret_cast<const char*>(janet_unwrap_string(argv[1]));
        }

        // On any thread other than the main thread, a name is simply looked up:
        // the node it already holds delivers the answer (Zelph::get_node with
        // atoms_only), whereas a name requiring a new node is rejected, as
        // creating it constitutes a write operation. The empty name gets the
        // same error as the main thread. A program evaluating a compiled network
        // on a worker thread resolves its input names in this way.
        if (std::this_thread::get_id() != s_instance->_main_thread_id)
        {
            network::Zelph::check_node_name(wstr);
            const network::Node existing = s_instance->_n->get_node(wstr, lang, true);
            if (existing == 0)
                throw std::runtime_error("zelph/resolve: '" + wstr + "' has no node, and creating one is main thread only.");

            Janet res = zelph_wrap_node(existing);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/resolve", argc, argv, false, res);
            return res;
        }

        network::Node n   = s_instance->_n->node(wstr, lang);
        Janet         res = zelph_wrap_node(n);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/resolve", argc, argv, false, res);
        return res;
    }
}
