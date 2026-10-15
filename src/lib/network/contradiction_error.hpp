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

#pragma once

#include "network_types.hpp"

#include <exception>
#include <string>

namespace zelph::network
{
    class contradiction_error final : public std::exception
    {
    public:
        // cppcheck-suppress passedByValue
        contradiction_error(const Node fact, const Variables& variables, const Node parent, const Node rule, const Node conclusion, std::string reason = {}, const Node claimed = 0)
            : std::exception()
            , _fact(fact)
            , _variables(variables)
            , _parent(parent)
            , _rule(rule)
            , _conclusion(conclusion)
            , _reason(std::move(reason))
            , _claimed(claimed)
        {
        }

        Node get_fact() const
        {
            return _fact;
        }

        const Variables& get_variables() const
        {
            return _variables;
        }

        Node get_parent() const
        {
            return _parent;
        }

        // The `=>` node associated with the rule whose firing met the
        // contradiction. The parent might instead be the condition set of
        // that rule (see deduce).
        Node get_rule() const
        {
            return _rule;
        }

        // What the consequence would have asserted: core `!` in case of a
        // denial, the refuted fact for a consequence that the graph
        // identifies as incorrect, the claimed fact for a derived refutation
        // that met it, 0 if the engine refused to build the fact (the reason
        // says why).
        Node get_conclusion() const
        {
            return _conclusion;
        }

        // Empty for a genuine contradiction -- a `!` consequence, or a
        // deduction the graph already knows to be wrong. Non-empty when the
        // engine REFUSED to build the deduced fact, in which case the reason
        // is the only thing that tells the user what to write instead.
        const std::string& get_reason() const
        {
            return _reason;
        }

        // The claimed fact that a refutation obtained through derivation
        // met, or 0: a member of the record beside the facts the conditions
        // matched.
        Node get_claimed() const
        {
            return _claimed;
        }

    private:
        Node        _fact;
        Variables   _variables;
        Node        _parent;
        Node        _rule;
        Node        _conclusion;
        std::string _reason;
        Node        _claimed;
    };
}
