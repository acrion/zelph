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

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace zelph;

namespace
{
    using ProofSet = std::set<const network::ProofNode*>;

    // Proofs ending at a leaf situated within a cycle
    // (ProofNode::cycle_leaf), located somewhere beneath. A walk edge bearing
    // such a proof appears printed beneath its [closure] line (refer to
    // ProofNode::walk_edges); one lacking such a proof is not. Walked using a
    // stack of its own, where a proof's children are gathered exactly once
    // upon entry: re-collecting them every time the proof returned to the top
    // of the stack incurred a cost proportional to the square of
    // their number, and a walk traverses as many facts as the length of its
    // path. A walk over 40 000 derived facts required twice the time to print
    // compared to searching, where the tree printed four lines.
    ProofSet cyclic_proofs(const std::shared_ptr<network::ProofNode>& root)
    {
        struct Frame
        {
            const network::ProofNode*              p;
            std::vector<const network::ProofNode*> below;
            std::size_t                            next{0};
        };
        ProofSet                            cyclic;
        std::set<const network::ProofNode*> done;
        std::vector<Frame>                  todo;
        const auto                          enter = [&](const network::ProofNode* p)
        {
            std::vector<const network::ProofNode*> below;
            for (const auto& q : p->premises)
                below.push_back(q.get());
            for (const auto& edges : p->walk_edges)
                for (const auto& q : edges)
                    below.push_back(q.get());
            todo.push_back({p, std::move(below)});
        };
        enter(root.get());
        while (!todo.empty())
        {
            Frame& v = todo.back();
            if (v.next < v.below.size())
            {
                const network::ProofNode* q = v.below[v.next++];
                if (done.count(q) == 0) enter(q);
                continue;
            }
            bool reaches = v.p->cycle_leaf;
            for (const network::ProofNode* q : v.below)
                reaches = reaches || cyclic.count(q) != 0;
            if (reaches) cyclic.insert(v.p);
            done.insert(v.p);
            todo.pop_back();
        }
        return cyclic;
    }

    // What a tree outputs beneath a proof: its premises positioned one level
    // lower, and two levels down, beneath the [closure] line of their walk,
    // the walk edges whose proofs end at a leaf that lies on a cycle.
    template <typename Visit>
    void for_each_child(const network::ProofNode* p, const ProofSet& cyclic, Visit&& visit)
    {
        for (const auto& q : p->premises)
            visit(q.get(), std::size_t{1});
        for (const auto& edges : p->walk_edges)
            for (const auto& q : edges)
                if (cyclic.count(q.get()) != 0) visit(q.get(), std::size_t{2});
    }

    // The topmost level in the tree where each proof is situated, over
    // the shared subproofs.
    std::map<const network::ProofNode*, std::size_t> shallowest_levels(const std::shared_ptr<network::ProofNode>& root, const ProofSet& cyclic)
    {
        std::map<const network::ProofNode*, std::size_t>              levels;
        std::map<std::size_t, std::vector<const network::ProofNode*>> waiting{{0, {root.get()}}};
        while (!waiting.empty())
        {
            auto [level, current] = std::move(*waiting.begin());
            waiting.erase(waiting.begin());
            for (const network::ProofNode* p : current)
            {
                if (!levels.emplace(p, level).second) continue;
                for_each_child(p, cyclic, [&](const network::ProofNode* q, const std::size_t down)
                               { if (levels.count(q) == 0) waiting[level + down].push_back(q); });
            }
        }
        return levels;
    }

    // The levels of the longest path through a proof: as deep as
    // its tree structure can be printed. Traversed using a dedicated
    // stack, to the same extent that it is deep.
    std::size_t proof_height(const std::shared_ptr<network::ProofNode>& root, const ProofSet& cyclic)
    {
        std::map<const network::ProofNode*, std::size_t>                                                                  height;
        std::vector<std::pair<const network::ProofNode*, std::vector<std::pair<const network::ProofNode*, std::size_t>>>> todo;
        const auto                                                                                                        push = [&](const network::ProofNode* p)
        {
            std::vector<std::pair<const network::ProofNode*, std::size_t>> below;
            for_each_child(p, cyclic, [&](const network::ProofNode* q, const std::size_t down)
                           { below.emplace_back(q, down); });
            todo.emplace_back(p, std::move(below));
        };
        push(root.get());
        std::vector<std::size_t> next{0};
        while (!todo.empty())
        {
            auto& [p, below] = todo.back();
            if (next.back() < below.size())
            {
                const network::ProofNode* q = below[next.back()++].first;
                if (height.count(q) == 0)
                {
                    push(q);
                    next.push_back(0);
                }
                continue;
            }
            std::size_t h = 0;
            for (const auto& [q, down] : below)
                h = std::max(h, height[q] + down);
            height[p] = h;
            todo.pop_back();
            next.pop_back();
        }
        return height[root.get()];
    }

