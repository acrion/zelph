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

#include "script/command_executor_impl.hpp"

#include "network/reasoning.hpp"
#include "script/script_engine.hpp"
#include "string/string_utils.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace zelph;

// True if the text is wrapped in ONE pair of parentheses enclosing the
// whole string -- "(a done b)" but not "(a p b) mark ok". Quoted sections
// are skipped, so a predicate like "is (not) father of" cannot mislead it.
static bool is_fully_parenthesized(const std::string& s)
{
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') return false;

    int  depth    = 0;
    bool in_quote = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '"')
        {
            in_quote = !in_quote;
            continue;
        }
        if (in_quote) continue;

        if (s[i] == '(')
        {
            ++depth;
        }
        else if (s[i] == ')')
        {
            if (--depth == 0) return i + 1 == s.size();
        }
    }
    return false;
}

namespace zelph::console
{
    network::Node CommandExecutor::Impl::resolve_node(const std::string& arg, const std::string& lang) const
    {
        network::Node nd = _n->get_node(arg, lang);
        if (nd == 0)
        {
            try
            {
                size_t pos = 0;
                nd         = std::stoull(arg, &pos);
                if (pos != arg.length())
                    nd = 0;
                else if (!_n->exists(nd))
                    throw std::runtime_error("Node does not exist");
            }
            catch (...)
            {
                nd = 0;
            }
        }
        return nd;
    }

    network::Node CommandExecutor::Impl::resolve_single_node(const std::string& arg, bool prioritize_id) const
    {
        bool is_numeric = std::all_of(arg.begin(), arg.end(), ::iswdigit);

        if (is_numeric && prioritize_id)
        {
            try
            {
                size_t        pos = 0;
                network::Node nd  = std::stoull(arg, &pos);
                if (pos == arg.length() && _n->exists(nd)) return nd;
            }
            catch (...)
            {
            }
        }

        network::Node nd = _n->get_node(arg);
        if (nd != 0) return nd;

        if (is_numeric && !prioritize_id)
        {
            try
            {
                size_t        pos   = 0;
                network::Node nd_id = std::stoull(arg, &pos);
                if (pos == arg.length() && _n->exists(nd_id)) return nd_id;
            }
            catch (...)
            {
            }
        }

        throw std::runtime_error("Unknown node '" + arg + "'");
    }

    // parse_zelph_to_janet for a fact pattern: an unparsable pattern is a
    // normal outcome here (see cmd_explain's unwrapping), not an error to
    // propagate.
    // `why` keeps the FIRST refusal, because the caller may report it. A
    // failure here is not necessarily an error -- the parenthesis-unwrapping
    // retry below depends on being able to fail once -- so the reason is
    // carried rather than thrown, and used only when no attempt succeeds.
    std::string CommandExecutor::Impl::try_parse_pattern(const std::string& pattern, std::string* why) const
    {
        try
        {
            return _script_engine->parse_zelph_to_janet(pattern);
        }
        catch (const std::exception& e)
        {
            if (why && why->empty()) *why = e.what();
            return {};
        }
    }

    // Evaluating a statement MATERIALIZES it (the zelph AST calls
    // zelph/fact), which would make every pattern "asserted" and turn
    // .explain into an assertion command. The evaluation therefore runs
    // inside a scratch cluster that is rolled back immediately: nodes that
    // already existed are never recorded, so the drop removes exactly what
    // this evaluation added -- and nothing else. The returned node ID is a
    // structural hash and stays meaningful after the rollback, so
    // check_fact() can answer honestly.
    network::Node CommandExecutor::Impl::evaluate_pattern_read_only(const std::string& code)
    {
        static const std::string scratch  = "__explain";
        const std::string        previous = _n->active_cluster_name();

        _n->set_active_cluster(scratch);

        network::Node target = 0;
        try
        {
            target = _script_engine->evaluate_expression(code, /*quiet*/ true, /*resolving_pattern*/ true);
        }
        catch (...)
        {
            restore_cluster(previous);
            _n->drop_scratch_cluster(scratch);
            throw;
        }

        restore_cluster(previous);
        _n->drop_scratch_cluster(scratch);
        return target;
    }

