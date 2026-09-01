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
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"

#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace zelph;

namespace zelph::console
{
    // A COLLECTION has an identity of its own and is built fresh by every
    // literal, so "@{a b}" inside a command pattern can only ever denote a
    // NEW container -- never the one the answer line came from. Pasting a
    // printed membership fact back into .explain or .prune-facts therefore
    // says "not asserted" / "Pruned 0" about data that is plainly there,
    // which reads as the engine contradicting its own output. The pattern is
    // not wrong and nothing can make the literal resolve; what was missing is
    // the sentence that says so, and the route that does work.
    void CommandExecutor::Impl::explain_collection_literal(const std::vector<std::string>& parts, const std::size_t first) const
    {
        bool has_collection = false;
        pattern_code(parts, first, &has_collection);
        if (!has_collection) return;

        _n->diagnostic("A collection literal @{...} builds a NEW container, so it cannot name an "
                       "existing one. Address the fact by its ID (.node without an argument reports "
                       "the last answer's node), or use a set constant {...}, whose identity IS its "
                       "members.",
                       true);
    }

    void CommandExecutor::Impl::cmd_explain(const std::vector<std::string>& cmd)
    {
        std::vector<std::string> parts(cmd.begin() + 1, cmd.end());

        const auto is_number = [](const std::string& s)
        { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };

        // Two readings of a trailing all-digit token, tried in this order:
        //
        //   (1) it is the max-depth argument -- the documented form,
        //       ".explain alice likes bob 5";
        //   (2) it belongs to the pattern -- which is the case whenever the
        //       fact's own object is a numeral, as in
        //       ".explain ((1 d+ 1) tci 0) sum 0". Reading (1) would steal
        //       the object there and leave a two-component statement that
        //       the AST builder cannot turn into a fact.
        //
        // (1) keeps precedence, so the documented form never changes
        // meaning; (2) only rescues arguments that (1) cannot resolve.
        std::size_t   depth  = 4;
        network::Node target = 0;

        if (!parts.empty() && is_number(parts.back()))
        {
            const std::vector<std::string> head(parts.begin(), parts.end() - 1);
            if (head.empty())
            {
                // Depth only: ".explain 3" explains the last output node.
                depth = std::stoul(parts.back());
                parts.clear();
            }
            else if ((target = resolve_explain_pattern(head)) != 0)
            {
                depth = std::stoul(parts.back());
            }
        }

        if (target == 0) target = resolve_explain_pattern(parts);

        if (target == 0)
        {
            if (!parts.empty())
            {
                // One message for four different situations is one message
                // too few. An argument that NAMES something -- an atom, a
                // core node, the contradiction the engine had just reported
                // with its premises -- is not a parse failure, and telling
                // the user it might be sends them to look at their typing.
                if (parts.size() == 1)
                {
                    if (const network::Node nd = resolve_node(parts[0], _n->lang()); nd != 0)
                    {
                        if (nd == _n->core.Contradiction)
                            throw std::runtime_error(".explain: '" + parts[0] + "' is the contradiction marker, not a fact. A contradiction "
                                                                                "materializes nothing, so there is no derivation left to reconstruct afterwards -- "
                                                                                "its premises are printed with the '⇐' line as it is derived, and .run-export records them.");

                        throw std::runtime_error(".explain: '" + parts[0] + "' is a node, not a fact. .explain reconstructs how a FACT was derived; "
                                                                            "pass the statement it takes part in, or use .node to inspect the node itself.");
                    }
                }

                throw std::runtime_error(".explain: cannot parse fact pattern, or it does not denote a fact");
            }

            target = string::last_node_to_string_node();
            if (!target)
                throw std::runtime_error(".explain: no previous output node -- pass a fact pattern");
        }

        if (!_n->check_fact(target).is_known())
        {
            _n->out("Fact is not asserted -- nothing to explain.", true);
            explain_collection_literal(parts);
            return;
        }

        std::set<network::Node> printed;
        std::string             out;
        render_proof(_n->explain(target, depth), "", true, printed, out);
        _n->out(out, true);
    }

