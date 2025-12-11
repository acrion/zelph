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

#include "unification.hpp"
#include "string_utils.hpp"
#include "zelph_impl.hpp"

using namespace zelph::network;

Unification::Unification(Zelph* n, Node condition, Node parent, const std::shared_ptr<Variables>& variables, const std::shared_ptr<Variables>& unequals)
    : _n(n)
    , _parent(parent)
    , _variables(variables)
    , _unequals(unequals)
{
    // "condition" means here one of the predicates that form a list of conditions (which are connected by "and")

    std::unordered_set<Node> relations = _n->filter(condition, _n->core.IsA, _n->core.RelationTypeCategory);

    if (relations.size() == 1) // more than one relation for given condition makes no sense. _relation_list is empty, so Next() won't return anything
    {
        Node relation = *relations.begin(); // there is always only 1 relation
        _subject      = _n->parse_fact(condition, _objects, _parent);

        if (_n->_pImpl->is_var(relation))
        {
            // the relation is a variable, so fill _relation_list with all possible relations (excluding variables)
            _relation_list     = _n->get_sources(_n->core.IsA, _n->core.RelationTypeCategory, true);
            _relation_variable = relation;
        }
        else
        {
            _relation_list.insert(relation); // leaving _relation_variable==0, which denotes that this condition specifies the relation (instead of using a variable for it)

            if (relation == _n->core.Unequal)
            {
                for (Node object : _objects)
                    (*_unequals)[_subject] = object;
            }
        }
    }

    _relation_index = _relation_list.begin();
}

bool Unification::increment_fact_index()
{
    do
    {
        if (!_fact_index_initialized)
        {
            _facts_of_current_relation = _n->_pImpl->find_left(*_relation_index);

            if (_facts_of_current_relation == _n->_pImpl->right_end())
                return false; // there is a relation without any facts that use it (might happen if it has been explicitly defined via fact(r, core.IsA, core.RelationType))
            else
            {
                _facts_snapshot         = _facts_of_current_relation->second;
                _fact_index             = _facts_snapshot.begin(); // used to iterate over all facts that have relation type *_relation_index
                _fact_index_initialized = true;
            }
        }
        else if (++_fact_index == _facts_snapshot.end()) // increment and return false if we reached the end, so _relation_index will be incremented
        {
            return false;
        }
    } while (_n->_pImpl->get_left(*_fact_index).count(*_relation_index) == 1); // skip nodes that represent not relations of type *_relation_index, but relations having *_relation_index as subject (using bidirectional connection to the subject)

    return true;
}

std::shared_ptr<Variables> Unification::Next()
{
    if (_relation_list.size() == 0 || (_relation_variable == 0 && *_relation_list.begin() == _n->core.Unequal))
        return nullptr;

    do
    {
        if (_relation_variable == 0
            || !_variables
            || utils::get(*_variables, _relation_variable, *_relation_index) == *_relation_index)
        {
            while (increment_fact_index()) // iterate over all matching facts (in snapshot)
            {
                std::unordered_set<Node> objects;
                Node                     subject = _n->parse_fact(*_fact_index, objects, *_relation_index);

                if (objects.size() > 0
                    && !_n->_pImpl->is_var(subject)
                    && !_n->_pImpl->is_var(*objects.begin())
                    && utils::get(*_variables, _subject, subject) == subject                            // either the variable is unbound, or it already points to subject
                    && utils::get(*_variables, *_objects.begin(), *objects.begin()) == *objects.begin() // todo: what if more than one?
                    && (_n->_pImpl->is_var(_subject)                                                    // either _subject is a variable, or it is identical to subject
                        || _subject == subject)
                    && (_n->_pImpl->is_var(*_objects.begin())    // either _object is variable, or it is identical to object
                        || *_objects.begin() == *objects.begin() // todo: what if more than one?
                        ))
                {
                    std::shared_ptr<Variables> result = std::make_shared<Variables>();

                    if (_variables->count(_subject) == 0 && _n->_pImpl->is_var(_subject)) (*result)[_subject] = subject;
                    if (_variables->count(*_objects.begin()) == 0 && _n->_pImpl->is_var(*_objects.begin())) (*result)[*_objects.begin()] = *objects.begin(); // todo: what if more than one?
                    if (_relation_variable != 0 && _variables->count(_relation_variable) == 0) (*result)[_relation_variable] = *_relation_index;

                    return result;
                }
            }
        }
        _fact_index_initialized = false;
    } while (++_relation_index != _relation_list.end());

    return nullptr;
}

std::shared_ptr<Variables> Unification::Unequals()
{
    return _unequals;
}