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

#include "container_reading.hpp"

#include "fact_structure.hpp"
#include "unification.hpp"

#include <unordered_set>

namespace zelph::network
{
    std::vector<Node> container_members(const Zelph* const z, const Node n)
    {
        std::vector<Node> members;
        for (const Node rel : z->get_right(n))
        {
            if (z->parse_relation(rel) != z->core.PartOf) continue;
            adjacency_set objs;
            const Node    s = z->parse_fact(rel, objs, 0);
            if (s != 0 && objs.count(n) == 1) members.push_back(s);
        }
        return members;
    }

    namespace
    {
        // Whether `set`, the subject of `rule`, also stands in a consequence of
        // the rule or within `member`, the single member it holds. The walk
        // follows a fact's parts and the members of every container of the
        // rule's text (not of a value: a data collection holds no text of the
        // rule), each node exactly once. A set that a consequence holds is one
        // a firing writes facts about, and it must meet the other rule's set
        // both there and at the subject; read as its member, it left the
        // comparison at the subject, and a rule over two sets was taken for the
        // rule over one.
        bool stands_elsewhere(const Zelph* const z, const Node set, const Node rule, const Node member)
        {
            const FactStructure fs = get_preferred_structure(z, rule, 3);
            std::vector<Node>   pending(fs.objects.begin(), fs.objects.end());
            pending.push_back(member);
            std::unordered_set<Node> seen;
            while (!pending.empty())
            {
                const Node x = pending.back();
                pending.pop_back();
                if (x == set) return true;
                if (x == 0 || Zelph::is_var(x) || !seen.insert(x).second) continue;
                if (Zelph::is_hash(x))
                {
                    const FactStructure parts = get_preferred_structure(z, x, 3);
                    if (parts.subject != 0 && parts.predicate != 0)
                    {
                        pending.push_back(parts.subject);
                        pending.push_back(parts.predicate);
                        pending.insert(pending.end(), parts.objects.begin(), parts.objects.end());
                        continue;
                    }
                }
                switch (read_container(z, x, ReadAt::Term).reading)
                {
                case Reading::ConditionsInTerm:
                case Reading::OwnCollection:
                case Reading::SetConstant:
                {
                    const std::vector<Node> members = container_members(z, x);
                    pending.insert(pending.end(), members.begin(), members.end());
                    break;
                }
                default:
                    break;
                }
            }
            return false;
        }
    }

    ContainerReading detail::read_own_subject(const Zelph* const z, const Node n, const Node rule)
    {
        // A statement, the common subject, incurs a single lookup.
        if (rule == 0 || z->predicate_of(n) != 0) return {};

        adjacency_set members;
        if (!z->condition_set_members(n, members) || members.size() != 1) return {};

        const Node member = *members.begin();
        if (!member_is_rule_text(z, member) || stands_elsewhere(z, n, rule, member)) return {};
        return {Reading::Condition, false, member};
    }
}
