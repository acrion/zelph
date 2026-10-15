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
#include "network/rule_identity.hpp"
#include "string/string_utils.hpp"

#include <janet.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    // The cluster in which a rule is constructed. A cluster records precisely
    // the nodes that were CREATED during its active phase, which is what
    // tells a rule's own parts from the nodes it reuses.
    const char* const kRuleScratch = "__rule";

    // The cluster where a query carrying a condition operator runs
    // (zelph/query-text): the records it captures are those the statement
    // created.
    const char* const kQueryScratch = "__query";

    // The cluster in which a claimed statement that carries a refutation runs
    // (zelph/claim-text), so that a refused one keeps nothing.
    const char* const kClaimScratch = "__claim";

    // Whether a thunk concluded in a suspended state instead of
    // returning a result or encountering an error.
    bool suspended(const JanetSignal signal)
    {
        return signal != JANET_SIGNAL_OK && signal != JANET_SIGNAL_ERROR;
    }

    // A thunk that writes a rule executes within a C call, within the confines
    // of the template scope. A thunk that awaits the event loop -- ev/sleep, a
    // channel, a stream -- or yields becomes suspended at that point, and a C
    // call cannot be resumed: the rule would vanish without a word. Nor could
    // the scope remain open during the wait, as every other fibre of the
    // thread would otherwise write rule text in the interim. Thus, the rule is
    // refused.
    [[noreturn]] void refuse_suspended(const std::string& message)
    {
#ifdef JANET_EV
        // The wait is registered for the root fibre: a newly assigned
        // schedule id voids the wake-up, which would resume that fibre at
        // whatever it does next, or, once the refusal has ended it, report a
        // second error.
        if (JanetFiber* const root = janet_root_fiber()) ++root->sched_id;
#endif
        throw std::runtime_error(message);
    }

    [[noreturn]] void refuse_suspended_rule(const char* const caller)
    {
        refuse_suspended(std::string(caller) + ": the forms that write a rule cannot wait for the event loop or yield. "
                                               "Compute what they wait for before the rule, and write the rule from the result.");
    }

    // A value rooted for the lifetime of this
    // object's existence.
    struct Rooted
    {
        explicit Rooted(const Janet v)
            : value(v)
        {
            janet_gcroot(value);
        }
        ~Rooted() { janet_gcunroot(value); }
        Rooted(const Rooted&)                = delete;
        Rooted&     operator=(const Rooted&) = delete;
        const Janet value;
    };

    // What the facts a focus wrote beside a rule require from what the
    // build created: the facts and all that they hold, the declarations of
    // their new predicates, and the memberships and tags of a new
    // collection they hold. When the rule turns out to duplicate one the
    // graph has, the build's scratch is dropped, and these remain outside
    // the drop (zelph/dedup-rule).
    std::vector<zelph::network::Node> needed_by_aside(const zelph::network::Zelph* const z, const std::vector<zelph::network::Node>& facts, const std::vector<zelph::network::Node>& created)
    {
        using zelph::network::Node;
        using zelph::network::Zelph;

        const std::unordered_set<Node> fresh(created.begin(), created.end());
        std::unordered_set<Node>       kept;
        std::vector<Node>              pending;
        const auto                     reach = [&](const Node n)
        {
            if (n != 0 && fresh.count(n) != 0 && kept.insert(n).second) pending.push_back(n);
        };
        for (const Node f : facts)
            reach(f);
        while (!pending.empty())
        {
            const Node n = pending.back();
            pending.pop_back();
            if (Zelph::is_var(n)) continue;

            if (const Node predicate = Zelph::is_hash(n) ? z->predicate_of(n) : Node{0}; predicate != 0)
            {
                zelph::network::adjacency_set objects;
                reach(z->parse_fact(n, objects));
                reach(predicate);
                for (const Node o : objects)
                    reach(o);
                reach(Zelph::create_hash(z->core.IsA, predicate, {z->core.RelationTypeCategory}));
            }
            else if (!z->is_named_any(n))
            {
                // A new collection arrives accompanied by its memberships
                // and its tags, the facts that constitute it.
                for (const Node f : z->get_right(n))
                {
                    if (fresh.count(f) == 0) continue;
                    zelph::network::adjacency_set objects;
                    const Node                    subject   = z->parse_fact(f, objects);
                    const Node                    predicate = z->predicate_of(f);
                    if ((predicate == z->core.PartOf && objects.count(n) == 1) || (predicate == z->core.IsA && subject == n)) reach(f);
                }
            }
        }
        return {kept.begin(), kept.end()};
    }

    // Whether a `=>` fact asks which implications exist rather than
    // declaring a rule: its condition is a bare variable (`S => O`), or it
    // holds a variable and none of its consequences can be asserted
    // (`(X p Y) => O`, `(X p Y) => k`, `(X p Y) => {(X q Y)}`). In neither
    // case does it constitute a rule. A `=>` whose consequence can be
    // asserted builds what it says, whether or not it qualifies as a rule:
    // `(*{(A w B) (B w C)} mypred whatever) => (A w C)` writes its fact
    // beside the set.
    bool asks_implications(const zelph::network::Zelph* const z, const zelph::network::Node fact)
    {
        using zelph::network::Node;
        using zelph::network::Zelph;

        if (fact == 0 || z->predicate_of(fact) != z->core.Causes) return false;
        zelph::network::adjacency_set consequences;
        const Node                    condition = z->parse_fact(fact, consequences);
        if (Zelph::is_var(condition)) return true;

        // The predicate tells a statement apart from a container, a name,
        // and a variable, as the genuine structure records it, and reads no
        // structure cache, because this runs for each typed rule.
        const auto assertable = [z](const Node c)
        { return c == z->core.Contradiction || z->predicate_of(c) != 0; };
        return std::none_of(consequences.begin(), consequences.end(), assertable) && z->var_in_closure(fact);
    }
}

namespace zelph
{
    // Extract the car (first element / subject) of a cons cell.
    // Returns nil if the argument is nil or not a valid cons cell.
    Janet ScriptEngine::Impl::janet_cfun_zelph_car(int32_t argc, Janet* argv)
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_cdr(int32_t argc, Janet* argv)
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

    // The parts of a fact node: [subject predicate objects], where objects
    // form a tuple arranged by their node identifiers, ensuring each fact
    // produces exactly one tuple. zelph/car and zelph/cdr accept solely
    // cons cells, while zelph/sources and zelph/targets never return the
    // parts of a fact, meaning a front end could not reconstruct a fact it
    // received -- be it a rule's consequence, a fact bound by a query. nil
    // is returned whenever something is not a fact. Inspired by
    // zelph_fact_parts from the C API.
    Janet ScriptEngine::Impl::janet_cfun_zelph_parse(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/parse", argc, argv, true);

        Janet               res  = janet_wrap_nil();
        const network::Node fact = zelph_unwrap_node(argv[0]);
        if (fact != 0 && !network::Zelph::is_var(fact) && s_instance->_n->exists(fact))
        {
            if (const network::Node predicate = s_instance->_n->parse_relation(fact); predicate != 0)
            {
                network::adjacency_set objects;
                if (const network::Node subject = s_instance->_n->parse_fact(fact, objects, 0); subject != 0)
                {
                    std::vector<network::Node> sorted(objects.begin(), objects.end());
                    std::sort(sorted.begin(), sorted.end());

                    Janet* const parts = janet_tuple_begin(static_cast<int32_t>(sorted.size()));
                    for (std::size_t i = 0; i < sorted.size(); ++i)
                        parts[i] = zelph_wrap_node(sorted[i]);
                    const Janet triple[3] = {zelph_wrap_node(subject), zelph_wrap_node(predicate), janet_wrap_tuple(janet_tuple_end(parts))};
                    res                   = janet_wrap_tuple(janet_tuple_n(triple, 3));
                }
            }
        }

        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/parse", argc, argv, false, res);
        return res;
    }

