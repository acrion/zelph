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

#include "answer.hpp"

using namespace zelph::network;

Answer::Answer(State state, long double probability, Node relation)
    : state(state)
    , _probability(std::min(1.0L, std::max(0.0L, probability)))
    , _relation(relation)
{
    if (state == State::Known && (_probability > 0 || _relation == 0))
    {
        throw std::runtime_error("Answer is known, but relation node is not set");
    }
}

Answer::Answer(long double probability, Node relation)
    : state(State::Known)
    , _probability(std::min(1.0L, std::max(0.0L, probability)))
    , _relation(relation)
{
    if (state == State::Known && _probability > 0 && _relation == 0)
    {
        throw std::runtime_error("Answer is known, but relation node is not set");
    }
}

Answer::Answer(Node relation)
    : state(State::Unknown)
    , _relation(relation)
{
}

Node Answer::relation() const
{
    return _relation;
}

bool Answer::is_known() const
{
    return state == State::Known;
}

bool Answer::is_correct() const
{
    return state == State::Known && _probability > 0.5L;
}

bool Answer::is_wrong() const
{
    return state == State::Known && _probability < 0.5L;
}

bool Answer::is_impossible() const
{
    return state == State::Known && _probability == 0;
}