    // Indented proof tree in the established "⇐" notation. Shared subproofs
    // (the DAG from hash-consing) are expanded once and referenced afterwards.
    void CommandExecutor::Impl::render_proof(const std::shared_ptr<network::ProofNode>& p, const std::string& indent, const bool last, std::set<network::Node>& printed, std::string& out) const
    {
        std::string line;
        zelph::string::node_to_string(_n, line, _n->lang(), p->fact, 3);
        line = zelph::string::unmark_identifiers(line);

        const std::string branch       = indent.empty() ? "" : indent + (last ? "└─ " : "├─ ");
        const std::string child_indent = indent.empty() ? "   " : indent + (last ? "   " : "│  ");

        switch (p->status)
        {
        case network::ProofNode::Status::Axiom:
            // A pattern some rule uses NEGATED is still an axiom when it was
            // asserted; the tag says how a rule reads it, not whether it
            // holds. It used to be written into the term above, which made
            // this line say the opposite of what it reports.
            out += branch + line
                 + (_n->check_fact(p->fact, _n->core.IsA, {_n->core.Negation}).is_known()
                        ? "  [axiom; negated by a rule]\n"
                        : "  [axiom]\n");
            return;
        case network::ProofNode::Status::RulePattern:
            // Not an axiom: the node exists because a rule was written with
            // this statement as a ground pattern, and nobody claimed it.
            out += branch + line + "  [rule pattern; not asserted]\n";
            return;
        case network::ProofNode::Status::Unfounded:
            // Not "the graph is broken": with a rule whose consequence has a
            // VARIABLE predicate -- the meta-rules zelph exists for -- that
            // consequence unifies with every fact there is, so a plainly
            // typed axiom lands here too. All the engine can say is that the
            // fact holds and that it found no derivation for it.
            out += branch + line + "  [asserted; no derivation found]\n";
            return;
        case network::ProofNode::Status::Truncated:
            out += branch + line + "  … [depth limit -- use '.explain <pattern> 0' for the full proof]\n";
            return;
        case network::ProofNode::Status::Derived:
            break;
        }

        if (printed.count(p->fact))
        {
            out += branch + line + "  [see above]\n";
            return;
        }
        printed.insert(p->fact);

        // Only the root carries this, and only when a second instantiation was
        // verified. Without it the tree reads as THE derivation of the fact,
        // which is a stronger claim than the search makes: it stops at the
        // first justification it can rebuild.
        out += branch + line + (p->more_justifications ? "  [one of several justifications]\n" : "\n");

        const std::size_t total = p->premises.size() + p->walked.size() + p->absent.size();
        std::size_t       index = 0;
        for (const auto& premise : p->premises)
            render_proof(premise, child_indent, ++index == total, printed, out);
        for (const network::Node path : p->walked)
        {
            // The stored node is the rule's tag fact, so node_to_string writes
            // the verbose "((C P279 T) closure one-or-more)" form -- the same
            // one .list-rules prints and the same one that re-enters as this
            // rule. The bindings turn it into the path that was actually
            // walked. [closure] and not [axiom]: nobody asserted the path, the
            // engine walked it, and a proof that claims otherwise is a
            // category error of exactly the kind a mathematical reader checks.
            std::string pline;
            zelph::string::node_to_string(_n, pline, _n->lang(), path, 3, p->bindings);
            pline = zelph::string::unmark_identifiers(pline);
            out += child_indent + (++index == total ? "└─ " : "├─ ") + pline + "  [closure]\n";
        }
        for (const network::Node neg : p->absent)
        {
            // The stored node is the rule's negation-tagged pattern, so
            // node_to_string writes the ¬(...) itself; passing the step's
            // bindings turns "¬(N hasdivisor D)" into the premise actually
            // checked, "¬(&7 hasdivisor D)". D stays a variable on purpose --
            // it is what "for no D" quantifies over.
            std::string nline;
            zelph::string::node_to_string(_n, nline, _n->lang(), neg, 3, p->bindings);
            nline = zelph::string::unmark_identifiers(nline);
            if (nline.rfind("¬", 0) != 0) nline = "¬(" + nline + ")";
            out += child_indent + (++index == total ? "└─ " : "├─ ") + nline + "  [absent]\n";
        }
    }
}
