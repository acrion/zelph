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

#include "reasoning.hpp"

#include "contradiction_error.hpp"
#include "fact_structure.hpp"
#include "zelph_impl.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

using namespace zelph::network;

namespace
{
    // Whether a term holds a variable lacking a value, ascertained solely from
    // the term: in contrast to the response from resolve_pattern, this
    // determination is independent of the existence of specific facts, since the
    // comparison does not rely on them.
    bool term_open(Reasoning* const self, const Node term, const Variables& variables)
    {
        const auto no_value = [&variables](const Node variable)
        {
            const auto it = variables.find(variable);
            return it == variables.end() || Zelph::is_var(it->second);
        };
        if (Zelph::is_var(term)) return no_value(term);
        if (!Zelph::is_hash(term) || !self->var_in_closure(term)) return false;

        std::unordered_set<Node> held;
        std::vector<Node>        history;
        collect_variables(self, term, held, 0, history);
        return std::any_of(held.begin(), held.end(), no_value);
    }
}

// A numeric comparison, expressed as `(A zelph:lt B)`, along with its siblings
// zelph:le, zelph:gt, zelph:ge, zelph:eq, and zelph:ne.
//
// The engine responds to it in the same manner as it does to `!=`: a test of
// two bound terms, never involving a lookup of a fact, and no data is stored or
// derived as a result. A structured term like (neg zint A) is read by value
// under the current bindings, without asking whether the graph holds it, so the
// condition depends on nothing that a later fact might supply, and both seeding
// and the negation levels correctly treat it as reading no fact. What it tests
// is the ordering between the two terms by value, which is the verdict that the
// stdlib `cmp` derives wherever `cmp` produces a result (refer to
// numeric_order.cpp, and numeric_compare_oracle.hpp in the tests, which holds
// the two to each other); `cmp` continues to be the basis for the rules that
// rely on it, and this condition is what a data check uses instead of asserting
// a `cmp` fact for each pair and letting the arithmetic rules derive its
// verdict, which would generate ten to sixteen derived facts for a pair of
// seven-digit decimals, compared to none.
//
// Like the guard it runs on the reasoning thread, following the conditions that
// bind its terms: optimize_order schedules it in the tier of `!=`, and behind
// every condition that might still bind a term while one remains open. It
// differs from `!=` in that `!=` employs a deferred reading -- an open term here
// is not remembered for a later condition to resolve, since the plan leaves no
// further condition capable of binding one; the comparison is refused instead,
// once.
void Reasoning::evaluate_comparison(const Node condition, const NumericRelation relation, const RulePos& rule, const ReasoningContext& ctx, const int depth, const bool negated)
{
    const std::string name = numeric_relation_name(relation);

    // The negation of a comparison involving two numbers is itself a
    // comparison, and the relation expressing this is available to be written.
    // `¬` would add only those terms that are not numbers -- `¬(A zelph:lt B)`
    // holding for a list of letters -- which is the reading neither a data
    // check nor SPARQL's `!` applied to a comparison (an error when terms are
    // incomparable) wants. Therefore, it is refused rather than given that
    // interpretation, just as `¬(X != Y)` is.
    if (negated)
    {
        if (should_log(depth)) log(depth, "compare", "negated comparison refused: " + format(condition));
        refuse_condition(condition,
                         "¬ cannot be applied to \"" + name + "\" -- write \""
                             + numeric_relation_name(numeric_relation_complement(relation))
                             + "\" instead, which holds for two numbers exactly when \"" + name
                             + "\" does not. A term that is no number satisfies neither.");
        return;
    }

    // The exact structure and not parse_fact, whose objects also hold the facts
    // that use the condition as their predicate: `x (&3 zelph:lt &5) y` is a
    // statement regarding the comparison and does not change what is being
    // compared.
    const FactStructure structure   = get_preferred_structure(this, condition, depth);
    const Node          lhs_pattern = structure.subject;
    // A term whose subject is its own predicate can make a condition read back
    // without a structure once the graph comes from a file or the fact stores are
    // off (fact_structure.hpp). Such a term can never be a number, and a term
    // that is not a number is not a location for an error.
    if (lhs_pattern == 0) return;
    if (structure.objects.size() != 1)
    {
        if (should_log(depth)) log(depth, "compare", "malformed comparison: " + format(condition));
        refuse_condition(condition, "\"" + name + "\" compares two terms -- write one on each side, as in \"A " + name + " B\".");
        return;
    }
    const Node rhs_pattern = *structure.objects.begin();

    // A term that no prior condition has bound, while the plan placed the
    // comparison after every condition capable of binding it: the rule's
    // shape, not this binding, leaves it open, so the verdict remains
    // identical across all bindings and is issued just once.
    const bool lhs_open = term_open(this, lhs_pattern, *rule.variables);
    const bool rhs_open = !lhs_open && term_open(this, rhs_pattern, *rule.variables);
    if (lhs_open || rhs_open)
    {
        if (should_log(depth)) log(depth, "compare", "comparison with an open term refused: " + format(condition));
        refuse_condition(condition,
                         "\"" + name + "\" compares two bound terms and binds neither -- no condition binds \""
                             + format(lhs_open ? lhs_pattern : rhs_pattern)
                             + "\". Bind it with a positive condition.");
        return;
    }

    const NumberVocabulary vocabulary = number_vocabulary();
    if (!vocabulary.has_digits()) report_no_digit_alphabet();

    const NumericOrder order = compare_terms(*this, vocabulary, lhs_pattern, rhs_pattern, *rule.variables);
    const bool         holds = numeric_relation_holds(relation, order);

    if (should_log(depth))
        log(depth, "compare", format(condition) + (holds ? " HOLDS" : " FAILS"));

    if (!holds) return;

    if (rule.index + 1 < rule.conditions->size())
    {
        proceed_after_condition(rule, ctx, depth, rule.variables, rule.unequals, rule.confidence);
        return;
    }

    // Terminal: the one from the `!=` guard (reasoning_evaluate.cpp). A
    // guard that remains unresolved there never acted as a filter, and the
    // match is dropped; a prune collects what the conditions matched, the
    // comparison serving as a filter just as the guard does.
    if (guards_unresolved(*rule.variables, *rule.unequals)) return;

    if (!ctx.rule_deductions.empty())
    {
        try
        {
            deduce(*rule.variables, rule.node, depth, ctx, rule.confidence);
        }
        catch (const contradiction_error& error)
        {
            report_contradiction(error);
        }
    }
    else if (_prune_mode)
    {
        collect_prune_targets(ctx.current_condition, *rule.variables, rule.node);
    }
    else
    {
        answer(ctx.current_condition, rule.variables, rule.node);
    }
}

// A term that is not a number is not a suitable location for an error: a data
// check runs over whatever the graph holds. The missing alphabet is a different
// issue. It represents the state of the session, removes every number
// simultaneously, and a load induces it independently, discarding the alphabet
// while the file's rules remain active, causing a data check to read as "no
// violations". Reported once, just as a net that cannot be used is. It is a
// finding, because the first pass of a `?` line drops every other diagnostic:
// reported there as a plain one, it would count as said without having been
// read.
void Reasoning::report_no_digit_alphabet() const
{
    if (_quiet || _no_digit_alphabet_reported) return;
    _no_digit_alphabet_reported = true;
    diagnostic_finding("Numeric comparison: no digit alphabet is registered, so every comparison stays silent. Import an arithmetic module such as decimal-arithmetic (after a .load, the modules the network was built with, substrate first), or register digits with zelph/set-number-digits.", true);
}

void Reasoning::refresh_comparison_predicates()
{
    bool any = false;
    for (std::size_t i = 0; i < numeric_relation_count; ++i)
    {
        _comparison_preds[i] = get_node(numeric_relation_names[i], "zelph", true);
        any                  = any || _comparison_preds[i] != 0;
    }
    _comparison_any = any;
}
