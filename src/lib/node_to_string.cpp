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

#include "node_to_string.hpp"

#include "zelph.hpp"

#include <boost/algorithm/string.hpp>

#include <mutex>

namespace zelph::console
{
    int                  format_fact_level = 0;
    std::recursive_mutex mtx;
}

bool zelph::console::is_inside_node_to_wstring()
{
    return format_fact_level > 0;
}

void zelph::console::node_to_wstring(const zelph::network::Zelph* const z, std::wstring& result, const std::string& lang, network::Node fact, const int max_objects, const network::Variables& variables, network::Node parent, std::shared_ptr<std::unordered_set<network::Node>> history)
{
    // Formats a fact into a string representation.

    struct IncDec
    {
        explicit IncDec(int& n)
            : _n(n) { ++_n; }
        ~IncDec() { --_n; }
        int& _n;
    };

    std::lock_guard lock(mtx);

    IncDec incDec(format_fact_level);
#ifdef DEBUG_FORMAT_FACT
    std::string indent(format_fact_level * 2, ' ');
    diagnostic_stream() << indent << "[DEBUG node_to_wstring] ENTRY fact=" << fact << " parent=" << parent << std::endl;
#endif

    if (!history) history = std::make_shared<std::unordered_set<network::Node>>();

    // Helper to resolve variables
    auto resolve_var = [&](network::Node n) -> network::Node
    {
        int           limit = 0;
        network::Node curr  = n;
        while (network::Zelph::is_var(curr) && limit++ < 100)
        {
            auto it = variables.find(curr);
            if (it == variables.end() || it->second == 0 || it->second == curr) break;
            curr = it->second;
        }
        return curr;
    };

    // 1. Variable Substitution
    network::Node resolved = resolve_var(fact);

    if (history->find(resolved) != history->end())
    {
#ifdef DEBUG_FORMAT_FACT
        diagnostic_stream() << indent << "[DEBUG node_to_wstring] HIT HISTORY for fact=" << resolved << " -> returning '?'" << std::endl;
#endif
        result = L"?";
        return;
    }

    auto is_statement_node = [&](network::Node nd) -> bool
    {
        if (nd == 0 || !z->exists(nd)) return false;

        network::Node pred = z->parse_relation(nd);
        if (pred == 0) return false;

        // Cons cells are formatted earlier as lists; still a statement node structurally,
        // but we don't want to force "(...)" around list syntax.
        if (pred == z->core.Cons) return true;

        // Predicate must be a relation type
        if (!z->check_fact(pred, z->core.IsA, {z->core.RelationTypeCategory}).is_correct())
            return false;

        const auto& r = z->get_right(nd);
        const auto& l = z->get_left(nd);

        // Must actually link to its predicate
        if (r.count(pred) == 0) return false;

        // Must have at least one bidirectional neighbor besides the predicate (= subject candidate)
        for (network::Node x : r)
            if (x != pred && l.count(x) != 0)
                return true;

        return false;
    };

    const bool resolved_is_stmt = is_statement_node(resolved);

    // 2. Name Check
    // If the node has a direct name, use it.
    std::wstring name = z->get_formatted_name(resolved, lang);
    if (!name.empty())
    {
#ifdef DEBUG_FORMAT_FACT
        diagnostic_stream() << indent << "[DEBUG node_to_wstring] Found name '" << string::unicode::to_utf8(name) << "' for node " << resolved << std::endl;
#endif
        result = string::mark_identifier(name);
        return;
    }

    // 3. Cons List Detection (Sequence)
    // Check if 'resolved' is a cons cell (relation node whose predicate is Cons).
    // If so, walk the cons chain and format as < e1 e2 ... en >.
    if (z->exists(resolved))
    {
        network::Node rel_type = z->parse_relation(resolved);
        if (rel_type == z->core.Cons)
        {
#ifdef DEBUG_FORMAT_FACT
            diagnostic_stream() << indent << "[DEBUG node_to_wstring] DETECTED CONS LIST starting at " << resolved << std::endl;
#endif
            auto child_history = std::make_shared<std::unordered_set<network::Node>>(*history);
            child_history->insert(resolved);

            std::vector<network::Node>        list_elements;
            network::Node                     current = resolved;
            std::unordered_set<network::Node> visited_cells;

            while (current != 0 && current != z->core.Nil && z->exists(current))
            {
                if (visited_cells.count(current)) break; // cycle protection
                visited_cells.insert(current);

                if (z->parse_relation(current) != z->core.Cons) break; // not a cons cell

                network::adjacency_set objs;
                network::Node          car = z->parse_fact(current, objs, 0);
                if (car != 0) car = resolve_var(car);
                if (car != 0)
                    list_elements.push_back(car);

                // Get cdr (rest of list) — the single object of this cons cell
                network::Node cdr = z->core.Nil;
                for (network::Node o : objs)
                {
                    cdr = resolve_var(o);
                    break;
                }
                current = cdr;
            }

            if (!list_elements.empty())
            {
                // Check whether all elements are single-character named nodes (digit-like).
                bool all_single_char = std::all_of(list_elements.begin(), list_elements.end(), [&](network::Node e) -> bool
                                                   {
                        network::Node         eff = resolve_var(e);
                        std::wstring nm  = z->get_formatted_name(eff, lang);
                        return nm.length() == 1; });

                if (all_single_char)
                {
                    // Reverse for display: stored order is LSB-first (e.g. [3,2,1] for 123),
                    // conventional display is MSB-first. Omit spaces to match input syntax.
                    std::vector<network::Node> display_elements(list_elements.rbegin(), list_elements.rend());

                    result = L"<";
                    for (network::Node e : display_elements)
                    {
                        std::wstring elem_str;
                        console::node_to_wstring(z, elem_str, lang, resolve_var(e), max_objects, variables, resolved, child_history);
                        result += boost::algorithm::trim_copy_if(elem_str, boost::algorithm::is_any_of(L"«»"));
                    }
                    result += L">";
                }
                else
                {
                    result     = L"<";
                    bool first = true;
                    for (network::Node e : list_elements)
                    {
                        if (!first) result += L" ";
                        std::wstring elem_str;
                        console::node_to_wstring(z, elem_str, lang, e, max_objects, variables, resolved, child_history);

                        // Wrap in parentheses if it's a composite expression (not a simple name).
                        if (!elem_str.empty()
                            && elem_str.find(L' ') != std::wstring::npos
                            && elem_str.front() != L'('
                            && elem_str.front() != L'<'
                            && elem_str.front() != L'{')
                        {
                            network::Node eff_e = resolve_var(e);
                            if (elem_str != z->get_formatted_name(eff_e, lang))
                                elem_str = L"(" + elem_str + L")";
                        }

                        result += elem_str;
                        first = false;
                    }
                    result += L">";
                }
                return;
            }
        }
    }

    // 4. Container Detection (Set)
    // Check if 'resolved' acts as a container (Object in a PartOf relation).
    std::unordered_set<network::Node> elements;

    if (z->exists(resolved))
    {
        for (network::Node rel : z->get_right(resolved))
        {
            if (rel == parent) continue;

            network::Node p = z->parse_relation(rel);
            if (p == z->core.PartOf)
            {
                network::adjacency_set objs;
                network::Node          s = z->parse_fact(rel, objs, 0);
                if (s != 0) s = resolve_var(s);

                if (s != 0 && objs.count(resolved) > 0)
                {
                    elements.insert(s);
                }
            }
        }
    }

    if (!elements.empty())
    {
#ifdef DEBUG_FORMAT_FACT
        diagnostic_stream() << indent << "[DEBUG node_to_wstring] DETECTED SET with " << elements.size() << " elements." << std::endl;
#endif
        auto child_history = std::make_shared<std::unordered_set<network::Node>>(*history);
        child_history->insert(resolved);

        std::vector<network::Node> sorted_elements(elements.begin(), elements.end());
        std::sort(sorted_elements.begin(), sorted_elements.end());

        result     = L"{";
        bool first = true;
        for (network::Node e : sorted_elements)
        {
            if (!first) result += L" ";
            std::wstring elem_str;
            node_to_wstring(z, elem_str, lang, e, max_objects, variables, resolved, child_history);

            if (!elem_str.empty()
                && elem_str.find(L' ') != std::wstring::npos
                && elem_str.front() != L'('
                && elem_str.front() != L'<'
                && elem_str.front() != L'{')
            {
                network::Node eff_e = resolve_var(e);
                if (elem_str != z->get_formatted_name(eff_e, lang))
                {
                    elem_str = L"(" + elem_str + L")";
                }
            }

            result += elem_str;
            first = false;
        }
        result += L"}";

        return;
    }

    // 5. Proxy / Instance Detection
    // If a node is anonymous and not a container, check if it is an instance of a concept (IsA).
    // If so, display the concept instead of the structural fact "network::Node IsA Concept".
    bool is_negation = false;
    if (z->exists(resolved))
    {
        for (network::Node rel : z->get_right(resolved))
        {
            if (z->parse_relation(rel) == z->core.IsA)
            {
                network::adjacency_set type_objs;
                network::Node          type_subj = z->parse_fact(rel, type_objs, 0);

                // Ensure 'resolved' is the subject (The Instance)
                if (type_subj == resolved && !type_objs.empty())
                {
                    network::Node concept_node = *type_objs.begin();

                    // Avoid self-reference loops
                    if (concept_node != resolved)
                    {
                        if (concept_node == z->core.Negation)
                        {
                            is_negation = true;
                            continue; // Skip metadata, we will format it structurally below
                        }

#ifdef DEBUG_FORMAT_FACT
                        diagnostic_stream() << indent << "[DEBUG node_to_wstring] Found IsA proxy to concept=" << concept_node << std::endl;
#endif
                        console::node_to_wstring(z, result, lang, concept_node, max_objects, variables, parent, history);

                        if (!result.empty() && result != L"?")
                        {
#ifdef DEBUG_FORMAT_FACT
                            diagnostic_stream() << indent << "[DEBUG node_to_wstring] Proxy resolved to: " << string::unicode::to_utf8(result) << std::endl;
#endif
                            return;
                        }
                    }
                }
            }
        }
    }

    // 6. Standard Fact Formatting (S P O)
    // Only if it wasn't a container or a simple proxy do we treat it as a structural fact.

#ifdef DEBUG_FORMAT_FACT
    diagnostic_stream() << indent << "[DEBUG node_to_wstring] Standard path (Statement/Fact)." << std::endl;
#endif

    network::adjacency_set objects;
    network::Node          subject = z->parse_fact(resolved, objects, parent);

    bool is_condition = false;

#ifdef DEBUG_FORMAT_FACT
    diagnostic_stream() << indent << "[DEBUG node_to_wstring] z->parse_fact result: subject=" << subject << ", objects_count=" << objects.size() << ", is_condition=" << is_condition << std::endl;
#endif

    if (subject == 0 && !is_condition)
    {
        // z->parse_fact failed to identify the subject. This happens when the subject is itself
        // a hash node (e.g. a cons cell) whose outgoing edges contain structural predicates,
        // causing z->parse_fact's candidate filter to discard it. We fall back to direct graph
        // inspection: the subject is the unique node that is bidirectionally connected to
        // `resolved` (i.e. present in both get_right and get_left), which is the defining
        // invariant established by fact() via connect(subject, relation) + connect(relation, subject).
        network::Node fallback_pred = z->parse_relation(resolved);
        network::Node fallback_subj = 0;

        if (fallback_pred != 0)
        {
            for (network::Node nd : z->get_right(resolved))
            {
                // nd is the subject iff it is bidirectional with resolved (in both left and right)
                // and is not the predicate itself.
                if (nd != fallback_pred && z->has_left_edge(resolved, nd))
                {
                    fallback_subj = nd;
                    break;
                }
            }
        }

        if (fallback_subj == 0)
        {
#ifdef DEBUG_FORMAT_FACT
            diagnostic_stream() << indent << "[DEBUG node_to_wstring] INVALID: Subject is 0 after fallback. Returning '??'" << std::endl;
#endif
            result = string::mark_identifier(L"??");
            return;
        }

#ifdef DEBUG_FORMAT_FACT
        diagnostic_stream() << indent << "[DEBUG node_to_wstring] Fallback subject found: " << fallback_subj << std::endl;
#endif

        // Reconstruct objects: nodes in get_left(resolved) that are neither the subject
        // nor bidirectionally connected (predicates and parents appear in get_right too).
        subject = fallback_subj;
        objects.clear();
        for (network::Node nd : z->get_left(resolved))
        {
            if (nd != fallback_subj && !z->has_right_edge(resolved, nd))
            {
                objects.insert(nd);
            }
        }
        if (objects.empty())
        {
            // Self-referential fact: subject is its own object.
            objects.insert(fallback_subj);
        }
    }

    auto child_history = std::make_shared<std::unordered_set<network::Node>>(*history);
    child_history->insert(resolved);

    std::wstring subject_name, relation_name;

    if (!is_condition || subject)
    {
        // Recursion for Subject
        std::wstring s_str;
        node_to_wstring(z, s_str, lang, subject, max_objects, variables, resolved, child_history);

        // Wrap subject only if it's a composite fact, not a named atom
        bool needs_parens = false;
        if (!s_str.empty()
            && s_str.find(L' ') != std::wstring::npos
            && s_str.front() != L'('
            && s_str.front() != L'<'
            && s_str.front() != L'{')
        {
            network::Node eff_subj = resolve_var(subject);
            std::wstring  raw_name = z->get_formatted_name(eff_subj, lang);
            // Compare the formatted string with the MARKED raw name
            if (s_str != string::mark_identifier(raw_name))
            {
                needs_parens = true;
            }
        }

        if (needs_parens)
            subject_name = L"(" + s_str + L")";
        else
            subject_name = s_str.empty() ? (is_condition ? L"" : string::mark_identifier(L"?")) : s_str;

        network::Node relation = z->parse_relation(resolved);
        // Recursion for Relation (usually just get name, but handle complex relations)
        // Here we can assume relations are mostly named or simple, preventing deep noise
        relation = resolve_var(relation);

        std::wstring raw_rel_name = z->get_formatted_name(relation, lang);
        if (!raw_rel_name.empty())
        {
            // Relation has a name -> mark it manually, as we didn't recurse
            relation_name = string::mark_identifier(raw_rel_name);
        }
        else
        {
            // Recurse -> returns marked string
            std::wstring r_str;
            node_to_wstring(z, r_str, lang, relation, max_objects, variables, resolved, child_history);
            relation_name = r_str.empty() ? string::mark_identifier(L"?") : r_str;

            // Wrap complex unnamed relations in parens too.
            // For consistency with subject/object, we usually assume relations are simple,
            // but if r_str has spaces and isn't a container, wrap it.
            if (relation_name.find(L' ') != std::wstring::npos
                && relation_name.front() != L'('
                && relation_name.front() != L'<'
                && relation_name.front() != L'{')
                relation_name = L"(" + relation_name + L")";
        }
    }

    std::wstring objects_name;

    if (objects.size() > max_objects)
    {
        objects_name = string::mark_identifier(L"(... " + std::to_wstring(objects.size()) + L" objects ...)");
    }
    else
    {
        for (network::Node object : objects)
        {
            std::wstring o_str;
            node_to_wstring(z, o_str, lang, object, max_objects, variables, resolved, child_history);

            // Wrap object only if it's a composite fact, not a named atom
            if (!o_str.empty()
                && o_str.find(L' ') != std::wstring::npos
                && o_str.front() != L'('
                && o_str.front() != L'<'
                && o_str.front() != L'{')
            {
                network::Node eff_obj  = resolve_var(object);
                std::wstring  raw_name = z->get_formatted_name(eff_obj, lang);
                // Compare formatted string with MARKED raw name
                if (o_str != string::mark_identifier(raw_name))
                {
                    o_str = L"(" + o_str + L")";
                }
            }

            if (o_str.empty()) o_str = string::mark_identifier(L"?");

            if (!objects_name.empty()) objects_name += L" ";
            objects_name += o_str;
        }
        if (objects_name.empty()) objects_name = string::mark_identifier(L"?");
    }

    // The components (subject_name, relation_name, objects_name) are already marked.
    result = subject_name + L" " + relation_name + L" " + objects_name;

    if (is_negation)
    {
        result = L"¬(" + result + L")";
    }
    // If this is a statement node used as a value inside another structure,
    // wrap the whole triple in parentheses to make it valid input syntax.
    else if (parent != 0 && resolved_is_stmt)
    {
        network::Node pred = z->parse_relation(resolved);
        if (pred != z->core.Cons) // lists are handled earlier; don't wrap "<...>"
            result = L"(" + result + L")";
    }

    boost::replace_all(result, L"\r\n", L" --- ");
    boost::replace_all(result, L"\n", L" --- ");
    boost::trim(result);
#ifdef DEBUG_FORMAT_FACT
    diagnostic_stream() << indent << "[DEBUG node_to_wstring] EXIT result='" << string::unicode::to_utf8(result) << "'" << std::endl;
#endif
}
