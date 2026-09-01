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

#include "zelph.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace zelph::network;

void Zelph::set_number_digits(const std::vector<Node>& digits_ascending)
{
    std::shared_ptr<const std::unordered_map<Node, uint32_t>> table;

    if (!digits_ascending.empty())
    {
        if (digits_ascending.size() < 2)
            throw std::invalid_argument("set_number_digits: at least 2 digits are required (or none to disable)");

        auto map = std::make_shared<std::unordered_map<Node, uint32_t>>();
        for (size_t i = 0; i < digits_ascending.size(); ++i)
            (*map)[digits_ascending[i]] = static_cast<uint32_t>(i);

        if (map->size() != digits_ascending.size())
            throw std::invalid_argument("set_number_digits: duplicate digit nodes");

        table = std::move(map);
    }

    std::unique_lock lock(_smtx_number_digits);
    _number_digits = std::move(table);
}

std::shared_ptr<const std::unordered_map<Node, uint32_t>> Zelph::number_digit_values() const
{
    std::shared_lock lock(_smtx_number_digits);
    return _number_digits;
}

std::shared_ptr<const DisplayTables> Zelph::display_tables() const
{
    std::shared_lock lock(_smtx_display_tables);
    return _display_tables;
}

bool Zelph::find_display_scheme(const std::string& name, std::size_t& index) const
{
    const auto tables = display_tables();
    if (!tables) return false;

    for (std::size_t i = 0; i < tables->schemes.size(); ++i)
    {
        if (tables->schemes[i].name == name)
        {
            index = i;
            return true;
        }
    }
    return false;
}

std::size_t Zelph::register_display_scheme(const DisplayScheme& scheme)
{
    if (scheme.name.empty())
        throw std::invalid_argument("register_display_scheme(): the scheme name must not be empty");

    std::unique_lock lock(_smtx_display_tables);

    // Copy-on-write: readers hold an immutable snapshot for the duration of
    // a rendering and are never disturbed by a concurrent registration.
    auto tables = _display_tables
                    ? std::make_shared<DisplayTables>(*_display_tables)
                    : std::make_shared<DisplayTables>();

    for (std::size_t i = 0; i < tables->schemes.size(); ++i)
    {
        if (tables->schemes[i].name == scheme.name)
        {
            // Update in place: operator entries reference schemes by index.
            tables->schemes[i] = scheme;
            _display_tables    = tables;
            return i;
        }
    }

    tables->schemes.push_back(scheme);
    _display_tables = tables;
    return tables->schemes.size() - 1;
}

void Zelph::register_operator_display(const std::size_t scheme, const std::vector<std::pair<Node, OperatorDisplay>>& entries)
{
    std::vector<Node> preds;
    preds.reserve(entries.size());

    {
        std::unique_lock lock(_smtx_display_tables);

        if (!_display_tables || scheme >= _display_tables->schemes.size())
            throw std::invalid_argument("display scheme: unknown scheme index");

        auto tables = std::make_shared<DisplayTables>(*_display_tables);

        for (const auto& [predicate, display] : entries)
        {
            if (predicate == 0)
                throw std::invalid_argument("display scheme: predicate must not be 0");

            const auto it = tables->operators.find(predicate);
            if (it != tables->operators.end())
            {
                throw std::runtime_error("display scheme: predicate '"
                                         + get_name(predicate, _lang, true)
                                         + "' is already registered in scheme '"
                                         + tables->schemes[it->second.scheme].name + "'");
            }

            OperatorDisplay entry = display;
            entry.scheme          = scheme;
            tables->operators.emplace(predicate, entry);
            preds.push_back(predicate);
        }

        _display_tables = tables;
    }

    // The self-fact sugar would render (X op X) as ":op X", which no
    // scheme's parser can read back.
    add_verbose_selffact_predicates(preds);
}

void Zelph::set_infix_display(const std::size_t scheme, const std::vector<InfixEntry>& operators)
{
    std::vector<std::pair<Node, OperatorDisplay>> entries;
    entries.reserve(operators.size());
    for (const InfixEntry& op : operators)
        entries.emplace_back(op.predicate,
                             OperatorDisplay{scheme, OperatorDisplay::Form::Infix, op.precedence, op.assoc});
    register_operator_display(scheme, entries);
}

void Zelph::set_application_display(const std::size_t scheme, const std::vector<Node>& predicates)
{
    std::vector<std::pair<Node, OperatorDisplay>> entries;
    entries.reserve(predicates.size());
    for (const Node p : predicates)
        entries.emplace_back(p, OperatorDisplay{scheme, OperatorDisplay::Form::Application, 0, -1});
    register_operator_display(scheme, entries);
}

// Register predicates whose self-facts must always render in the verbose
// "S P S" form instead of the ":pred S" display sugar. Deliberately
// ADDITIVE across calls (unlike the replace-the-set semantics of
// set_number_digits): modules stack (arithmetic -> symbolic-core -> eml),
// and a later module must not clobber an earlier module's registrations.
// Display-only session state, cleared by .reset and not persisted --
// like the digit alphabet, the graph topology is unaffected.
void Zelph::add_verbose_selffact_predicates(const std::vector<Node>& preds)
{
    std::unique_lock lock(_smtx_verbose_selffact_preds);
    _verbose_selffact_preds.insert(preds.begin(), preds.end());
}

// True if self-facts on this predicate must not use the ":pred S" display
// sugar. Queried by node_to_string for every self-fact candidate; the
// shared lock keeps concurrent formatting cheap.
bool Zelph::selffact_sugar_suppressed(const Node pred) const
{
    std::shared_lock lock(_smtx_verbose_selffact_preds);
    return _verbose_selffact_preds.contains(pred);
}