    // A tree exceeding this depth is not printed: every line is indented
    // according to its level, causing the output to expand in proportion to
    // the square of the depth -- a chain of 100 000 steps would print
    // 15 GB.
    constexpr std::size_t printable_levels = 5000;
}

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
        // (1) keeps precedence, thus the documented form consistently
        // preserves its meaning, except in cases where the shorter
        // pattern fails to reference a fact that the graph holds and the
        // whole argument references one: ".explain y val 7 3"
        // regarding the asserted (y val 7 3) answered "Fact is not asserted",
        // because the shorter pattern parses and refers to the absent
        // (y val 7).
        std::size_t   depth  = 4;
        network::Node target = 0;

        const auto max_depth = [](const std::string& digits) -> std::size_t
        {
            try
            {
                return std::stoul(digits);
            }
            catch (const std::out_of_range&)
            {
                throw std::runtime_error(".explain: max-depth too large: " + digits);
            }
        };

        if (!parts.empty() && is_number(parts.back()))
        {
            const std::vector<std::string> head(parts.begin(), parts.end() - 1);
            if (head.empty())
            {
                // Depth only: ".explain 3" explains the last output node.
                depth = max_depth(parts.back());
                parts.clear();
            }
            else if (const network::Node shorter = resolve_explain_pattern(head); shorter != 0)
            {
                const network::Node whole = _n->check_fact(shorter).is_known() ? network::Node{0} : resolve_explain_pattern(parts);
                if (whole != 0 && _n->check_fact(whole).is_known())
                {
                    target = whole;
                }
                else
                {
                    target = shorter;
                    depth  = max_depth(parts.back());
                }
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
                throw std::runtime_error(
                    ".explain: no previous output node -- pass a fact pattern. The fallback is "
                    "the node of the last statement, answer or deduction PRINTED, and a module "
                    "loaded with .import prints none.");
            if (!_n->exists(target))
                throw std::runtime_error(
                    ".explain: the node printed last no longer exists -- a cluster drop or a removal "
                    "took it. Pass a fact pattern.");
        }

        if (!_n->check_fact(target).is_known())
        {
            _n->out("Fact is not asserted -- nothing to explain.", true);
            explain_collection_literal(parts);
            return;
        }

        const auto     proof   = _n->explain(target, depth);
        const bool     limited = _n->last_explain_counts().limited;
        const ProofSet cyclic  = cyclic_proofs(proof);
        if (depth == 0 || depth > printable_levels)
        {
            // The height named is the proof's: the complete proof is what
            // explain() returns unless the search exhausted its budget, and
            // the proof found within the limit instead is cut at the limit,
            // so the complete version is at least as tall. Labelling the
            // limit as the height told a reader who had requested
            // 6000 levels that a proof of 20 000 was 6000 deep, and to give
            // a max-depth.
            const std::size_t longest = proof_height(proof, cyclic);
            if (const std::size_t printed = depth == 0 ? longest : std::min(longest, depth); printed > printable_levels)
            {
                _n->out("The proof is " + std::string(limited ? "at least " : "") + std::to_string(longest) + " levels deep, too deep to print as a tree. A max-depth of "
                            + std::to_string(printable_levels) + " or less prints its top levels: '.explain <pattern> 50'.",
                        true);
                string::set_last_node(target);
                return;
            }
        }

        // The tree beneath does not represent the top of the complete
        // proof: the search for a proof of the specified fact, with all
        // leaves being axioms, exhausted its budget (refer to
        // Reasoning::explain). It claimed the complete proof exceeded size
        // limits, which was inaccurate where a proof terminating at an
        // "[asserted; no derivation found]" leaf had been immediately
        // discovered.
        if (limited)
            _n->out("Note: the search for a proof whose leaves are all axioms ran out of its budget; this tree was searched within the depth limit (see '.help .explain').", true);

        std::string                                            out;
        const std::map<const network::ProofNode*, std::size_t> shallowest = shallowest_levels(proof, cyclic);
        render_proof(proof, depth, shallowest, cyclic, out);
        _n->out(out, true);

        // The process of rendering the tree wrote every line's node as the
        // last one printed; the argument-less form explains that node:
        // following ".explain 3", a subsequent ".explain 0" explained the
        // tree's last line -- an axiom -- rather than the answer the reader
        // had inquired about. Since the tree pertains to its root, the root
        // becomes what the next call without an argument explains.
        string::set_last_node(target);
    }

    // Indented proof tree in the established "⇐" notation. Shared subproofs
    // (the DAG from hash-consing) are expanded once and referenced afterwards.
    //
    // The tree is cut at max-depth: a derived fact at that level is printed
    // but not expanded, and no content beneath it is printed. The facts a
    // walk traverses lie below its [closure] line, two levels beneath the
    // step, and are cut similarly. The search reuses a finished
    // subproof wherever its fact recurs, so without the cut, a subproof
    // found at a shallow depth would be printed past the limit where the
    // same fact appeared at a deeper level. A derived fact is expanded at
    // its initial appearance; when the limit cut that expansion, it is
    // expanded once again at the highest point in the tree where the fact
    // appears, ensuring the reader sees as much of its proof as the limit
    // allows anywhere. Every other occurrence says "[see above]".
    // Expanding it again at every occurrence higher than the last one
    // produced a depth-limited tree four times the length of the full proof.
    //
    // "[see above]" denotes an expansion of the identical proof. A tree
    // searched within the depth limit may contain multiple proofs for a
    // single fact, and a line referencing whichever of them was printed last
    // read as circular where that proof ran through the line's own
    // ancestors; a different proof of the fact is printed as what it is.
    //
    // The tree is traversed using an independent stack: a single call
    // frame for each level overflowed the thread's stack during a proof
    // involving a few thousand levels.
    void CommandExecutor::Impl::render_proof(const std::shared_ptr<network::ProofNode>& root, const std::size_t max_depth, const std::map<const network::ProofNode*, std::size_t>& shallowest, const std::set<const network::ProofNode*>& cyclic, std::string& out) const
    {
        // A fact appears again within a tree ("[see above]", an expansion
        // repeated at a more elevated level), and writing a lengthy
        // term is not cheap: each one is written only once.
        std::map<network::Node, std::string> written;
        const auto                           text_of = [&](const network::Node fact) -> const std::string&
        {
            auto [it, fresh] = written.try_emplace(fact);
            if (fresh)
            {
                zelph::string::node_to_string(_n, it->second, _n->lang(), fact, 3);
                it->second = zelph::string::unmark_identifiers(it->second);
            }
            return it->second;
        };

        // A single rendering of the tree re-expanding a cut proof at the
        // level `highest` names for it. The `dry` mode writes
        // nothing, recording only, within `levels`, the highest level at
        // which each proof is printed.
        const auto render = [&](const std::map<const network::ProofNode*, std::size_t>& highest, const bool dry, std::map<const network::ProofNode*, std::size_t>& levels)
        {
            std::map<const network::ProofNode*, ProofExpansion> printed;

            // What is being printed: the expansion of a derived fact -- the
            // indentation of its premises, the next line to print, and
            // whether the portion of its subtree that has been printed has
            // hit the depth limit -- or, without a proof, the walk edges
            // located beneath a [closure] line.
            struct Expansion
            {
                const network::ProofNode*              p{nullptr};
                std::vector<const network::ProofNode*> edges;
                std::string                            indent;
                std::size_t                            level{0};
                std::size_t                            next{0};
                bool                                   cut{false};
                bool                                   again{false};
            };
            std::vector<Expansion> open;

            // Prints the line associated with `p`. Returns true if `p` is
            // expanded, thereby adding it to `open`; otherwise, `cut`
            // says whether the line represents a subtree that the limit cut.
            const auto line_of = [&](const network::ProofNode* p, const std::string& indent, const bool last, const std::size_t level, bool& cut) -> bool
            {
                const auto [at, fresh] = levels.try_emplace(p, level);
                if (!fresh) at->second = std::min(at->second, level);

                const auto write = [&](const char* label)
                {
                    if (dry) return;
                    out += indent.empty() ? "" : indent + (last ? "└─ " : "├─ ");
                    out += text_of(p->fact);
                    out += label;
                };
                const std::string child_indent = indent.empty() ? "   " : indent + (last ? "   " : "│  ");
                cut                            = false;

                switch (p->status)
                {
                case network::ProofNode::Status::Axiom:
                    // A pattern some rule uses NEGATED is still an axiom when it
                    // was asserted; the tag says how a rule reads it, not whether
                    // it holds. It used to be written into the term above, which
                    // made this line say the opposite of what it reports.
                    if (!dry)
                        write(_n->check_fact(p->fact, _n->core.IsA, {_n->core.Negation}).is_known()
                                  ? "  [axiom; negated by a rule]\n"
                                  : "  [axiom]\n");
                    return false;
                case network::ProofNode::Status::RulePattern:
                    // Not an axiom: the node exists because a rule was written
                    // with this statement as a ground pattern, and nobody claimed
                    // it.
                    write("  [rule pattern; not asserted]\n");
                    return false;
                case network::ProofNode::Status::RuleMentioned:
                    // Not an axiom either: a statement mentions this rule,
                    // hence it is not in force. It could have been typed as
                    // well, and the label does not say that nobody asserted it.
                    write("  [rule mentioned; not in force]\n");
                    return false;
                case network::ProofNode::Status::Unfounded:
                    // Not "the graph is broken": when a rule's consequence
                    // features a VARIABLE predicate -- the meta-rules zelph
                    // exists for -- that consequence unifies with every existing
                    // fact, meaning a plainly typed axiom is also included here,
                    // as is a derived fact whose premise was removed or whose
                    // negated premise became true at a later stage. The engine
                    // can only state that the fact holds and that no derivation
                    // was discovered for it, or none that does not retrace the
                    // fact's own cycle.
                    write("  [asserted; no derivation found]\n");
                    return false;
                case network::ProofNode::Status::Truncated:
                    write("  … [depth limit -- use '.explain <pattern> 0' for the full proof]\n");
                    cut = true;
                    return false;
                case network::ProofNode::Status::Derived:
                    break;
                }

                const auto earlier = printed.find(p);
                if (earlier != printed.end())
                {
                    const auto top    = highest.find(p);
                    const bool expand = earlier->second.cut && earlier->second.level > level && !earlier->second.again
                                     && top != highest.end() && top->second == level;
                    if (!expand)
                    {
                        write("  [see above]\n");
                        cut = earlier->second.cut;
                        return false;
                    }
                }

                if (max_depth != 0 && level >= max_depth)
                {
                    write("  … [depth limit -- use '.explain <pattern> 0' for the full proof]\n");
                    cut = true;
                    return false;
                }

                // Only the root carries this attribute, and solely when the search
                // uncovers an additional instantiation that holds
                // independently of the fact. Absent this, the tree is interpreted
                // as THE derivation of the fact, a stronger claim than the search
                // makes: it stops at the first justification it can rebuild.
                write(p->more_justifications ? "  [one of several justifications]\n" : "\n");
                open.push_back({p, {}, child_indent, level, 0, false, earlier != printed.end()});
                return true;
            };

            bool cut = false;
            line_of(root.get(), "", true, 0, cut);
            while (!open.empty())
            {
                Expansion& e = open.back();
                if (e.p == nullptr)
                {
                    if (e.next < e.edges.size())
                    {
                        const std::size_t               k      = e.next++;
                        const network::ProofNode* const edge   = e.edges[k];
                        const std::string               indent = e.indent;
                        const std::size_t               level  = e.level + 1;
                        bool                            below  = false;
                        if (!line_of(edge, indent, k + 1 == e.edges.size(), level, below)) e.cut = below || e.cut;
                        continue;
                    }
                    const bool reached_cut = e.cut;
                    open.pop_back();
                    open.back().cut = reached_cut || open.back().cut;
                    continue;
                }

                const network::ProofNode* const p        = e.p;
                const std::size_t               premises = p->premises.size();
                const std::size_t               total    = premises + p->walked.size() + p->absent.size();
                if (e.next < premises)
                {
                    const std::size_t               k       = e.next++;
                    const network::ProofNode* const premise = p->premises[k].get();
                    const std::string               indent  = e.indent;
                    const std::size_t               level   = e.level + 1;
                    bool                            below   = false;
                    if (!line_of(premise, indent, k + 1 == total, level, below)) e.cut = below || e.cut;
                    continue;
                }
                if (e.next < premises + p->walked.size())
                {
                    // The stored node is the rule's tag fact, so node_to_string
                    // writes the verbose "((C P279 T) closure one-or-more)" form
                    // -- the same one .list-rules prints and the same one that
                    // re-enters as this rule. The bindings turn it into the path
                    // that was actually walked. [closure] and not [axiom]: nobody
                    // asserted the path, the engine walked it, and a proof that
                    // claims otherwise is a category error of exactly the kind a
                    // mathematical reader checks.
                    const std::size_t w    = e.next - premises;
                    const bool        last = ++e.next == total;
                    if (!dry)
                    {
                        std::string pline;
                        zelph::string::node_to_string(_n, pline, _n->lang(), p->walked[w], 3, p->bindings);
                        pline = zelph::string::unmark_identifiers(pline);
                        out += e.indent + (last ? "└─ " : "├─ ") + pline + "  [closure]\n";
                    }
                    std::vector<const network::ProofNode*> shown;
                    if (w < p->walk_edges.size())
                        for (const auto& q : p->walk_edges[w])
                            if (cyclic.count(q.get()) != 0) shown.push_back(q.get());
                    if (shown.empty()) continue;

                    // The edges are positioned two levels beneath the step.
                    // Below the limit, they are not printed, and the step
                    // is treated as cut, similar to one whose premise
                    // stands at the limit. The group was opened
                    // regardless of the level, and the edges of a [closure]
                    // line at the limit were printed beneath it.
                    if (max_depth != 0 && e.level + 2 > max_depth)
                        e.cut = true;
                    else
                        open.push_back({nullptr, std::move(shown), e.indent + (last ? "   " : "│  "), e.level + 1, 0, false, false});
                    continue;
                }
                if (e.next < total)
                {
                    // The stored node is the rule's negation-tagged pattern, so
                    // node_to_string writes the ¬(...) itself; passing the step's
                    // bindings turns "¬(N hasdivisor D)" into the premise actually
                    // checked, "¬(&7 hasdivisor D)". D stays a variable on purpose
                    // -- it is what "for no D" quantifies over.
                    const network::Node neg  = p->absent[e.next - premises - p->walked.size()];
                    const bool          last = ++e.next == total;
                    if (!dry)
                    {
                        std::string nline;
                        zelph::string::node_to_string(_n, nline, _n->lang(), neg, 3, p->bindings);
                        nline = zelph::string::unmark_identifiers(nline);
                        if (nline.rfind("¬", 0) != 0) nline = "¬(" + nline + ")";
                        out += e.indent + (last ? "└─ " : "├─ ") + nline + "  [absent]\n";
                    }
                    continue;
                }

                printed[p]             = {e.level, e.cut, e.again};
                const bool reached_cut = e.cut;
                open.pop_back();
                if (!open.empty()) open.back().cut = reached_cut || open.back().cut;
            }
        };

        // Where a proof stands highest is a question about the printed tree,
        // not about the proof itself: the shallowest position in the proof may
        // lie below a fact printed as "[see above]", below which nothing is
        // printed, and no line then stood at that level -- the fact was printed
        // higher up as "[see above]", pointing at a deeper expansion that the
        // limit had cut, and the fact was expanded again nowhere. Thus, the
        // tree is laid out without being written until the highest printed
        // level of each proof agrees with the one it was laid out by; expanding
        // a proof again prints more of the tree, which can bring another proof
        // higher. Without a limit, nothing is cut, and nothing is expanded
        // again.
        std::map<const network::ProofNode*, std::size_t> highest = shallowest;
        if (max_depth != 0)
        {
            for (int round = 0; round < 8; ++round)
            {
                std::map<const network::ProofNode*, std::size_t> levels;
                render(highest, true, levels);
                if (levels == highest) break;
                highest = std::move(levels);
            }
        }
        std::map<const network::ProofNode*, std::size_t> levels;
        render(highest, false, levels);
    }
}