    // `¬(pattern)`: the fact ((pattern) ~ negation), which serves as the
    // negated condition a rule names (Zelph::negate). The Janet equivalent
    // of ¬(pattern) and ((pattern) ~ negation) in zelph syntax.
    Janet ScriptEngine::Impl::janet_cfun_zelph_negate(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/negate");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, true);

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, false, res);
            return res;
        }

        Janet res = zelph_wrap_node(s_instance->_n->negate(n));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, false, res);
        return res;
    }

    // The rule currently present in the graph that `rule` duplicates, or
    // 0, retrieved via the fingerprint index
    // (Zelph::find_equivalent_rule).
    //
    // A rule in force takes precedence over one merely mentioned in a
    // statement, such as the template associated with a switch next to the
    // copy the switch claimed: the text names both, yet only the claimed one
    // is the rule that fires. A mention is not at all equivalent to a rule
    // being written -- removing the new rule as its duplicate would result in
    // no rule being in force, and a rule typed after a switch that mentions
    // it would never fire. Only when resolving a pattern (via .explain, the
    // .prune commands) does the mention get named, since naming it is the
    // purpose in that context.
    network::Node ScriptEngine::Impl::find_duplicate_rule(const network::Node rule)
    {
        return _n->find_equivalent_rule(rule, _resolving_pattern);
    }

    // A rule built while another one is being built would activate the same
    // scratch cluster, merge it into itself upon completion, and forfeit
    // what the outer build recorded, so the outer rule's parts would never
    // be marked. `zelph/rule*` initiates no build, but when called within
    // that context, it would construct a rule independently, in force by
    // itself rather than as a part of the outer rule. A rule nested in another
    // is part of the outer rule's text, which the outer build writes.
    void ScriptEngine::Impl::refuse_nested_rule(const char* const caller) const
    {
        if (_building_rule)
            janet_panicf("%s: a rule cannot be built while another rule is being built. "
                         "A rule nested in another is a part of the outer rule: write it there as a `=>` fact, "
                         "(zelph/fact condition \"=>\" consequence), or build it before.",
                         caller);
    }

    // The part of a rule build shared among zelph/dedup-rule,
    // zelph/build-rule, the zelph/rule macro, and zelph/rule-text: execute
    // the thunk tasked with writing the rule into the scratch cluster, then
    // make the cluster that was active before -- returned via `previous` --
    // active again. With `build`, the rule is made prior to exiting the
    // scratch, using the output the thunk returned. The scratch retains the
    // content the thunk generated, allowing the caller to read it and
    // subsequently either merge it into `previous` or drop it.
    JanetSignal ScriptEngine::Impl::run_rule_build(const char* const caller, JanetFunction* const thunk, std::string& previous, Janet& out, const RuleBuildStep build, std::vector<network::Node>* const mention_markings)
    {
        refuse_nested_rule(caller);

        previous = _n->active_cluster_name();
        _n->set_active_cluster(kRuleScratch);

        // The thunk builds for the rule a rule STRUCTURE, not a claim -- refer
        // to the revocation in janet_cfun_zelph_fact, which must remain
        // excluded from rule construction or else a second rule mentioning the
        // same ground statement would turn the initial rule's pattern into
        // data. The caller marks the rule's parts within what the thunk
        // created, thus any fact the thunk creates alongside the rule stays
        // unmarked: a claim. A collection it writes is the rule's own, and its
        // identifier says so for good (Zelph::enter_template_scope).
        _building_rule = true;

        // A mention writes values rather than rule text, thus opening no
        // template scope; the pattern scope retains the refutation markings
        // it writes until its marks are set (finish_mention).
        const bool  saved_mentioning = std::exchange(_mentioning, mention_markings != nullptr);
        JanetSignal signal           = JANET_SIGNAL_OK;
        {
            std::optional<network::Zelph::TemplateScope> scope;
            std::optional<network::Zelph::PatternScope>  held;
            if (mention_markings)
                held.emplace(*_n, mention_markings);
            else
                scope.emplace(*_n);
            signal = pcall_rooted(thunk, 0, nullptr, &out);

            // Here, the step that builds the rule from the value returned by
            // the thunk executes, remaining within the scratch cluster, the
            // template scope, and the build. It throws an exception rather than
            // triggering a Janet panic, which is a longjmp and would bypass the
            // scope's destructor. This exception is treated as the build's
            // error, just as an error from the thunk would be.
            if (signal == JANET_SIGNAL_OK && build != nullptr)
            {
                try
                {
                    const Rooted returned(out);
                    out = build(returned.value);
                }
                catch (const std::exception& ex)
                {
                    out    = janet_cstringv(ex.what());
                    signal = JANET_SIGNAL_ERROR;
                }
                catch (...)
                {
                    out    = janet_cstringv("unknown error");
                    signal = JANET_SIGNAL_ERROR;
                }
            }
        }

        _building_rule = false;
        _mentioning    = saved_mentioning;

        // A scratch cluster of our own must not swallow the user's: whatever
        // survives is handed back to the cluster that was active, so
        // .cluster-drop still rolls a rule back with the rest of an experiment.
        if (previous.empty())
            _n->deactivate_cluster();
        else
            _n->set_active_cluster(previous);

        // Nothing from a half-built rule is kept
        // (refuse_suspended_rule).
        if (suspended(signal))
        {
            _n->drop_scratch_cluster(kRuleScratch);
            refuse_suspended_rule(caller);
        }

        return signal;
    }

    // What the islands of the statement built before its code ran,
    // combined with what a rule's build or a query's recording captured:
    // the islands run while the line is parsed, meaning no recording
    // initiated by the code can see them, and a ground term an island built
    // within a rule stayed data -- "($(a + b) p X) => (X q r)" answered
    // `a + b` to "A + B" -- just as it did when situated under an operator
    // in a query. Only what the walk from the rule's parts or from the
    // operator reaches gets marked.
    void ScriptEngine::Impl::add_island_created(std::vector<network::Node>& created) const
    {
        if (_statement) created.insert(created.end(), _statement->island_created.begin(), _statement->island_created.end());
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_dedup_rule(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/dedup-rule");

        JanetFunction* const thunk = janet_getfunction(argv, 0);
        const bool           focus = argc == 2;
        if (focus && std::string(reinterpret_cast<const char*>(janet_getkeyword(argv, 1))) != "focus")
            janet_panicf("zelph/dedup-rule: the only option is :focus, got %v", argv[1]);

        // A focus in the rule's own slots writes a fact beside the rule,
        // which zelph/aside records for the statement to claim
        // (ParsedStatement::aside, from `own_aside` on). A focus within a
        // rule or an opaque argument that the rule holds records into
        // `mentioned_aside`, where the facts are marked with the rule's
        // parts after the rule is kept.
        ParsedStatement* const     statement = s_instance->_statement;
        const std::size_t          own_aside = statement ? statement->aside.size() : 0;
        std::vector<network::Node> mentioned_aside;

        std::string previous;
        Janet       out    = janet_wrap_nil();
        JanetSignal signal = JANET_SIGNAL_OK;
        {
            const AsideScope mentions(s_instance->_mention_aside, &mentioned_aside);
            signal = s_instance->run_rule_build("zelph/dedup-rule", thunk, previous, out);
        }

        if (signal != JANET_SIGNAL_OK)
        {
            s_instance->_n->drop_scratch_cluster(kRuleScratch); // see build_marked_rule
            janet_signalv(signal, out);
        }

        // The thunk builds the whole rule and returns its `=>` fact, which
        // serves as the basis for the duplicate check and as the starting
        // point of the marking. Any other result is refused just like a
        // failed thunk, and none of what it built is kept: a plain fact
        // used to be kept as a claim, and a condition set was kept as a
        // rule that was never checked for a duplicate.
        network::Node rule = 0;
        network::Node part = 0;
        if (!focus)
        {
            rule = zelph_unwrap_node(out);
        }
        else
        {
            const Janet* items = nullptr;
            int32_t      len   = 0;
            if (janet_indexed_view(out, &items, &len) && len == 2)
            {
                rule = zelph_unwrap_node(items[0]);
                part = zelph_unwrap_node(items[1]);

                // The parser passes a name or variable in focus --
                // `(X p Y) => *!` -- to zelph/fact as either a string or a
                // symbol, and this string or symbol is what is received here.
                // It is looked up the way zelph/fact resolved it, without
                // creating anything. A value that refers to no part of the rule
                // is refused below.
                if (janet_checktype(items[1], JANET_STRING))
                {
                    part = s_instance->resolve_janet_arg_no_create(items[1], s_instance->_n->lang());
                }
                else if (janet_checktype(items[1], JANET_SYMBOL))
                {
                    std::lock_guard<std::mutex> lock(s_instance->_state_mutex);
                    const auto                  it = s_instance->_scoped_variables.find(reinterpret_cast<const char*>(janet_unwrap_symbol(items[1])));
                    if (it != s_instance->_scoped_variables.end() && s_instance->_n->exists(it->second)) part = it->second;
                }
            }
        }

        const network::Zelph* const z       = s_instance->_n;
        bool                        is_rule = rule != 0 && z->predicate_of(rule) == z->core.Causes;
        if (is_rule && focus)
        {
            network::adjacency_set consequences;
            const network::Node    condition = z->parse_fact(rule, consequences);
            is_rule                          = part != 0 && (part == condition || part == z->core.Causes || consequences.count(part) == 1);
        }

        if (!is_rule)
        {
            std::string returned;
            {
                const Rooted held(out);
                returned = format_janet(held.value);
            }
            s_instance->_n->drop_scratch_cluster(kRuleScratch);
            if (focus)
                throw std::runtime_error("zelph/dedup-rule: the thunk returned " + returned
                                         + ", which is not a rule and one of its parts: with :focus it builds the whole rule and "
                                           "returns its `=>` fact together with the condition, the `=>` or a consequence of it");
            throw std::runtime_error("zelph/dedup-rule: the thunk returned " + returned
                                     + ", which is not a rule: it builds the whole rule and returns its `=>` fact");
        }

        // A `=>` fact involving variables that does not conform to the form
        // of a rule, such as `S => O` or `(X p Y) => O`, asks which
        // implications exist (see below and asks_implications).
        // It functions as a query just like any other: once answered, what
        // it wrote that holds its variables is removed
        // (Zelph::drop_query_text). When kept as a rule's text, it remained
        // within the graph after every asking.
        if (s_instance->_statement && asks_implications(z, rule))
        {
            std::vector<network::Node>                                 created  = s_instance->_n->cluster_nodes(kRuleScratch);
            const std::vector<std::pair<network::Node, network::Node>> declared = s_instance->_n->cluster_declarations(kRuleScratch);
            s_instance->_n->merge_cluster(kRuleScratch, previous);
            s_instance->add_island_created(created);
            s_instance->_statement->asks = true;
            s_instance->_statement->query_declared.insert(s_instance->_statement->query_declared.end(), declared.begin(), declared.end());
            s_instance->_statement->query_created.insert(s_instance->_statement->query_created.end(), created.begin(), created.end());
            return focus ? zelph_wrap_node(part) : out;
        }

        // A rule is rejected when its consequence includes a predicate that
        // no condition binds, and nothing it built is kept
        // (unbound_predicate).
        {
            network::adjacency_set consequences;
            const network::Node    condition = z->parse_fact(rule, consequences);
            if (const std::string why = unbound_predicate(z, {condition}, consequences); !why.empty())
            {
                s_instance->_n->drop_scratch_cluster(kRuleScratch);
                throw std::runtime_error(why);
            }
        }

        const network::Node twin = s_instance->find_duplicate_rule(rule);

        // A typed rule statement qualifies as a rule even if its focus causes
        // it to evaluate to one of its parts: the statement claims the rule
        // itself, not the component, and is not a query
        // (ScriptEngine::process_janet). A `=>` fact that lacks the form of a
        // rule -- `S => O`, `c => X` -- represents the inquiry into which
        // implications are present, and continues to be one.
        const network::Node kept = twin != 0 ? twin : rule;
        if (s_instance->_statement && z->has_rule_form(kept)) s_instance->_statement->rule = kept;

        if (twin == 0)
        {
            // What the scratch cluster recorded is exactly what this statement
            // brought into being -- which is how a GROUND pattern can be told
            // from the same statement asserted earlier. Read it before the
            // merge, which drops the bookkeeping.
            std::vector<network::Node> created = s_instance->_n->cluster_nodes(kRuleScratch);
            s_instance->_n->merge_cluster(kRuleScratch, previous);
            s_instance->add_island_created(created);
            s_instance->_n->mark_rule_patterns(rule, created);
            for (const network::Node fact : mentioned_aside)
                s_instance->_n->mark_mention(fact, created);
            s_instance->assert_kept_rule(rule);
            return focus ? zelph_wrap_node(part) : out;
        }

        // Find the corresponding part of the existing rule at this moment,
        // while the new rule remains in place.
        const network::Node kept_part = focus ? network::corresponding_part(z, rule, twin, part) : 0;

        // When the rule duplicates one the graph has, what a focus wrote
        // beside it stays. The drop previously removed all that the build
        // generated: following "(a p b, X r Y) => (X q Y)", the line
        // "((x says *(a p b)), X r Y) => (X q Y)" lost `x says (a p b)`,
        // leaving the statement with nothing to claim. These facts and their
        // dependencies exit the scratch and go to the cluster that was active
        // before, so `.cluster-drop` still takes them back.
        if (statement != nullptr && statement->aside.size() > own_aside)
        {
            const std::vector<network::Node> aside(statement->aside.begin() + static_cast<std::ptrdiff_t>(own_aside), statement->aside.end());
            s_instance->_n->move_into_cluster(previous, needed_by_aside(z, aside, s_instance->_n->cluster_nodes(kRuleScratch)));
        }

        // The scratch drop must not disarm the fact stores: re-entering an
        // existing rule is an ordinary thing to do, and it used to cost the
        // session its genuine-structure store. See drop_scratch_cluster.
        s_instance->_n->drop_scratch_cluster(kRuleScratch);
        s_instance->assert_kept_rule(twin);
        return zelph_wrap_node(focus ? kept_part : twin);
    }

    // A builder asserts the rule it keeps, just as a typed rule asserts
    // its own: a rule zelph/mention authored carries the mention's mark
    // (Zelph::mark_mention), and the identical text built afterwards by
    // zelph/dedup-rule or zelph/build-rule is that node, hash-consed.
    // Left marked, it was returned without ever coming into force. Within a
    // parsed statement, the statement claims the rule upon completion, and a
    // statement that asks claims nothing, so the claim here occurs
    // solely outside one.
    void ScriptEngine::Impl::assert_kept_rule(const network::Node rule) const
    {
        if (_statement == nullptr && rule != 0 && _n->predicate_of(rule) == _n->core.Causes) _n->unmark_rule_pattern(rule);
    }

    // Build a rule in the manner a parsed rule is constructed, omitting
    // the check for duplication: the build of the zelph/rule macro
    // (zelph/build-rule-parts), and zelph/build-rule, the scope in which a
    // rule authored by hand operates.
    //
    // Before a function executes, its arguments are evaluated, meaning the
    // function cannot know which parts of a rule its call built. Instead, the
    // macro encloses its argument forms within a thunk, which executes here
    // within the scratch cluster. This cluster records exactly what the thunk
    // creates -- a helper it calls included -- and the thunk executes within
    // the template scope, so a collection it writes is the rule's own.
    // Subsequently, the rule's ground parts are marked in the same manner as
    // the parser marks a typed rule's: namely, the parts of each `=>` fact
    // over the rule's condition, which corresponds to the condition set
    // returned by the build (the rule that `build`, the macro's step, made
    // from the parts the thunk returned) or the subject of the `=>` fact
    // that the thunk returns. A thunk that manually writes a rule's
    // consequences as several `=>` facts under a single condition returns
    // just one of them. Any node that predated the call was not
    // created by it and stays what it was.
    Janet ScriptEngine::Impl::build_marked_rule(JanetFunction* const thunk, const RuleBuildStep build)
    {
        std::string       previous;
        Janet             out    = janet_wrap_nil();
        const JanetSignal signal = run_rule_build("zelph/rule", thunk, previous, out, build);

        // A failed build retains nothing that its thunk built, just as a wait
        // already retained nothing. After being merged, a rule that the forms
        // wrote as a `=>` fact on the side was in effect independently, a rule
        // that failed after its first consequence was in force with that one,
        // and the ground conditions of a rule that was never completed turned
        // into data.
        if (signal != JANET_SIGNAL_OK)
        {
            _n->drop_scratch_cluster(kRuleScratch);
            janet_signalv(signal, out);
        }

        // The thunk linked to zelph/build-rule writes the `=>` fact
        // directly, a fact that make_rule has not seen (unbound_predicate).
        // Every `=>` fact it wrote, over the condition of the one it returns,
        // constitutes a rule in its own right, as the marking below reads
        // it, and undergoes validation too.
        if (const network::Node rule = build == nullptr ? zelph_unwrap_node(out) : network::Node{0}; rule != 0 && _n->predicate_of(rule) == _n->core.Causes)
        {
            network::adjacency_set consequences;
            const network::Node    condition = _n->parse_fact(rule, consequences);
            for (const network::Node other : _n->cluster_nodes(kRuleScratch))
            {
                network::adjacency_set more;
                if (condition != 0 && other != rule && _n->predicate_of(other) == _n->core.Causes && _n->parse_fact(other, more) == condition)
                    for (const network::Node consequence : more)
                        consequences.insert(consequence);
            }
            if (const std::string why = unbound_predicate(_n, {condition}, consequences); !why.empty())
            {
                _n->drop_scratch_cluster(kRuleScratch);
                throw std::runtime_error("zelph/build-rule: " + why);
            }
        }

        // Read before the merge, which drops the accounting
        // records.
        const std::vector<network::Node> created = _n->cluster_nodes(kRuleScratch);
        _n->merge_cluster(kRuleScratch, previous);

        const network::Node result = zelph_unwrap_node(out);
        if (result == 0) return out;

        network::adjacency_set consequences;
        const network::Node    condition = _n->predicate_of(result) == _n->core.Causes
                                             ? _n->parse_fact(result, consequences)
                                             : result;

        for (const network::Node rule : created)
        {
            if (_n->predicate_of(rule) != _n->core.Causes) continue;
            if (rule == result || (condition != 0 && _n->parse_fact(rule, consequences) == condition))
                _n->mark_rule_patterns(rule, created);
        }
        assert_kept_rule(result);
        return out;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_build_rule(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/build-rule");

        return s_instance->build_marked_rule(janet_getfunction(argv, 0), nullptr);
    }

    // The C function that the zelph/rule macro expands into. Its thunk
    // evaluates the macro's argument forms and returns them as a single tuple,
    // with conditions appearing first, and the rule is built from this tuple
    // within the build (rule_from_parts). A builder invoked by the thunk would
    // run after the argument forms, originating from the same frame as any
    // call those forms initiate, making it impossible to distinguish the
    // macro's call from any other. No function in the expansion builds a rule
    // outside a build: a program that takes this one via macex1 initiates its
    // own build, which is rejected when attempted within another, just as
    // zelph/rule* is.
    Janet ScriptEngine::Impl::janet_cfun_zelph_build_rule_parts(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/rule");

        return s_instance->build_marked_rule(janet_getfunction(argv, 0), &rule_from_parts);
    }

    // Mark as rule patterns the ground parts of each rule within `created`,
    // which is what a mention brought into existence. Each `=>` fact present
    // there counts, just as it does during a firing (Reasoning::deduce), and
    // not merely the one that the thunk returns: a thunk authored manually
    // might return the statement that names the rule.
    void ScriptEngine::Impl::mark_mentioned_rules(const std::vector<network::Node>& created) const
    {
        for (const network::Node n : created)
            if (_n->predicate_of(n) == _n->core.Causes) _n->mark_rule_patterns(n, created);
    }

    // Write a rule that another statement mentions: the parser wraps every
    // `=>` found inside a different statement in this. Mentioning a rule
    // claims none of its parts. The thunk runs as that of zelph/build-rule
    // does, within the scratch cluster and the template scope, and the ground
    // parts of every rule it created are marked as rule patterns, just as
    // those of a typed rule are: `((a p b) => (c q d)) is noted` answers
    // neither `a p b` nor `c q d`, and the collections the thunk produces
    // belong exclusively to the rule, so `((X p Y) => (X q @{c})) is noted`
    // does not answer `c in @{c}`. A part that was present beforehand retains
    // its prior status. A mentioned rule is marked exclusively as a part of
    // another rule the thunk created; otherwise, it may serve as data and a
    // premise. Inside a parsed statement, the marking is deferred until the
    // statement concludes and is then discarded for a query (ParsedStatement).
    // During the construction of a rule, the enclosing build marks the mention
    // with the rest of its parts.
    Janet ScriptEngine::Impl::janet_cfun_zelph_rule_text(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/rule-text");

        JanetFunction* const thunk = janet_getfunction(argv, 0);
        Janet                out   = janet_wrap_nil();

        if (!s_instance->_building_rule)
        {
            std::string                previous;
            std::vector<network::Node> aside;
            JanetSignal                signal = JANET_SIGNAL_OK;
            {
                const AsideScope own(s_instance->_aside, &aside);
                const AsideScope nested(s_instance->_mention_aside, &aside);
                signal = s_instance->run_rule_build("zelph/rule-text", thunk, previous, out);
            }

            // A mention that does not succeed leaves no trace: if merged, its
            // parts would remain unmarked, as claims that nobody made.
            if (signal != JANET_SIGNAL_OK)
            {
                s_instance->_n->drop_scratch_cluster(kRuleScratch);
                janet_signalv(signal, out);
            }

            // Read before the merge, which drops the accounting
            // records.
            std::vector<network::Node> created = s_instance->_n->cluster_nodes(kRuleScratch);
            s_instance->_n->merge_cluster(kRuleScratch, previous);
            s_instance->add_island_created(created);
            s_instance->mark_mentioned_aside(aside, created);

            // A mentioned `=>` that asks about the existence of implications
            // turns the statement into a query (asks_implications): "x says
            // (S => O)" was answered, and then claimed with its variables.
            // The parser wrote the code for a query for the same reason
            // (names_query_variable), so what it wrote is removed once the
            // query is answered.
            if (s_instance->_statement && asks_implications(s_instance->_n, zelph_unwrap_node(out))) s_instance->_statement->asks = true;

            if (s_instance->_statement)
                s_instance->_statement->mentions.push_back(std::move(created));
            else
                s_instance->mark_mentioned_rules(created);
            return out;
        }

        JanetSignal signal = JANET_SIGNAL_OK;
        {
            const network::Zelph::TemplateScope scope(*s_instance->_n);
            const BlockScope                    rule_variables(s_instance->_mentioning, false);
            const AsideScope                    own(s_instance->_aside, s_instance->_mention_aside);
            signal = pcall_rooted(thunk, 0, nullptr, &out);
            if (suspended(signal)) refuse_suspended_rule("zelph/rule-text");
        }

        if (signal != JANET_SIGNAL_OK) janet_signalv(static_cast<JanetSignal>(signal), out);
        return out;
    }

    // Within a mention, the facts a focus wrote beside the part it hands on
    // are mentioned too: they are marked at the end of the statement, or at
    // once outside one. The query "X says Y" answered `x says (a p b)`
    // following the line "k notes ((x says *(a p b)) => (c q d))", which
    // solely addresses the rule. A fact that existed before keeps its
    // status.
    void ScriptEngine::Impl::mark_mentioned_aside(const std::vector<network::Node>& facts, const std::vector<network::Node>& created) const
    {
        for (const network::Node fact : facts)
        {
            if (_statement)
                _statement->statement_mentions.push_back({fact, created, {}});
            else
                _n->mark_mention(fact, created);
        }
    }

    // (zelph/aside fact): return the fact, and record it as the one a focus
    // writes beside the part it hands on (`_aside`). Not recorded are a fact
    // that holds a variable, a rule, and the tag of a comma list or of a
    // condition operator: these belong to the text, not to a claim.
    Janet ScriptEngine::Impl::janet_cfun_zelph_aside(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return argv[0];
        require_main_thread("zelph/aside");

        const network::Node         fact = zelph_unwrap_node(argv[0]);
        const network::Zelph* const z    = s_instance->_n;
        if (s_instance->_aside != nullptr && fact != 0 && !network::Zelph::is_var(fact) && network::Zelph::is_hash(fact))
        {
            network::adjacency_set objects;
            z->parse_fact(fact, objects);
            const network::Node predicate = z->predicate_of(fact);
            const bool          tag       = z->is_condition_tag(fact) || (predicate == z->core.IsA && objects.count(z->core.Conjunction) != 0);
            if (predicate != 0 && predicate != z->core.Causes && !tag && !z->var_in_closure(fact))
                s_instance->_aside->push_back(fact);
        }
        return argv[0];
    }

    // What a mention wrote turns into one once it is finalized: its marks
    // are set at the end of the parsed statement, discarded when the
    // statement asks, or immediately outside one. A refutation marking it
    // held back refutes only where it stayed a claim, as a fact the thunk
    // wrote beside the statement it returns.
    void ScriptEngine::Impl::finish_mention(const network::Node root, std::vector<network::Node>& created, std::vector<network::Node>& markings) const
    {
        add_island_created(created);
        if (_statement)
            _statement->statement_mentions.push_back({root, std::move(created), std::move(markings)});
        else
            mark_mention({root, std::move(created), std::move(markings)});
    }

    void ScriptEngine::Impl::mark_mention(const ParsedStatement::Mention& mention) const
    {
        _n->mark_mention(mention.root, mention.created);
        for (const network::Node marking : mention.markings)
            if (const network::Node refuted = _n->refuted_marking_subject(marking); refuted != 0 && _n->exists(marking) && !_n->is_rule_pattern(marking) && !_n->var_in_closure(refuted))
                _n->apply_refutation(refuted);
    }

    // Write a statement without claiming it: (zelph/mention s p o & objs)
    // generates a single fact, (zelph/mention thunk) runs a thunk that
    // builds one and yields it. What the call creates and the statement
    // reaches gets the rule-pattern mark -- one mark for a rule's text and
    // for a mention, distinguished by position (Zelph::is_mention) -- and
    // whatever was present beforehand retains its status, including a claim:
    // a mention of an asserted statement still answers. The thunk runs as
    // zelph/rule-text's does, within the scratch cluster and without
    // claiming, yet outside the template scope: a collection it writes is a
    // value.
    Janet ScriptEngine::Impl::janet_cfun_zelph_mention(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/mention");
        if (argc == 2) janet_panic("zelph/mention: give a thunk that builds the statement, or a subject, a predicate and at least one object");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/mention", argc, argv, true);

        Janet out = janet_wrap_nil();
        if (argc == 1)
        {
            JanetFunction* const thunk = janet_getfunction(argv, 0);

            // Within a rule build, the build marks the mention along with
            // the rest of its parts, as zelph/rule-text defers to the build
            // for handling.
            if (s_instance->_building_rule)
            {
                JanetSignal signal = JANET_SIGNAL_OK;
                {
                    const AsideScope own(s_instance->_aside, s_instance->_mention_aside);
                    signal = pcall_rooted(thunk, 0, nullptr, &out);
                }
                if (suspended(signal)) refuse_suspended_rule("zelph/mention");
                if (signal != JANET_SIGNAL_OK) janet_signalv(signal, out);
                return out;
            }

            std::string                previous;
            std::vector<network::Node> markings;
            std::vector<network::Node> aside;
            JanetSignal                signal = JANET_SIGNAL_OK;
            {
                const AsideScope own(s_instance->_aside, &aside);
                const AsideScope nested(s_instance->_mention_aside, &aside);
                signal = s_instance->run_rule_build("zelph/mention", thunk, previous, out, nullptr, &markings);
            }
            if (signal != JANET_SIGNAL_OK)
            {
                s_instance->_n->drop_scratch_cluster(kRuleScratch);
                janet_signalv(signal, out);
            }

            // The mark starts at the statement that the thunk yields,
            // meaning a thunk producing no node would result in all it wrote
            // being claimed.
            const network::Node root = zelph_unwrap_node(out);
            if (root == 0)
            {
                std::string returned;
                {
                    const Rooted held(out);
                    returned = format_janet(held.value);
                }
                s_instance->_n->drop_scratch_cluster(kRuleScratch);
                throw std::runtime_error("zelph/mention: the thunk returned " + returned + ", which is not a node: it builds the statement and returns it");
            }

            // Read before the merge, which drops the accounting records.
            std::vector<network::Node> created = s_instance->_n->cluster_nodes(kRuleScratch);
            s_instance->_n->merge_cluster(kRuleScratch, previous);
            s_instance->mark_mentioned_aside(aside, created);
            s_instance->finish_mention(root, created, markings);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/mention", argc, argv, false, out);
            return out;
        }

        const network::Node    s = s_instance->resolve_janet_arg(argv[0]);
        const network::Node    p = s_instance->resolve_janet_arg(argv[1]);
        network::adjacency_set objs;
        for (int32_t i = 2; i < argc; ++i)
            if (const network::Node o = s_instance->resolve_janet_arg(argv[i]); o != 0) objs.insert(o);
        if (s == 0 || p == 0 || objs.empty()) return janet_wrap_nil();

        // Before the invocation, the arguments had been constructed, thus
        // the sole outcome it can produce is the fact.
        network::Node f = 0;
        if (s_instance->_building_rule)
        {
            f = s_instance->_n->fact(s, p, objs);
        }
        else if (s_instance->_statement)
        {
            const network::Answer known = s_instance->_n->check_fact(s, p, objs);
            if (known.is_known())
            {
                f = known.relation();
            }
            else
            {
                std::vector<network::Node> markings;
                {
                    const network::Zelph::PatternScope held(*s_instance->_n, &markings);
                    f = s_instance->_n->fact(s, p, objs);
                }
                std::vector<network::Node> created{f};
                s_instance->finish_mention(f, created, markings);
            }
        }
        else
        {
            f = s_instance->_n->mention(s, p, objs);
        }

        out = zelph_wrap_node(f);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/mention", argc, argv, false, out);
        return out;
    }

    // Run the code of a query. A query consists of a condition list
    // evaluated once, and what it constructs under an operator is a pattern,
    // exactly as in a rule -- "X go Y, ¬(a p b)" asserted `a p b` and
    // subsequently returned nothing, since the query had constructed its own
    // counterexample. Which of the created nodes stand beneath an operator
    // depends on structure, determined after the statement finishes
    // (Zelph::mark_query_operands), and likewise which of them hold the
    // query's variables and go once it is answered (Zelph::drop_query_text);
    // this records what the statement generated, within a scratch cluster
    // that gets merged into the one that was active before it, ensuring that
    // `.cluster-drop` still rolls the query back. A failing query retains none
    // of the structure it built. Outside a parsed statement and inside a rule
    // build, the thunk executes directly.
    Janet ScriptEngine::Impl::janet_cfun_zelph_query_text(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/query-text");

        JanetFunction* const thunk  = janet_getfunction(argv, 0);
        Janet                out    = janet_wrap_nil();
        JanetSignal          signal = JANET_SIGNAL_OK;

        if (s_instance->_statement == nullptr || s_instance->_building_rule)
        {
            signal = pcall_rooted(thunk, 0, nullptr, &out);
            if (suspended(signal)) refuse_suspended("zelph/query-text: a statement's code cannot wait for the event loop or yield.");
            if (signal != JANET_SIGNAL_OK) janet_signalv(signal, out);
            return out;
        }

        {
            const std::string previous = s_instance->_n->active_cluster_name();
            s_instance->_n->set_active_cluster(kQueryScratch);

            // Nothing within a query refutes before the completion of its
            // associated statement; refer to
            // ParsedStatement::query_refutations.
            std::vector<network::Node> refutations;
            {
                const network::Zelph::PatternScope held(*s_instance->_n);
                std::vector<network::Node>* const  saved = std::exchange(s_instance->_query_refutations, &refutations);
                signal                                   = pcall_rooted(thunk, 0, nullptr, &out);
                s_instance->_query_refutations           = saved;
            }

            if (previous.empty())
                s_instance->_n->deactivate_cluster();
            else
                s_instance->_n->set_active_cluster(previous);

            if (signal == JANET_SIGNAL_OK)
            {
                // Read before the merge, which drops the accounting records.
                std::vector<network::Node>                                 created  = s_instance->_n->cluster_nodes(kQueryScratch);
                const std::vector<std::pair<network::Node, network::Node>> declared = s_instance->_n->cluster_declarations(kQueryScratch);
                s_instance->_n->merge_cluster(kQueryScratch, previous);
                s_instance->_statement->query_declared.insert(s_instance->_statement->query_declared.end(), declared.begin(), declared.end());
                s_instance->add_island_created(created);
                s_instance->_statement->query_created.insert(s_instance->_statement->query_created.end(), created.begin(), created.end());
                s_instance->_statement->query_refutations.insert(s_instance->_statement->query_refutations.end(), refutations.begin(), refutations.end());
                return out;
            }

            s_instance->_n->drop_scratch_cluster(kQueryScratch);
        }

        if (suspended(signal)) refuse_suspended("zelph/query-text: a statement's code cannot wait for the event loop or yield.");
        janet_signalv(signal, out);
    }

    // Run the code of a claimed statement that carries a refutation alongside
    // what else it writes. The refutation is refused for a fact the graph
    // claims, and the statement may claim that same fact itself: "c q d, (c q
    // d) ~ refuted" was refused after it had asserted a new `c q d`, which
    // stayed, and so did the subject of "(y q z) says ((a p b) ~ refuted)"
    // after `a p b`. The same applies to a statement that takes a node by
    // reference, a Janet variable, or the output of an inline keyword: its
    // claim can reach a rule's marking pattern of a claimed fact. The code and
    // the claim operate within a scratch cluster that is merged into the
    // previously active cluster once the statement finishes, and is dropped
    // otherwise: the drop removes what the statement created and takes back
    // the pattern marks and refutations it changed in what existed
    // (Network::note_unmarked, Network::note_refutation). Outside a parsed
    // statement and within a rule build, the thunk executes directly.
    Janet ScriptEngine::Impl::janet_cfun_zelph_claim_text(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/claim-text");

        JanetFunction* const thunk  = janet_getfunction(argv, 0);
        Janet                out    = janet_wrap_nil();
        JanetSignal          signal = JANET_SIGNAL_OK;

        ParsedStatement* const statement = s_instance->_statement;
        if (statement == nullptr || s_instance->_building_rule || statement->claim_scratch
            || s_instance->_n->active_cluster_name() == kClaimScratch)
        {
            signal = pcall_rooted(thunk, 0, nullptr, &out);
            if (suspended(signal)) refuse_suspended("zelph/claim-text: a statement's code cannot wait for the event loop or yield.");
            if (signal != JANET_SIGNAL_OK) janet_signalv(signal, out);
            return out;
        }

        // The scratch stays active even after the code has succeeded,
        // continuing until the statement is finalized (end_claim_scratch):
        // the claim at the end of the line may still be refused, and it was
        // made after the merge, so `x says ,m` with a rule's marking pattern
        // of a claimed fact in `m` was refused and stayed written.
        statement->claim_previous = s_instance->_n->active_cluster_name();
        statement->claim_scratch  = true;
        s_instance->_n->set_active_cluster(kClaimScratch);
        signal = pcall_rooted(thunk, 0, nullptr, &out);
        if (signal == JANET_SIGNAL_OK) return out;

        s_instance->end_claim_scratch(false);
        if (suspended(signal)) refuse_suspended("zelph/claim-text: a statement's code cannot wait for the event loop or yield.");
        janet_signalv(signal, out);
    }

    void ScriptEngine::Impl::end_claim_scratch(const bool keep)
    {
        if (_statement == nullptr || !_statement->claim_scratch) return;
        _statement->claim_scratch = false;

        const std::string& previous = _statement->claim_previous;
        if (previous.empty())
            _n->deactivate_cluster();
        else
            _n->set_active_cluster(previous);

        // A statement that revealed itself as a query only during execution
        // was written as a claim: "x says ((X p Y) => ,k)" where `k` is a
        // name asks which implications exist (zelph/rule-text), and what the
        // line constructed remained after each inquiry. The scratch recorded
        // everything, and that is the query's text (Zelph::drop_query_text).
        if (keep && _statement->asks)
        {
            const std::vector<network::Node> made = _n->cluster_nodes(kClaimScratch);
            _statement->query_created.insert(_statement->query_created.end(), made.begin(), made.end());
            const std::vector<std::pair<network::Node, network::Node>> declared = _n->cluster_declarations(kClaimScratch);
            _statement->query_declared.insert(_statement->query_declared.end(), declared.begin(), declared.end());
        }

        if (keep)
            _n->merge_cluster(kClaimScratch, previous);
        else
            _n->drop_scratch_cluster(kClaimScratch);
    }

    // Run the code that builds a condition operator -- `¬`, `≈`, `⁺`/`∗`,
    // or any of these expressed explicitly as the fact they generate. In
    // both a rule and a query, its operand is a pattern, not a claim, thus
    // it is LOOKED UP wherever the graph holds it (_resolving_pattern):
    // following "¬(a p b)", the rule "(X go Y, ¬(a p b)) => (X ok Y)" and
    // the query "X go Y, ¬(a p b)" both failed with "this fact is known to
    // be wrong", even though a refuted fact is precisely one that does not
    // hold. What the graph does not hold is built as before.
    Janet ScriptEngine::Impl::janet_cfun_zelph_operand(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/operand");

        JanetFunction* const thunk  = janet_getfunction(argv, 0);
        Janet                out    = janet_wrap_nil();
        JanetSignal          signal = JANET_SIGNAL_OK;
        {
            const BlockScope looking_up(s_instance->_resolving_pattern, true);
            signal = pcall_rooted(thunk, 0, nullptr, &out);
        }

        if (suspended(signal)) refuse_suspended("zelph/operand: a condition cannot wait for the event loop or yield.");
        if (signal != JANET_SIGNAL_OK) janet_signalv(signal, out);
        return out;
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
    //   (zelph/rule* [cond1 cond2] consequence1 consequence2)
    //
    // The builder behind zelph/rule* and the zelph/rule macro. Invoked via
    // zelph/rule*, it receives its arguments already constructed, thus it
    // marks nothing, and any collection within them is a value. Within the
    // macro, execution occurs inside a rule's build, where a Janet panic would
    // longjmp past the template scope's destructor, hence it throws. Each
    // argument is validated before any writing: a consequence refused after
    // the first one had been written left the rule in force with that one. An
    // identifier that refers to no node within the graph is not a node
    // either: it may be a number, or a node that a rolled-back build
    // generated and that a Janet binding still holds. Such an identifier
    // passed the check but failed solely during the rule's writing phase,
    // once its condition set had already been established.
    Janet ScriptEngine::Impl::make_rule(const int32_t argc, const Janet* const argv)
    {
        // No fewer than conditions plus 1 consequence, as stated in the words
        // of janet_arity.
        if (argc < 2) throw std::runtime_error("arity mismatch, expected at least 2, got " + std::to_string(argc));
        if (_log_janet_functions) log_janet_call("zelph/rule*", argc, const_cast<Janet*>(argv), true);

        // First argument: indexed collection of condition fact nodes
        const Janet* cond_data;
        int32_t      cond_len;
        if (!janet_indexed_view(argv[0], &cond_data, &cond_len) || cond_len == 0)
            throw std::runtime_error("zelph/rule: first argument must be a non-empty array or tuple of conditions");

        // Collect condition nodes
        std::unordered_set<network::Node> condition_nodes;
        for (int32_t i = 0; i < cond_len; ++i)
        {
            network::Node n = zelph_unwrap_node(cond_data[i]);
            if (!n || !_n->exists(n))
                throw std::runtime_error("zelph/rule: condition at index " + std::to_string(i) + " is not a valid zelph/node");

            // Same reason as in zelph/conjunction: a condition that is not a
            // pattern makes the rule inert, and a generator that builds one
            // has no other way to find out.
            if (!is_condition_pattern(n))
                throw std::runtime_error("zelph/rule: condition at index " + std::to_string(i) + " is \"" + _n->format(n)
                                         + "\", which is not a fact pattern and can never match");

            condition_nodes.insert(n);
        }

        network::adjacency_set consequences;
        for (int32_t i = 1; i < argc; ++i)
        {
            const network::Node consequence = zelph_unwrap_node(argv[i]);
            if (!consequence || !_n->exists(consequence))
                throw std::runtime_error("zelph/rule: consequence at index " + std::to_string(i - 1) + " is not a valid zelph/node");
            consequences.insert(consequence);
        }

        if (const std::string why = unbound_predicate(_n, {condition_nodes.begin(), condition_nodes.end()}, consequences); !why.empty())
            throw std::runtime_error("zelph/rule: " + why);

        if (condition_nodes.empty())
        {
            Janet res = janet_wrap_nil();
            if (_log_janet_functions) log_janet_call("zelph/rule*", argc, const_cast<Janet*>(argv), false, res);
            return res;
        }

        // Create condition set and mark as conjunction
        network::Node condition_set = _n->conjunction_collection(condition_nodes);
        _n->fact(condition_set, _n->core.IsA, {_n->core.Conjunction});

        // A `=>` fact with all the consequences as its objects, as the
        // parser writes `(A) => (B) (C)`. A `=>` fact for each consequence
        // meant a rule for each consequence: each fired independently,
        // so a fresh variable shared by two consequences had a witness
        // within each one.
        _n->fact(condition_set, _n->core.Causes, consequences);

        Janet res = zelph_wrap_node(condition_set);
        if (_log_janet_functions) log_janet_call("zelph/rule*", argc, const_cast<Janet*>(argv), false, res);
        return res;
    }

    // The step in the zelph/rule macro's build (run_rule_build): the thunk
    // returns the macro's arguments as a single tuple, and the rule is made
    // from it while the build remains active.
    Janet ScriptEngine::Impl::rule_from_parts(const Janet parts)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(parts, &data, &len))
            throw std::runtime_error("zelph/rule: the forms did not return the rule's conditions and consequences");
        return s_instance->make_rule(len, data);
    }

    // `zelph/rule*`, refused while a rule is being built
    // (`refuse_nested_rule`).
    Janet ScriptEngine::Impl::janet_cfun_zelph_rule_star(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/rule*");
        s_instance->refuse_nested_rule("zelph/rule*");
        return s_instance->make_rule(argc, argv);
    }

    // Build a cons list from string characters (for compact <abc> syntax).
    // Characters are reversed before list construction so that the last (rightmost)
    // character — the least significant digit in a numeric string — becomes the
    // outermost cons cell. This matches the node-list syntax where the user writes
    // digits in reverse order: <3 2 1> and <123> produce the same internal structure.
    //
    // Each character corresponds to the node bearing that name within
    // language zelph, regardless of what .lang says. A character does not
    // represent a word from the session's language, and the arithmetic
    // substrates build the digits of a literal here, so &3 must denote a
    // single node under every language.
    Janet ScriptEngine::Impl::janet_cfun_zelph_list_chars(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/list-chars");
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

        network::Node list_node = s_instance->_n->list(elements, "zelph");
        Janet         res       = zelph_wrap_node(list_node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list-chars", argc, argv, false, res);
        return res;
    }

    // Build a cons list from existing nodes (for < A B > node-list syntax).
    // The first node in the input becomes the outermost cons cell (= head of the cons list).
    // For numbers, write digits in reverse so that the LSB comes first:
    // <3 2 1> gives 3 as the outermost car (= LSB of "123"), matching the internal
    // structure of the compact <123> syntax.
    Janet ScriptEngine::Impl::janet_cfun_zelph_list(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/list");
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

    Janet ScriptEngine::Impl::janet_cfun_zelph_set(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/set");
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

    Janet ScriptEngine::Impl::janet_cfun_zelph_collection(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/collection");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/collection", argc, argv, true);

        std::unordered_set<network::Node> elements;
        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.insert(n);
        }

        network::Node node = s_instance->_n->collection(elements);
        if (node && s_instance->_statement && !s_instance->_building_rule) s_instance->_statement->collections.push_back(node);
        Janet res = zelph_wrap_node(node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/collection", argc, argv, false, res);
        return res;
    }

    // Is `n` something a rule may hold as a condition?
    //
    // Unification matches a PATTERN, so a condition that carries no statement
    // matches nothing and the rule containing it can never fire. A nested
    // condition set is the one member that is not itself a fact: the
    // evaluator descends into it.
    bool ScriptEngine::Impl::is_condition_pattern(const network::Node n) const
    {
        if (n == 0) return false;
        if (_n->predicate_of(n) != 0) return true;
        return _n->check_fact(n, _n->core.IsA, {_n->core.Conjunction}).is_known();
    }

    // Every member of a set that has just been tagged `~ conjunction`.
    void ScriptEngine::Impl::check_conditions_are_patterns(const network::Node set) const
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
    Janet ScriptEngine::Impl::janet_cfun_zelph_conjunction(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, -1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/conjunction");
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

        const network::Node set = s_instance->_n->conjunction_collection(conditions);
        s_instance->_n->fact(set, s_instance->_n->core.IsA, {s_instance->_n->core.Conjunction});

        // A comma list that a statement writes outside a rule build is
        // assigned to the statement, just as a collection literal is, and its
        // claim enters it. Unentered, the comma list claimed no members:
        // following "(a p b) => (c q d)", the line "x says (a p b, j r s)"
        // treated `a p b` as a pattern, and "x says (,held, j r s)" was
        // accepted with a refuted fact within `held`.
        if (s_instance->_statement && !s_instance->_building_rule) s_instance->_statement->collections.push_back(set);

        Janet res = zelph_wrap_node(set);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/conjunction", argc, argv, false, res);
        return res;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_fact(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/fact");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, true);

        // The claim below may be refused, and then neither the statement nor a
        // name it would create is written
        // (Zelph::refuse_claim_of_marked_parts). A rule's pattern is handed in
        // as a node, so a name is looked up without being created, and solely
        // for such a call.
        if (s_instance->_in_janet_block && !s_instance->_building_rule)
        {
            bool marked = false;
            for (int32_t i = 0; i < argc && !marked; ++i)
                marked = janet_checktype(argv[i], JANET_ABSTRACT) && s_instance->_n->is_rule_pattern(zelph_unwrap_node(argv[i]));
            if (marked)
            {
                network::adjacency_set parts;
                for (int32_t i = 2; i < argc; ++i)
                    if (const network::Node o = s_instance->resolve_janet_arg_no_create(argv[i]); o != 0) parts.insert(o);
                s_instance->_n->refuse_claim_of_marked_parts(s_instance->resolve_janet_arg_no_create(argv[0]), s_instance->resolve_janet_arg_no_create(argv[1]), parts);
            }
        }

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
        //
        // A rule's ground pattern constitutes a pattern as well, and a refuted
        // fact is looked up for it in the identical manner, regardless of its
        // position within the rule: following "¬(john waits nil)", the rule
        // "(john waits nil) => (mary sleeps nil)" was refused as known to be
        // wrong, and likewise "((john waits nil) ~ refuted) => (mary sleeps
        // nil)", the exact rule that reads the refutation. Only the
        // known-wrong answer is taken over here; for any other, fact() returns
        // the existing node directly. A positive condition referencing the
        // refuted fact thus never matches, and a consequence based on it is
        // reported as a contradiction when the rule fires, just as occurs
        // with a rule written before the refutation.
        if (s_instance->_resolving_pattern || s_instance->_building_rule)
        {
            const network::Answer known = s_instance->_n->check_fact(s, p, objs);

            if (const network::Node found = known.is_known() && (s_instance->_resolving_pattern || known.is_wrong()) ? known.relation() : network::Node{0}; found != 0)
            {
                Janet res = zelph_wrap_node(found);
                if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
                return res;
            }
        }

        // A refuted marking of a ground fact constitutes a refutation, and a
        // refutation has its own call, which refuses one of a fact the graph
        // claims: written with zelph/fact, the marking took a claimed fact
        // from every query without a word. A marking whose fact holds a
        // variable is a pattern, and this is the mechanism through which a
        // program requests refutations.
        if (p == s_instance->_n->core.IsA && objs.size() == 1 && !s_instance->_building_rule && !s_instance->_resolving_pattern
            && *objs.begin() == s_instance->_n->get_node(network::Zelph::refuted_fact_name(), "zelph")
            && s_instance->_n->predicate_of(s) != 0 && !s_instance->_n->var_in_closure(s))
        {
            if (s_instance->_in_janet_block)
                throw std::runtime_error("zelph/fact: \"(" + s_instance->_n->format(s) + ") ~ refuted\" claims that the fact does not hold, "
                                                                                         "and that claim is written (zelph/refute subject predicate object), which refuses it for a "
                                                                                         "fact the graph claims. A marking whose fact holds a variable is a pattern and may be built "
                                                                                         "to query refutations.");

            // A ground marking within the query's own parts is claimed when
            // the query is, which occurs once the statement completes; the
            // claim is refused now for a fact that the graph claimed before
            // the query, as "(F) ~ refuted" on its own line is.
            if (s_instance->_query_refutations != nullptr)
            {
                const std::vector<network::Node> made  = s_instance->_n->cluster_nodes(kQueryScratch);
                const network::Answer            fact  = s_instance->_n->check_fact(s);
                const bool                       fresh = std::find(made.begin(), made.end(), s) != made.end();
                if (!fresh && fact.is_correct() && !s_instance->_n->is_rule_pattern(s))
                    throw std::runtime_error("fact(): this fact is known to be true");
                s_instance->_query_refutations->push_back(s);
            }

            // Every other caller corresponds to the code of a claimed
            // statement or the term of an inline keyword. When the statement
            // specifies the fact it marks, the parser builds that fact using
            // zelph/refute, which refuses one that the graph claims; a fact
            // the statement holds by reference arrives at this location
            // without verification: following `a p b` and
            // `%(def f (zelph/fact "a" "p" "b"))`, the lines `,f ~ refuted`
            // and `(x says *,f) ~ refuted` took the claimed `a p b` from each
            // query, just as the node produced by an inline keyword did.
            if (s_instance->_query_refutations == nullptr && s_instance->_n->check_fact(s).is_correct() && !s_instance->_n->is_rule_pattern(s))
                throw std::runtime_error("fact(): this fact is known to be true");
        }

        // A `=>` fact whose consequence features a predicate not bound by any
        // condition is checked once the outermost evaluation concludes, or
        // before a run (Reasoning::note_fact_rule): the inner rule of a
        // generator and a rule a statement holds take on this form until the
        // call that holds them runs, which comes after this one. Inside a rule
        // build as well, where the builder examines solely the rule it
        // returns; one that rejects discards what it built, and the check
        // skips a node that has vanished. Only a node this call creates, or
        // one its claim puts in force (below): an existing rule in force
        // re-entered, one potentially read from a file, is not this call's to
        // take back. A pattern being resolved does not write a rule.
        const bool            unbound_rule = p == s_instance->_n->core.Causes && !s_instance->_resolving_pattern
                                          && !unbound_predicate(s_instance->_n, {s}, objs).empty();
        const network::Answer known_rule   = unbound_rule ? s_instance->_n->check_fact(s, p, objs) : network::Answer(network::Node{0});
        const bool            note_rule    = unbound_rule && !known_rule.is_known();

        // A mention of the rule, written by zelph/mention, is out of force,
        // and the claim below revokes its mark: claimed unchecked, it had been
        // a rule in force whose consequence predicate no condition binds. It
        // is verified as one this call creates, and a rejection takes back the
        // claim alone.
        const bool out_of_force = unbound_rule && known_rule.is_known() && !s_instance->_n->is_rule_in_force(known_rule.relation());

        // What a command resolves is a pattern, and a marking built for one
        // refutes nothing (Zelph::PatternScope): ".explain ((john waits nil)
        // ~ refuted)" un-claimed `john waits nil` permanently, even though
        // the marking itself went with the command's scratch cluster.
        network::Node f = 0;
        {
            std::optional<network::Zelph::PatternScope> pattern;
            if (s_instance->_resolving_pattern) pattern.emplace(*s_instance->_n);
            // A command drops what it resolves, thus a mark of a rule's
            // pattern is not rejected at that point (Zelph::resolution_fact);
            // an operand resolves a pattern that stays, and is rejected just
            // like any writer is.
            f = s_instance->_resolving_command ? s_instance->_n->resolution_fact(s, p, objs) : s_instance->_n->fact(s, p, objs);
        }
        if (f && note_rule) s_instance->_n->note_fact_rule(f, "zelph/fact");

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
        //
        // The facts nested in it are likewise claimed, outside a `=>`, just
        // as a typed statement claims them. Every nested fact that a
        // `zelph/fact` call in the block built was claimed by that call, and
        // one provided by reference was claimed once it turned into data, so
        // only a node still marked -- one obtained from a rule's text -- is
        // entered. A set constant or a collection handed in is not
        // traversed: a marked fact among its members remains a pattern.
        if (f && s_instance->_in_janet_block && !s_instance->_building_rule)
        {
            s_instance->_n->unmark_rule_pattern(f);
            s_instance->_n->claim_marked_parts(f, s, p, objs);
        }
        if (f && out_of_force && s_instance->_n->is_rule_in_force(f)) s_instance->_n->note_fact_rule(f, "zelph/fact", true);

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
    Janet ScriptEngine::Impl::janet_cfun_zelph_refute(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/refute");
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

        // A refutation constitutes a statement concerning the fact, not a
        // claim of it, thus it revokes the pattern-only status of neither the
        // fact itself nor anything nested in it (Zelph::refute). It did, just
        // as an assertion does: once the marking was withdrawn, a fact that a
        // rule had merely written down read as asserted and fired the rule.
        const network::Node f = s_instance->_n->refute(s, p, objs);

        // A parsed statement claims the value it evaluates to, and `¬(F)`
        // evaluates to F (ScriptEngine::process_janet).
        if (f && s_instance->_statement && !s_instance->_building_rule) s_instance->_statement->refuted.push_back(f);

        Janet res = zelph_wrap_node(f);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, false, res);
        return res;
    }

    // Execute a query: print the pattern and trigger matching via apply_rule.
    // This is the Janet equivalent of entering a zelph statement that contains
    // variables (e.g. "X ~ human"). Takes a single zelph/node argument
    // (typically the return value of a zelph/fact call containing variables).
    Janet ScriptEngine::Impl::janet_cfun_zelph_query(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/query");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/query", argc, argv, true);

        // A query resets the variable scope (below). Within the argument
        // forms of a rule, that scope binds the rule's variables, and a
        // symbol that appeared after the query was transformed into a fresh
        // variable: the X and Y in the consequence were no longer those of
        // the condition, and the rule derived `?? q ??`.
        refuse_inside_rule_build("zelph/query");

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
}
