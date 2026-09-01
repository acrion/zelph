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

#include <string>

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
    Janet ScriptEngine::Impl::janet_cfun_zelph_name(int32_t argc, Janet* argv)
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_sources(int32_t argc, Janet* argv)
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_targets(int32_t argc, Janet* argv)
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
    Janet ScriptEngine::Impl::closure_impl(int32_t argc, Janet* argv, const char* name, bool forward)
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_path_guard(int32_t argc, Janet* argv)
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
}