    // Tokens -> the node the pattern denotes, or 0 if this reading does not
    // work out. Every failure mode is folded into 0 so that cmd_explain can
    // TRY a reading: parse failure, a statement the AST builder rejects
    // (too few components), and a pattern that denotes nothing.
    // Tokens -> janet code for the fact pattern they denote, or {} if no
    // reading works out. Shared by .explain and the prune commands: a
    // pattern one of them accepts has to mean the same to the other, and
    // .prune-* used to quote every non-variable token instead, which turned
    // ".prune-nodes (s4 rel X)" into a fact of the three literal names
    // "(s4", "rel" and "X)" -- no variable left, and the command then said
    // so and did nothing.
    std::string CommandExecutor::Impl::pattern_code(const std::vector<std::string>& parts, const std::size_t first, bool* has_collection, std::string* why) const
    {
        if (has_collection) *has_collection = false;
        if (parts.empty()) return {};

        // The quotes are stripped by the time a command sees its tokens, so
        // a token has to be RE-quoted to mean the name it named. Which ones
        // is recorded per token (`first` is where `parts` starts in the
        // command), because it cannot be recovered: `x>y` is a name the
        // parser would otherwise read as the three atoms `x > y`, and
        // everything structural -- a nested fact, a term island, ¬, an
        // &-literal -- has to stay verbatim to keep parsing.
        //
        // Without the record, whitespace is the only evidence left that a
        // token was quoted, which is what the Janet command handler falls
        // back to.
        std::string pattern;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            const std::string& p = parts[i];
            if (!pattern.empty()) pattern += ' ';

            const std::size_t index = first + i;
            if (index < _sources.size())
                pattern += _sources[index];
            else if (p.find_first_of(" \t") != std::string::npos)
                pattern += '"' + string::escape_atom(p) + '"';
            else
                pattern += p;
        }

        // Reported to the caller because it decides what a failure MEANS, not
        // whether the pattern parses -- see explain_collection_literal.
        if (has_collection) *has_collection = pattern.find("@{") != std::string::npos;

        // A pattern wrapped in a single pair of parentheses --
        // ".explain ((&6 + &7) = &13)" -- is a TERM, which the statement
        // grammar rejects; unwrapping yields the statement the user
        // meant. Tried SECOND, so a pattern that parses as given keeps
        // its original reading.
        std::string code = try_parse_pattern(pattern, why);
        if (code.empty() && is_fully_parenthesized(pattern))
            code = try_parse_pattern(pattern.substr(1, pattern.size() - 2), why);
        return code;
    }

    network::Node CommandExecutor::Impl::resolve_explain_pattern(const std::vector<std::string>& parts, const std::size_t first)
    {
        const std::string code = pattern_code(parts, first);
        if (code.empty()) return 0;

        try
        {
            return evaluate_pattern_read_only(code);
        }
        catch (const std::exception&)
        {
            return 0;
        }
    }

    // A node argument that may be a name, a numeric ID, or a printed FACT --
    // "a p b", or the parenthesised form the renderer uses for a nested one.
    // .explain and the prune commands have taken that third form all along,
    // while the exploration commands could not address a fact node at all
    // unless the user hunted down its numeric ID -- although the fact is
    // exactly what they had just seen printed, and printed output is meant to
    // read back as input.
    //
    // The fact reading is tried LAST, so a name that happens to parse as a
    // statement keeps its meaning.
    //
    // `count` carries the optional trailing number these commands take, with
    // the two readings .explain settled on and in its order: the documented
    // count wins, and a trailing numeral stays part of the pattern only when
    // the shorter reading resolves to nothing.
    network::Node CommandExecutor::Impl::resolve_node_or_fact(const std::vector<std::string>& parts, size_t* count)
    {
        const auto is_number = [](const std::string& s)
        { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };

        const auto resolve = [this](const std::vector<std::string>& p) -> network::Node
        {
            if (p.empty()) return 0;
            if (p.size() == 1)
            {
                if (const network::Node nd = resolve_node(p[0], _n->lang()); nd != 0) return nd;
            }

            // A pattern denotes a node whether or not the graph holds it --
            // the ID is the hash of the triple, so evaluating "q nosuchrel r"
            // yields a perfectly good number for a node that does not exist,
            // and the commands here would have printed it as "??". .explain
            // can report that state ("Fact is not asserted"); an exploration
            // command has nothing to show, so it has to say Unknown node.
            const network::Node nd = resolve_explain_pattern(p, 1);
            return nd != 0 && _n->exists(nd) ? nd : network::Node{0};
        };

        if (count != nullptr && parts.size() >= 2)
        {
            const std::vector<std::string> head(parts.begin(), parts.end() - 1);
            const network::Node            without_last = resolve(head);

            if (without_last != 0)
            {
                // The documented reading -- node plus trailing count -- wins
                // whenever the full argument list does not ALSO denote a
                // node. That is what keeps ".out a -1" reporting a malformed
                // count instead of degrading into "Unknown node", and it is
                // the only reading left for any trailing token that is not a
                // number at all.
                //
                // When both readings resolve -- a multi-object fact whose
                // last object is a numeral, next to the same fact one object
                // shorter -- the count keeps precedence, exactly as .explain
                // resolves the same ambiguity for its depth.
                if (resolve(parts) == 0 || is_number(parts.back()))
                {
                    *count = string::parse_count(parts.back());
                    return without_last;
                }
            }
        }

        return resolve(parts);
    }
}
