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

#include <zelph_export.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>

namespace zelph
{
    namespace network
    {
        struct ZELPH_EXPORT Answer
        {
        protected:
            enum class State
            {
                Known,
                Unknown
            } state{State::Unknown};

            long double _probability{1};
            Node        _relation{0};

        public:
            Answer(State state, long double probability, Node relation = 0);
            Answer(long double probability, Node relation);
            explicit Answer(Node relation);

            Node relation() const;
            bool is_known() const;
            bool is_correct() const;
            bool is_wrong() const;
            bool is_impossible() const;
        };
    }
}
