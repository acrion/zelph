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

#include <chrono>
#include <iostream>
#include <unordered_set>

using namespace zelph::network;

Unification::Unification(Zelph* n, Node condition, Node parent, const std::shared_ptr<Variables>& variables, const std::shared_ptr<Variables>& unequals, ThreadPool* pool)
    : _n(n), _parent(parent), _variables(variables), _unequals(unequals), _pool(pool)
{
    // "condition" means here one of the predicates that form a list of conditions (which are connected by "and")

    adjacency_set relations = _n->filter(condition, _n->core.IsA, _n->core.RelationTypeCategory);

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

    if (_relation_variable != 0)
    {
        auto it = _variables->find(_relation_variable);
        if (it != _variables->end())
        {
            Node bound = it->second;
            if (_relation_list.count(bound))
            {
                _relation_list     = {bound};
                _relation_variable = 0;
            }
            else
            {
                _relation_list.clear();
            }
        }
    }

    if (_relation_list.empty()) return;

    // Always initialize sequential fallback
    _relation_index         = _relation_list.begin();
    _fact_index_initialized = false;

    static std::unordered_set<Node> logged_relations;

    // parallel only with fixed relation
    if (_pool && _relation_variable == 0)
    {
        Node fixed_rel = *_relation_list.begin();

        auto snap_start = std::chrono::steady_clock::now();

        adjacency_set snapshot;
        if (!_n->_pImpl->snapshot_left_of(fixed_rel, snapshot))
        {
            return;
        }

        auto   snap_end = std::chrono::steady_clock::now();
        double snap_ms  = std::chrono::duration<double, std::milli>(snap_end - snap_start).count();

        if (logged_relations.insert(fixed_rel).second)
        {
            std::clog << "Unification snapshot for relation " << fixed_rel
                      << " – size: " << snapshot.size()
                      << " (took " << snap_ms << " ms)";
            if (snapshot.size() > 0)
            {
                std::clog << " >>> PARALLEL ACTIVATED";
            }
            std::clog << std::endl;
        }

        if (snapshot.size() > 0)
        {
            _use_parallel = true;

            size_t threads    = std::thread::hardware_concurrency();
            size_t chunks     = std::min(threads * 4, snapshot.size());
            size_t chunk_size = snapshot.size() / chunks;

            _snapshot_vec.assign(snapshot.begin(), snapshot.end());

            _active_tasks = chunks;

            for (size_t c = 0; c < chunks; ++c)
            {
                size_t start = c * chunk_size;
                size_t end   = (c + 1 == chunks) ? _snapshot_vec.size() : (c + 1) * chunk_size;

                _pool->enqueue([this, fixed_rel, start, end]()
                               {
                                   for (size_t i = start; i < end; ++i)
                                   {
                                       Node fact = _snapshot_vec[i];

                                       if (_n->_pImpl->get_left(fact).count(fixed_rel) == 1) continue;

                                       adjacency_set objects;
                                       Node          subject = _n->parse_fact(fact, objects, fixed_rel);

                                       auto result = extract_bindings(subject, objects, fixed_rel);
                                       if (result)
                                       {

                                           std::lock_guard<std::mutex> l(_queue_mtx);
                                           _match_queue.push(std::move(result));
                                           _queue_cv.notify_one();
                                       }
                                   }

                                   {
                                       std::lock_guard<std::mutex> l(_queue_mtx);
                                       --_active_tasks;
                                       _queue_cv.notify_all();
                                   } });
            }
        }
        else
        {
            std::clog << "Sequential fallback for small snapshot (" << snapshot.size() << " facts)" << std::endl;
        }
    }
}

bool Unification::increment_fact_index()
{
    if (_relation_index == _relation_list.end())
    {
        return false;
    }

    do
    {
        if (!_fact_index_initialized)
        {
            if (!_n || !_n->_pImpl || !_n->_pImpl->snapshot_left_of(*_relation_index, _facts_snapshot))
            {
                return false; // there is a relation without any facts that use it (might happen if it has been explicitly defined via fact(r, core.IsA, core.RelationType))
            }
            _fact_index             = _facts_snapshot.begin(); // used to iterate over all facts that have relation type *_relation_index
            _fact_index_initialized = true;
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
    if (_relation_list.empty()) return nullptr;

    if (_use_parallel)
    {
        std::unique_lock<std::mutex> lock(_queue_mtx);
        _queue_cv.wait(lock, [this]
                       { return !_match_queue.empty() || _active_tasks == 0; });
        if (_match_queue.empty()) return nullptr;
        auto match = std::move(_match_queue.front());
        _match_queue.pop();
        return match;
    }
    else
    {
        if (_relation_variable == 0
            || utils::get(*_variables, _relation_variable, *_relation_index) == *_relation_index)
        {
            while (increment_fact_index()) // iterate over all matching facts (in snapshot)
            {
                adjacency_set objects;
                Node          subject = _n->parse_fact(*_fact_index, objects, *_relation_index);

                auto result = extract_bindings(subject, objects, *_relation_index);
                if (result)
                {
                    return result;
                }
            }
        }

        if (++_relation_index == _relation_list.end()) return nullptr;
        _fact_index_initialized = false;
        return Next();
    }
}

std::shared_ptr<Variables> Unification::extract_bindings(Node subject, const adjacency_set& objects, Node current_relation) const
{
    if (objects.size() > 0
        && subject != 0
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
        auto result = std::make_shared<Variables>();
        if (_n->_pImpl->is_var(_subject)) (*result)[_subject] = subject;
        if (_n->_pImpl->is_var(*_objects.begin()) && _variables->count(*_objects.begin()) == 0) (*result)[*_objects.begin()] = *objects.begin();
        if (_relation_variable != 0 && _variables->count(_relation_variable) == 0) (*result)[_relation_variable] = current_relation;
        return result;
    }
    return nullptr;
}

std::shared_ptr<Variables> Unification::Unequals()
{
    return _unequals;
}
