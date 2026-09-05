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

// What a malformed statement is TOLD, separated from the code that builds a
// well-formed one.
//
// The code generator walks the PEG's own syntax tree, so it knows two things
// no subsequent phase possesses: what the user wrote, token by token, and the
// ROLE the fragment plays -- a condition of a comma list, a rule's
// consequence, a term in subject or in object position. After the tree has
// been turned into a Janet call, both are gone, and what the user got instead
// was the calling convention: "arity mismatch, expected at least 3, got 2".
//
// So the split is between the two things that decide such a message. The place
// is what the caller passes in, because only the caller is standing there. The
// sentence is written once, here, because it is one mistake however many
// surface forms reach it -- and a sentence per call site is how a message set
// drifts apart.
//
// Anything else the generator refuses belongs here as it is touched next; the
// refusals still living in script_engine.cpp are not moved wholesale, since a
// message is worth relocating when it is being worked on, not before.

#include <janet.h>

#include <string>
#include <vector>

namespace zelph::script
{
    /// The surface text of one PEG-AST argument -- the token as it was typed.
    /// A structured argument has no single token and answers "(...)".
    std::string arg_text(Janet arg);

    /// How many values a `(...)` group holds, or -1 when `arg` is not one.
    int nested_value_count(Janet arg);

    /// Refuse a statement of fewer than three parts. `role` names the place it
    /// stands in ("condition 2 of the comma list", "the consequence"), and is
    /// empty where the surrounding form adds nothing to the message.
    [[noreturn]] void refuse_short_statement(const std::string& role, const std::vector<Janet>& args);

    /// What to append to "Syntax error: Could not parse statement." when the
    /// text alone says what went wrong. Empty when it does not.
    ///
    /// This is a second KIND of entry in this file. Everything above works on
    /// the PEG's syntax tree, which exists because the parse succeeded and
    /// only the shape was refused. A parse that FAILS leaves no tree, no
    /// position and no expectation -- a PEG has no error productions -- so
    /// the only evidence left is the text the user typed, and the message has
    /// to be reconstructed from it.
    ///
    /// One rule accounts for most of what people actually type: `:stmt-any`
    /// separates two values by `:s+`, so a value glued to an opening
    /// parenthesis is not a statement. `f(x)` is the shape every reader of
    /// mathematics writes, and in zelph it is spelled with a space -- or
    /// inside a notation island such as the standard library's `$( … )`.
    /// The one report this was written for reached it through `%`, which is a
    /// LINE escape and can never be a term; that case gets its own sentence,
    /// because the answer to it is a different one (the unquote `,name`).
    std::string diagnose_unparsable(const std::string& statement);
}
