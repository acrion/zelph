/*
Copyright (c) 2025 acrion innovations GmbH
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

#include "utils.hpp"
#include "zelph.hpp"

#include <map>
#include <memory>
#include <unordered_set>

namespace zelph
{
    namespace network
    {
        class Unification
        {
        public:
            Unification(Zelph* n, Node condition, Node parent, const std::shared_ptr<Variables>& variables, const std::shared_ptr<Variables>& unequals);
            std::shared_ptr<Variables> Next();
            std::shared_ptr<Variables> Unequals();

        protected:
            Zelph* const                       _n;
            Node                               _parent;
            std::shared_ptr<Variables>         _variables;
            std::shared_ptr<Variables>         _unequals;
            std::unordered_set<Node>           _relation_list;
            Node                               _relation_variable{0};
            std::unordered_set<Node>::iterator _fact_index{decltype(_facts_of_current_relation->second)::iterator()};
            std::unordered_set<Node>::iterator _relation_index;
            Node                               _subject;
            std::unordered_set<Node>           _objects;

        private:
            bool                                               increment_fact_index();
            std::map<Node, std::unordered_set<Node>>::iterator _facts_of_current_relation;
            bool                                               _fact_index_initialized{false}; // required because condition (_fact_index == decltype(_facts_of_current_relation->second)::iterator()) causes _DEBUG_ERROR("map/set iterators incompatible") if false
        };
    }
}
