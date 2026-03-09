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

namespace zelph::network
{
    class contradiction_error final : public std::exception
    {
    public:
        // cppcheck-suppress passedByValue
        contradiction_error(const Node fact, const Variables& variables, const Node parent)
            : std::exception()
            , _fact(fact)
            , _variables(variables)
            , _parent(parent)
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

    private:
        Node      _fact;
        Variables _variables;
        Node      _parent;
    };
}
