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

#include <doctest/doctest.h>

#include "test_helpers.hpp"

#include "network/reasoning.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef __GLIBC__
    #include <malloc.h>
#endif

using namespace zelph::test;

namespace
{
    // The tree the last .explain output, excluding the trailing
    // newline.
    std::string explained(const zelph::io::OutputCollector& collector)
    {
        std::string tree = last_out_text(collector);
        while (!tree.empty() && tree.back() == '\n')
            tree.pop_back();
        return tree;
    }

    // A single line within a printed proof tree: the level it occupies (the
    // root resides at level 0; a line at level L >= 1 starts with L + 1
    // three-column segments created using spaces, │, ├─, and └─), followed
    // by the statement and its label.
    struct TreeLine
    {
        std::size_t level{0};
        std::string fact;
        std::string label;
    };

    std::vector<TreeLine> tree_lines(const std::string& tree)
    {
        std::vector<TreeLine> result;
        std::istringstream    lines(tree);
        for (std::string line; std::getline(lines, line);)
        {
            if (line.empty()) continue;
            std::size_t columns = 0;
            std::size_t i       = 0;
            while (i < line.size())
            {
                if (line[i] == ' ')
                {
                    ++columns;
                    ++i;
                }
                else if (line.compare(i, 3, "│") == 0 || line.compare(i, 3, "├") == 0 || line.compare(i, 3, "└") == 0 || line.compare(i, 3, "─") == 0)
                {
                    ++columns;
                    i += 3;
                }
                else
                {
                    break;
                }
            }
            TreeLine t;
            t.level                = columns == 0 ? 0 : columns / 3 - 1;
            const std::string rest = line.substr(i);
            std::size_t       cut  = rest.find("  [");
            const std::size_t cut2 = rest.find("  … [");
            if (cut2 != std::string::npos && (cut == std::string::npos || cut2 < cut)) cut = cut2;
            t.fact  = rest.substr(0, cut);
            t.label = cut == std::string::npos ? std::string{} : rest.substr(cut + 2);
            result.push_back(t);
        }
        return result;
    }

    std::size_t deepest_level(const std::string& tree)
    {
        std::size_t deepest = 0;
        for (const TreeLine& t : tree_lines(tree))
            deepest = std::max(deepest, t.level);
        return deepest;
    }

    // A statement that the tree prints below itself, on a single path from
    // the root, or an empty string. This kind of tree reads as a derivation
    // that runs in a circle.
    std::string fact_under_itself(const std::string& tree)
    {
        std::vector<std::string> path;
        for (const TreeLine& t : tree_lines(tree))
        {
            path.resize(std::min(path.size(), t.level));
            if (std::find(path.begin(), path.end(), t.fact) != path.end()) return t.fact;
            path.push_back(t.fact);
        }
        return {};
    }

    // A statement printed "[see above]" where the expansion it refers to --
    // the most recent one output before it -- includes one of the line's
    // own ancestors, or an empty string. This line reads as a derivation
    // that loops back on itself, even though none is printed below itself.
    std::string see_above_through_ancestor(const std::string& tree)
    {
        const std::vector<TreeLine> lines = tree_lines(tree);
        std::vector<std::string>    path;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            path.resize(std::min(path.size(), lines[i].level));
            if (lines[i].label == "[see above]")
            {
                for (std::size_t j = i; j-- > 0;)
                {
                    if (lines[j].fact != lines[i].fact || !(lines[j].label.empty() || lines[j].label == "[one of several justifications]")) continue;
                    if (j + 1 >= lines.size() || lines[j + 1].level != lines[j].level + 1) continue;
                    for (std::size_t k = j + 1; k < lines.size() && lines[k].level > lines[j].level; ++k)
                        if (std::find(path.begin(), path.end(), lines[k].fact) != path.end()) return lines[i].fact;
                    break;
                }
            }
            path.push_back(lines[i].fact);
        }
        return {};
    }

    // The depth levels of the lines designated as
    // "[depth limit]".
    std::vector<std::size_t> limit_levels(const std::string& tree)
    {
        std::vector<std::size_t> levels;
        for (const TreeLine& t : tree_lines(tree))
            if (t.label.find("[depth limit") != std::string::npos) levels.push_back(t.level);
        return levels;
    }

    // The statements printed as "[see above]" higher up than any line that
    // expands them, with each such line attaining the depth limit: the
    // reader is sent from the point where the limit would permit the
    // greatest portion of the proof to the point where it permits the
    // smallest.
    std::vector<std::string> highest_only_referenced(const std::string& tree)
    {
        const std::vector<TreeLine>                                      lines = tree_lines(tree);
        std::map<std::string, std::size_t>                               highest;
        std::map<std::string, std::vector<std::pair<std::size_t, bool>>> expansions; // level, cut
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            const TreeLine& t = lines[i];
            if (t.label == "[see above]")
            {
                const auto [it, fresh] = highest.try_emplace(t.fact, t.level);
                if (!fresh) it->second = std::min(it->second, t.level);
            }
            if (!(t.label.empty() || t.label == "[one of several justifications]") || i + 1 == lines.size() || lines[i + 1].level != t.level + 1) continue;
            bool cut = false;
            for (std::size_t j = i + 1; j < lines.size() && lines[j].level > t.level; ++j)
                cut = cut || lines[j].label.find("[depth limit") != std::string::npos;
            expansions[t.fact].emplace_back(t.level, cut);
        }
        std::vector<std::string> result;
        for (const auto& [fact, expanded] : expansions)
        {
            const auto referenced    = highest.find(fact);
            bool       all_cut_below = referenced != highest.end();
            for (const auto& [level, cut] : expanded)
                all_cut_below = all_cut_below && cut && level > referenced->second;
            if (all_cut_below) result.push_back(fact);
        }
        return result;
    }

    // Facts created via the API, which a script comprising thousands of
    // lines is unable to achieve within a reasonable time frame: subject,
    // predicate, object by name.
    void assert_fact(zelph::network::Reasoning* graph, const std::string& subject, const std::string& predicate, const std::string& object)
    {
        const std::string lang = graph->lang();
        graph->fact(graph->node(subject, lang), graph->node(predicate, lang), {graph->node(object, lang)});
    }

    // A fact defined by the names of its subject, predicate, and
    // object, without asserting it: the identifier of a fact node is
    // the hash of these three components.
    zelph::network::Node fact_node(zelph::network::Reasoning* graph, const std::string& subject, const std::string& predicate, const std::string& object)
    {
        const std::string lang = graph->lang();
        return zelph::network::Zelph::create_hash(graph->node(predicate, lang), graph->node(subject, lang), zelph::network::adjacency_set{graph->node(object, lang)});
    }

    // The facts the walk of the first path condition within a proof steps
    // over, as far as a rule establishes them (ProofNode::walk_edges). The
    // tree outputs these facts exclusively when a proof terminates on a
    // cycle.
    std::vector<zelph::network::Node> first_walk(const zelph::network::ProofNode& proof)
    {
        std::vector<zelph::network::Node> edges;
        if (!proof.walk_edges.empty())
            for (const auto& edge : proof.walk_edges.front())
                edges.push_back(edge->fact);
        return edges;
    }

    double seconds_since(const std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

#ifdef __linux__
    // A value from /proc/self/status in kilobytes: "VmRSS:", the current
    // resident set, or "VmHWM:", its peak.
    long status_kb(const std::string& key)
    {
        std::ifstream status("/proc/self/status");
        for (std::string line; std::getline(status, line);)
            if (line.rfind(key, 0) == 0) return std::stol(line.substr(key.size()));
        return -1;
    }

    // How much the resident set expanded beyond its prior size during the
    // execution of `work`, at its peak, in kilobytes. Writing 5 to
    // clear_refs resets the peak back to the current resident set size
    // (Linux 4.0). Memory previously released by earlier work but retained
    // by the allocator is returned first: if reused, it would conceal the
    // actual increase.
    template <typename Work>
    long peak_growth_kb(const Work& work)
    {
    #ifdef __GLIBC__
        malloc_trim(0);
    #endif
        {
            std::ofstream reset("/proc/self/clear_refs");
            REQUIRE(reset.is_open());
            reset << "5";
        }
        const long before = status_kb("VmRSS:");
        work();
        return status_kb("VmHWM:") - before;
    }
#endif
}

// ---------------------------------------------------------------------------
// .explain: reconstruction of proof from the saturated graph. During
// inference, no provenance is recorded -- these tests pin that backward
// search alone recovers justifications from the rules in force, labels its
// leaves truthfully, resolves facts that mutually derive one another as a
// single component, conducts a single search for each fact, indicates when
// an additional justification is available, adheres to the depth limit, and
// prints shared subproofs precisely once.
//
// Which of two competing rules the search tries first is determined by the
// rules' node ids, which are hashes of their respective parts, not by the
// order in which they were entered. A test requiring the search to meet one
// route first runs two variants that keep the competing rules at identical
// ids while exchanging their roles.
// ---------------------------------------------------------------------------

TEST_CASE("explain: axioms and single-step derivations")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X likes Y) => (Y liked-by X)");
        interactive.process("alice likes bob");
        interactive.run(true, false, false);

        SUBCASE("input facts are axioms")
        {
            collector.clear();
            interactive.process(".explain alice likes bob");
            CHECK(any_output_contains(collector, "[axiom]"));
        }
        SUBCASE("derived facts show their premise")
        {
            collector.clear();
            interactive.process(".explain bob liked-by alice");
            CHECK(any_output_contains(collector, "alice likes bob"));
            CHECK(any_output_contains(collector, "[axiom]"));
        }
        SUBCASE("unasserted facts are reported, not invented")
        {
            collector.clear();
            interactive.process(".explain bob likes alice");
            CHECK(any_output_contains(collector, "not asserted"));
        } });
}

// A rule that some statement only MENTIONS never fires: the template within a
// rule generator, or a rule that a statement refers to (logic.md, "Mentioning
// a Rule Is Not Asserting It"). Forward evaluators skip it, and so must the
// search. It did not: a generator's template, with a predicate being a
// variable, unified with every fact, causing the tree to justify a typed fact
// via a rule that never fired, declared second justifications that did not
// exist, and labelled an unrelated typed fact as "asserted; no derivation
// found".
//
// Referred to by its own text, such a rule was printed as "[axiom]", an
// input fact that holds. A rule that a statement refers to is not in force,
// and the mentioned node could also be a typed rule: a statement names the
// typed rule's own node when it writes identical text and that text is
// hash-consed, when a rule derives it from the typed rule's parts, or when a
// script builds it from the rule node it holds, and the typed rule then
// ceases to fire, regardless of whether its text holds a variable. The graph
// does not tell that node from a rule that is merely mentioned, so its label
// says only what is true in both readings. The label is exclusive to the
// queried node; a premise is labelled as the input the engine read it as.
TEST_CASE("explain: a rule that is only mentioned justifies nothing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a rule a statement talks about")
        {
            process_lines(interactive, R"(
((X p Y) => (X q Y)) is questionable
a p b
a q b
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (a q b) 0");
            CHECK(explained(collector) == "a q b  [axiom]");

            // The rule, named by its text, is the one mentioned in
            // the statement, which is not in force.
            collector.clear();
            interactive.process(".explain ((A p B) => (A q B)) 0");
            CHECK(explained(collector) == "(X p Y) => (X q Y)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));

            // Typed, the identical text serves as a rule in force, which
            // the text then names: an input.
            interactive.process("(X p Y) => (X q Y)");
            collector.clear();
            interactive.process(".explain ((A p B) => (A q B)) 0");
            CHECK(explained(collector) == "(X p Y) => (X q Y)  [axiom]");
        }
        SUBCASE("a rule a switch mentions, named by its text")
        {
            // The template contained within a rule generator is a rule
            // that the switch mentions, yet it is not in force:
            // .list-rules displays only the switch itself.
            interactive.process("(K is on) => ((X p Y) => (X q Y))");
            collector.clear();
            interactive.process(".explain ((A p B) => (A q B)) 0");
            CHECK(explained(collector) == "(X p Y) => (X q Y)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_starts_with(collector, "(K is on) => "));
            CHECK_FALSE(any_output_starts_with(collector, "(X p Y) => (X q Y)"));
        }
        SUBCASE("a rule whose variables stand only in its collections")
        {
            // The placement of a mentioned rule's variables does not change
            // its label: here they stand solely within a collection of the
            // rule's text, or within one contained inside a set constant,
            // and the variable store associated with the rule's facts holds
            // none.
            interactive.process("((a p @{Y}) => (c q d)) is weird");
            collector.clear();
            interactive.process(".explain ((a p @{B}) => (c q d)) 0");
            CHECK(explained(collector) == "(a p @{Y}) => (c q d)  [rule mentioned; not in force]");

            interactive.process("((a p {@{Y}}) => (c q d)) is odd");
            collector.clear();
            interactive.process(".explain ((a p {@{B}}) => (c q d)) 0");
            CHECK(explained(collector) == "(a p {@{Y}}) => (c q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));
        }
        SUBCASE("a rule whose variables stand only in conjunction sets")
        {
            // A conjunction set is a node devoid of fact structure: here
            // the rule's conditions, then the subject of its consequence.
            // Either way, the mentioned rule is interpreted identically to
            // how its single-condition twin is read.
            interactive.process("((X p Y, X r Y) => (c q d)) is noted");
            collector.clear();
            interactive.process(".explain ((A p B, A r B) => (c q d)) 0");
            CHECK(explained(collector) == "((X p Y), (X r Y)) => (c q d)  [rule mentioned; not in force]");

            interactive.process("((a q b) => ((X p Y, X r Y) is noted)) is odd");
            collector.clear();
            interactive.process(".explain ((a q b) => ((A p B, A r B) is noted)) 0");
            const std::string tree = explained(collector);
            CHECK(tree.starts_with("(a q b) => "));
            CHECK(tree.ends_with(" is noted)  [rule mentioned; not in force]"));

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));
        }
        SUBCASE("a rule that holds a collection, before and after a switch writes it")
        {
            interactive.process("((X p Y) => (X q @{c})) is noted");
            collector.clear();
            interactive.process(".explain ((A p B) => (A q @{c})) 0");
            CHECK(explained(collector) == "(X p Y) => (X q @{c})  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));

            // Written by a switch, the identical text constitutes a rule in
            // force, and the text names that rule, justified by the switch.
            process_lines(interactive, R"(
(K is on) => ((X p Y) => (X q @{c}))
k is on
a p b
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".explain ((A p B) => (A q @{c})) 0");
            CHECK(explained(collector) == R"((X p Y) => (X q @{c})
   └─ k is on  [axiom])");
        }
        SUBCASE("a ground rule with one condition, only mentioned")
        {
            // No individual typed this rule, and it is not in force. It
            // bore the label "[axiom]", an input in force. Its text is
            // precisely the node that a typed twin would be (the
            // subsequent scenario), thus the graph cannot say that no one
            // asserted it either: the label names what holds across both
            // readings.
            interactive.process("((a p b) => (c q d)) is odd");
            collector.clear();
            interactive.process(".explain ((a p b) => (c q d)) 0");
            CHECK(explained(collector) == "(a p b) => (c q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));
        }
        SUBCASE("a ground rule typed and mentioned, named by its text")
        {
            // Without any variable, without a collection belonging to the
            // rule itself, and without a conjunction set beneath it, the
            // typed rule and the one the statement mentions form a single
            // node (the corner Zelph::is_mentioned names), and the typed
            // rule ceases its firing. "[axiom]" claimed an input in force;
            // "not asserted" would reject the claim that it was typed.
            process_lines(interactive, R"(
(a p b) => (c q d)
((a p b) => (c q d)) is odd
)");
            collector.clear();
            interactive.process(".explain ((a p b) => (c q d)) 0");
            CHECK(explained(collector) == "(a p b) => (c q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));

            process_lines(interactive, "a p b\n");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("c q X");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("a ground rule over a set constant of conditions, typed and mentioned")
        {
            // Two conditions fail to exit the corner via their number: a
            // set constant hash-conses, thus this typed rule and the
            // mention constitute a single node as well, and the typed rule
            // stops firing.
            process_lines(interactive, R"(
(*{(a p b) (a r b)} ~ conjunction) => (c q d)
((*{(a p b) (a r b)} ~ conjunction) => (c q d)) is odd
)");
            collector.clear();
            interactive.process(".explain ((*{(a p b) (a r b)} ~ conjunction) => (c q d)) 0");
            CHECK(explained(collector) == "((a p b), (a r b)) => (c q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));

            process_lines(interactive, "a p b\na r b\n");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("c q X");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("a ground rule over a comma list of conditions, only mentioned")
        {
            // A comma list is a conjunction set that the statement
            // constructs for itself, meaning this mention constitutes a
            // node apart from a typed twin (two scenarios detailed
            // below). It is not in force regardless, and a statement that
            // a rule derives can name a typed rule of this form itself
            // (as shown below), thus the label is the one each rule
            // mentioned by a statement bears.
            interactive.process("((a p b, a r b) => (c q d)) is odd");
            collector.clear();
            interactive.process(".explain ((a p b, a r b) => (c q d)) 0");
            CHECK(explained(collector) == "((a p b), (a r b)) => (c q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));
        }
        SUBCASE("a ground rule holding a collection, only mentioned")
        {
            // The same applies to a collection written within a rule's
            // text, which belongs to the rule itself: each statement that
            // writes it builds a new one.
            interactive.process("((a p b) => (c q @{x})) is odd");
            collector.clear();
            interactive.process(".explain ((a p b) => (c q @{x})) 0");
            CHECK(explained(collector) == "(a p b) => (c q @{x})  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));
        }
        SUBCASE("ground rules over a comma list or holding a collection, typed and mentioned")
        {
            // Expressed in plain text, the mention is a node apart from the
            // typed rule, and nothing mentions the typed rule: it stays in
            // force, and the text names it rather than the mention -- an
            // input.
            process_lines(interactive, R"(
(a p b, a r b) => (c q d)
((a p b, a r b) => (c q d)) is odd
(a p b) => (c q @{x})
((a p b) => (c q @{x})) is odd
)");
            collector.clear();
            interactive.process(".explain ((a p b, a r b) => (c q d)) 0");
            CHECK(explained(collector) == "((a p b), (a r b)) => (c q d)  [axiom]");

            collector.clear();
            interactive.process(".explain ((a p b) => (c q @{x})) 0");
            CHECK(explained(collector) == "(a p b) => (c q @{x})  [axiom]");
        }
        SUBCASE("a ground rule a switch mentions, named by its text")
        {
            // The rule is a ground pattern of the switch's text, in
            // addition to being a rule that the switch mentions, and was
            // labelled as the pattern, "not asserted". Named by the query,
            // it carries the label of each rule a statement mentions,
            // which says what holds of it as a rule: it is not in force.
            interactive.process("(K is on) => ((a p b) => (c q d))");
            collector.clear();
            interactive.process(".explain ((a p b) => (c q d)) 0");
            CHECK(explained(collector) == "(a p b) => (c q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_starts_with(collector, "(K is on) => "));
            CHECK_FALSE(any_output_starts_with(collector, "(a p b) => (c q d)"));

            // Its ground parts are patterns of the switch's text, which
            // remain unclaimed by anyone, and keep that label.
            collector.clear();
            interactive.process(".explain (c q d) 0");
            CHECK(explained(collector) == "c q d  [rule pattern; not asserted]");
        }
        SUBCASE("typed ground rules that nothing mentions")
        {
            // In force and an input, regardless of the content of the
            // text: the label assigned to a mentioned rule is for a node
            // that some statement names.
            process_lines(interactive, R"(
(a p b) => (c q d)
(a p b, a r b) => (c q e)
(a p b) => (c q @{x})
)");
            collector.clear();
            interactive.process(".explain ((a p b) => (c q d)) 0");
            CHECK(explained(collector) == "(a p b) => (c q d)  [axiom]");

            collector.clear();
            interactive.process(".explain ((a p b, a r b) => (c q e)) 0");
            CHECK(explained(collector) == "((a p b), (a r b)) => (c q e)  [axiom]");

            collector.clear();
            interactive.process(".explain ((a p b) => (c q @{x})) 0");
            CHECK(explained(collector) == "(a p b) => (c q @{x})  [axiom]");
        }
        SUBCASE("a typed ground rule with one condition, mentioned by a derived statement")
        {
            // A ground `=>` fact is data to a rule over rules, which binds
            // the typed rule's parts and builds `G => (c q d)` from them:
            // the typed rule's own node, which now is considered mentioned
            // and ceases to fire. The graph provides no distinction
            // between this and a rule that is merely mentioned. "[axiom]"
            // claimed an input in force; "not asserted" would deny that it
            // was typed.
            process_lines(interactive, R"(
(a p b) => (c q d)
(G => (c q d)) => ((G => (c q d)) is seen)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".explain ((a p b) => (c q d)) 0");
            CHECK(explained(collector) == "(a p b) => (c q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK_FALSE(any_output_contains(collector, "(a p b) => (c q d)"));

            process_lines(interactive, "a p b\n");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("c q X");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("a typed ground rule over a comma list, mentioned by a derived statement")
        {
            // Expressed in textual form, a comma list is the statement's
            // own; a rule over rules binds the typed rule's conjunction
            // set itself, thereby causing the derived statement to name
            // the typed rule.
            process_lines(interactive, R"(
(a p b, a r b) => (c q d)
(G => (c q d)) => ((G => (c q d)) is seen)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".explain ((a p b, a r b) => (c q d)) 0");
            CHECK(explained(collector) == "((a p b), (a r b)) => (c q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK_FALSE(any_output_contains(collector, "((a p b), (a r b)) => (c q d)"));

            process_lines(interactive, "a p b\na r b\n");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("c q X");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("a typed ground rule holding a collection, mentioned by a derived statement")
        {
            // The same applies to a collection of the rule's own, which
            // the rule over rules binds as H.
            process_lines(interactive, R"(
(a p b) => (c q @{x})
(G => (c q H)) => ((G => (c q H)) is seen)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".explain ((a p b) => (c q @{x})) 0");
            CHECK(explained(collector) == "(a p b) => (c q @{x})  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK_FALSE(any_output_contains(collector, "(a p b) => (c q @{x})"));

            process_lines(interactive, "a p b\n");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("c q X");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("a typed rule whose variables stand only in a template, beside a rule over rules")
        {
            // The variable resides within a collection of the rule's own
            // text, which var_in_closure enters, thus a rule over rules does
            // not read the typed rule as data: nothing mentions it, and it
            // stays in force and serves as an input.
            interactive.process("(a p @{Y}) => (c q d)");
            interactive.process("(G => (c q d)) => ((G => (c q d)) is seen)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".explain ((a p @{B}) => (c q d)) 0");
            CHECK(explained(collector) == "(a p @{Y}) => (c q d)  [axiom]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(a p @{Y}) => (c q d)"));
        }
        SUBCASE("a typed rule with a variable, mentioned by a script that holds its node")
        {
            // A script names the node it holds, so the statement it
            // constructs mentions the typed rule itself, causing it to
            // cease firing. Its text holds a variable, and "not asserted"
            // would reject the claim that it was typed.
            interactive.process(R"js(%(def r (zelph/fact (zelph/fact 'X "p" "b") "=>" (zelph/fact 'X "q" "d"))))js");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X p b) => (X q d)"));

            interactive.process(R"js(%(zelph/fact r "is" "odd"))js");
            collector.clear();
            interactive.process(".explain ((A p b) => (A q d)) 0");
            CHECK(explained(collector) == "(X p b) => (X q d)  [rule mentioned; not in force]");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "No rules found."));

            process_lines(interactive, "a p b\n");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("a q X");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("an arrow between two names, mentioned")
        {
            // `=>` connecting two names constitutes a fact and is no rule:
            // its condition is nothing that could possibly hold, and
            // .list-rules never counts it. A statement that refers to it
            // builds it, and it holds, just as every fact a statement
            // refers to does: the claim "rule mentioned" would be false.
            interactive.process("(p => q) is tautology");
            collector.clear();
            interactive.process(".explain (p => q) 0");
            CHECK(explained(collector) == "p => q  [axiom]");
        }
        SUBCASE("a ground rule a statement talks about is a premise")
        {
            // The rule labels are exclusive to the node that was queried.
            // In this case, the mentioned rule is the data a rule over
            // rules read, and as a premise, it carries the label of that
            // input.
            process_lines(interactive, R"(
((a p b) => (c q d)) is odd
(G => H) => (G seen H)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".explain ((a p b) seen (c q d)) 0");
            CHECK(explained(collector) == R"((a p b) seen (c q d)
   └─ (a p b) => (c q d)  [axiom])");
        }
        SUBCASE("a ground rule over a value that holds another rule's variable is a premise")
        {
            // `@{x}` is data, and the generated rule (X p Y) => (X in @{x})
            // writes its own X into it. That X is not a variable in the
            // ground rule mentioned by the statement, which the rule over
            // rules reads as data regardless of what other rules wrote into a
            // value it holds: as a premise, it is that input.
            process_lines(interactive, R"(
d has @{x}
(A has C) => ((X p Y) => (X in C))
(A has C) => (((a p b) => (c q C)) is odd)
(G => H) => (G seen H)
)");
            interactive.run(true, false, false);
            interactive.process("S seen O");
            collector.clear();
            interactive.process(".explain");
            CHECK(explained(collector) == R"((a p b) seen (c q @{x})
   └─ (a p b) => (c q @{x})  [axiom])");
        }
        SUBCASE("a rule whose variables stand only in its collections is no premise")
        {
            // Y resides within a collection of the rule's own text, which
            // var_in_closure enters, causing the engine not to read this
            // rule as data, and the rule over rules derives nothing from
            // it: a mentioned rule containing a variable serves as a
            // premise of nothing.
            process_lines(interactive, R"(
((a p @{Y}) => (c q d)) is weird
(G => H) => (G seen H)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S seen O");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a switched rule, before and after the switch")
        {
            // When activated, the generator claims a renamed copy of the
            // rule it mentions; that copy is in force and justifies the
            // fact.
            process_lines(interactive, R"(
(K is on) => ((X p Y) => (X q Y))
a p b
a q b
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (a q b) 0");
            CHECK(explained(collector) == "a q b  [axiom]");

            interactive.process("k is on");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".explain (a q b) 0");
            CHECK(explained(collector) == R"(a q b
   └─ a p b  [axiom])");
        }
        SUBCASE("a switched rule, named by its text")
        {
            // The text names the copy the switch claimed and the rule the
            // switch mentions alike, and a typed rule is resolved to an
            // existing rule sharing the same shape. The explanation,
            // resolved to the mention, referred to a rule that had never
            // fired, and it was expressed as an axiom. Which of the two
            // comes first is determined by the node ids, so both shapes are
            // tried: in the first one, the mention does.
            for (const std::string container : {"{(Y q k)}", "(Y q k)"})
            {
                CAPTURE(container);
                interactive.process(".new");
                process_lines(interactive, "(K is on) => ((X p Y) => (X likes " + container + "))\nsw is on\n");
                interactive.run(true, false, false);

                collector.clear();
                interactive.process(".explain ((X p Y) => (X likes " + container + ")) 0");
                const std::string tree = explained(collector);
                CHECK(tree.ends_with("\n   └─ sw is on  [axiom]"));
            }
        }
        SUBCASE("a rule generator")
        {
            process_lines(interactive, R"(
(A knows B) => ((X p B) => (X q B))
tom knows red
x p red
z q green
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (x q red) 0");
            CHECK(explained(collector) == R"(x q red
   └─ x p red  [axiom])");

            collector.clear();
            interactive.process(".explain (z q green) 0");
            CHECK(explained(collector) == "z q green  [axiom]");
        } });
}

// A rule authored by a generator is explained by the generator's premise,
// also where the rule holds a nested rule whose conditions the generator's
// binding reaches. The nested rule was copied with the generator's variable
// remaining unaltered within it, so the text with the binding substituted
// named no node.
TEST_CASE("explain: a generated rule that holds a nested rule is explained by the generator's premise")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("as a value")
        {
            process_lines(interactive, R"(
(G go H) => ((X p Y) => (X likes ((A r H, A s B) => (c q d))))
t go k
)");
            collector.clear();
            interactive.process(".explain ((X p Y) => (X likes ((A r k, A s B) => (c q d)))) 0");
            CHECK(explained(collector).ends_with("\n   └─ t go k  [axiom]"));
        }
        SUBCASE("in a collection")
        {
            process_lines(interactive, R"(
(G go H) => ((X p Y) => (X hates @{((A r H, A s B) => (c q d))}))
t go k
)");
            collector.clear();
            interactive.process(".explain ((X p Y) => (X hates @{((A r k, A s B) => (c q d))})) 0");
            CHECK(explained(collector).ends_with("\n   └─ t go k  [axiom]"));
        } });
}

// A rule authored by a generator whose premise no longer holds has lost
// its justification, and is labelled just as any derived fact that has
// lost its justification is. The construction is asked which rule the
// generator's consequence stands for, under a solution to the generator's
// join; thus, with no solution left, the consequence matched nothing, and
// the rule was read as an axiom, which says that no rule in force
// concludes it. The binding that the generator's consequence and the rule
// determine by themselves is also asked.
TEST_CASE("explain: a generated rule whose premise no longer holds is no axiom")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("its premise removed")
        {
            process_lines(interactive, R"(
(a p b) => (c q d)
(G => (S q O)) => ((X r Y) => (X t O))
.remove a
)");
            collector.clear();
            interactive.process(".explain ((X r Y) => (X t d)) 0");
            CHECK(explained(collector) == "(X r Y) => (X t d)  [asserted; no derivation found]");
        }
        SUBCASE("its premise removed after the rule bound the premise's collection")
        {
            // The rule produced by the generator names the data term of the
            // collection of `(a p b) => (c q @{d})`, which the generator has
            // bound. The term does not reference any collection, thus the
            // generator's join is left to find the binding; since the
            // construction under the term itself builds the identical rule,
            // the rule and the generator's consequence continue to determine
            // it. Once the typed rule is established, the rule is explained
            // over it as data; upon removal of the typed rule, it has lost
            // its justification. A literal does not refer to any existing
            // collection: the rule is precisely what `.explain` without an
            // argument explains.
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
)");
            collector.clear();
            interactive.process(".explain");
            CHECK(explained(collector) == "(X r Y) => (X in @{})\n   └─ (a p b) => (c q @{d})  [axiom]");

            interactive.process(".remove a");
            collector.clear();
            interactive.process(".explain");
            CHECK(explained(collector) == "(X r Y) => (X in @{})  [asserted; no derivation found]");
        } });
}

// A rule over rules binds the condition set of `(a p b, c q d) => (e f g)`
// and writes a rule that refers to the set twice: once as the object of
// `noted`, where it keeps the set, and again where a membership is written
// into it, where it names the set's data term. The generator's binding is
// the set, and the term is the data term of that set. Interpreted as two
// bindings of a single variable that disagree, the generator was never
// joined, and the rule read as an axiom, which says that no rule in force
// derives it.
TEST_CASE("explain: a generated rule that names a bound condition set and its data term is explained by its generator")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(C => (e f g)) => ((X r Y) => ((X in C) noted C))
)");
        collector.clear();
        interactive.process(".explain");
        const std::string tree = explained(collector);
        CHECK(tree.starts_with("(X r Y) => ((X in @{"));
        CHECK(tree.ends_with(") noted {(c q d) (a p b)})\n   └─ ((c q d), (a p b)) => (e f g)  [axiom]")); });
}

// A rule over rules binds the collections of n ground rules and writes one
// rule per collection, each over the collection's data term. The term refers
// to no collection, hence the generator's join is not seeded with it, and
// every solution of the generator's condition qualifies as a candidate, at a
// unit of work each. A solution whose binding is neither the term nor the
// collection that the term represents is dismissed by the ids alone; only
// the one that is asks the construction, which builds a rule. Asked for
// every solution, the construction would build a rule per solution: an
// additional unit of work per rule, and half again as long for one such rule
// among three thousand.
TEST_CASE("explain: a generated rule over a data term asks the construction only for the solution that binds its collection")
{
    const auto work = [](const std::size_t rules)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        std::string                 program = ".deductions off\n";
        for (std::size_t i = 0; i < rules; ++i)
            program += "(a" + std::to_string(i) + " p b) => (c" + std::to_string(i) + " q @{d" + std::to_string(i) + "})\n";
        program += "(G => (S q C)) => ((X r Y) => (X in C))\n.deductions all\n(az p b) => (cz q @{dz})\n";
        process_lines(interactive, program);
        collector.clear();
        interactive.process(".explain");
        CHECK(explained(collector) == "(X r Y) => (X in @{})\n   └─ (az p b) => (cz q @{dz})  [axiom]");
        return interactive.graph()->last_explain_counts().work;
    };
    const std::size_t few  = work(20);
    const std::size_t many = work(420);
    CAPTURE(few);
    CAPTURE(many);
    CHECK(many - few < 600); // one unit per solution, not two
}

// A membership within the term of a rule's bucket is explained by the firing
// that wrote it, a ground member of the bucket by any firing of its rule,
// and a rule whose condition reads such a membership joins it just like any
// other fact.
TEST_CASE("explain: a membership of a bucket's term is explained by the firing that wrote it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("written by the bucket's rule")
        {
            process_lines(interactive, R"(
(X reported Y) => (Y in @{X})
alice reported bug1
bob reported bug2
)");
            // A literal names no existing collection: the answer's node is
            // the one explained by `.explain` when invoked without an
            // argument.
            interactive.process("bug2 in O");
            collector.clear();
            interactive.process(".explain");
            CHECK(explained(collector) == R"(bug2 in @{bug1 bug2}
   └─ bob reported bug2  [axiom])");
        }
        SUBCASE("a ground member of the bucket")
        {
            // No firing writes `bug1 in` the term: the term holds the
            // bucket's ground members starting from the initial firing,
            // and that particular firing is what brought the membership
            // into the graph. Its premises constitute the explanation;
            // absent those, the membership read as asserted.
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
)");
            interactive.process("bug1 in O");
            collector.clear();
            interactive.process(".explain");
            CHECK(explained(collector) == R"(bug1 in @{bug1 bug2 bug3}
   └─ alice reported bug3  [axiom])");
        }
        SUBCASE("a ground member of the bucket, after two firings")
        {
            // Each firing of the rule builds the term with its ground
            // members, thus each one derives the membership, and the root
            // says that it shows one of them.
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
bob reported bug4
)");
            interactive.process("bug1 in O");
            collector.clear();
            interactive.process(".explain");
            const std::string tree = explained(collector);
            INFO(tree);
            CHECK((tree == R"(bug1 in @{bug1 bug2 bug3 bug4}  [one of several justifications]
   └─ alice reported bug3  [axiom])"
                   || tree == R"(bug1 in @{bug1 bug2 bug3 bug4}  [one of several justifications]
   └─ bob reported bug4  [axiom])"));
        }
        SUBCASE("read by another rule")
        {
            // The `bucket in @{bucket}` entry is data of the term
            // `b likes @{bucket}` names, written by the same firing.
            process_lines(interactive, R"(
(X p Y) => (X likes @{bucket})
(A in C, X likes C) => (A seen-in X)
b p k
)");
            collector.clear();
            interactive.process(".explain (bucket seen-in b) 0");
            CHECK(explained(collector) == R"(bucket seen-in b
   ├─ b likes @{bucket}
   │  └─ b p k  [axiom]
   └─ bucket in @{bucket}
      └─ b p k  [axiom])");
        } });
}

TEST_CASE("explain: a premise carrying further objects is found")
{
    // Unification matches a one-object condition against a fact that carries
    // more -- `(X p Y)` binds Y to b and to c of `a p b c`, and the rule
    // fires twice, exactly as the query answers twice. The proof search
    // resolved its premises by the exact triple hash, where `a p b` is a
    // DIFFERENT node that does not exist, so it found no derivation at all
    // and labelled a derived fact "asserted; no derivation found" -- the one
    // thing .explain must never say about a fact nobody asserted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b c");
        interactive.process("(X p Y) => (X q Y)");
        interactive.run(true, false, false);

        for (const char* target : {".explain (a q b)", ".explain (a q c)"})
        {
            collector.clear();
            interactive.process(target);
            CHECK(any_output_contains(collector, "a p b c")); // the fact that MATCHED
            CHECK(any_output_contains(collector, "[axiom]"));
            CHECK_FALSE(any_output_contains(collector, "no derivation found"));
        }

        // The exactness the proof search opts out of stays elsewhere: a
        // prune asked for `a p b` must not take the longer fact with it.
        collector.clear();
        interactive.process(".prune-facts (a p b)");
        CHECK(any_output_contains(collector, "Pruned 0"));

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).size() == 2); });
}

// The wider fact serves as a premise also where the narrow one is present. In
// this case, a rule within a cycle brings forth the narrow instance (a p b)
// from (b q a), which the wider (a p b c) derived. The search took the narrow
// fact whenever it was available, observed solely (b q a) <= (a p b) <=
// (b q a), and answered "[asserted; no derivation found]" regarding the
// derived (b q a), even though the forward pass's derivation, originating from
// (a p b c), was just one step away. A consequence that only matches a wider
// fact does not derive it either: (a p b c) is an input fact, an axiom, even
// though the narrow consequence (X p Y) unifies with it.
TEST_CASE("explain: a wider fact derives a premise also where the narrow one exists")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (Y q X)
(Y q X) => (X p Y)
a p b c
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (b q a) 0");
        CHECK(explained(collector) == "b q a\n   └─ a p b c  [axiom]");

        collector.clear();
        interactive.process(".explain (c q a) 0");
        CHECK(explained(collector) == "c q a\n   └─ a p b c  [axiom]");

        collector.clear();
        interactive.process(".explain (a p b) 0");
        CHECK(explained(collector) == "a p b\n   └─ b q a\n      └─ a p b c  [axiom]"); });
}

// The narrow instance a consequence denotes need not be present: due to the
// rule being entered while auto-run was off, (a P279 b) has not yet been
// added to the graph. Despite this, the match of (X R Z) with (a P279 b c)
// was taken as exact, and the wider fact was printed as derived through an
// instantiation that derives (a P279 b). Once (a P279 b) exists, the same
// fact appeared as "[asserted; no derivation found]" -- a consequence that
// only matches a wider fact does not derive it, thus it is an axiom both
// times.
// A ground condition whose exact node is present but fails to hold -- a
// rule's own pattern, here (a p b) of the first rule -- still matches the
// wider fact (a p b c), just as during the forward pass, which derived
// (a r b) from it. The search left the instantiation at the exact node and
// printed the derived fact as "[asserted; no derivation found]".
TEST_CASE("explain: a wider fact is tried where the exact one is only a rule's pattern")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
a q b
(a p b) => (z s z)
((X q Y), (X p Y)) => (X r Y)
a p b c
)");
    interactive.run(true, false, false);
    collector.clear();
    interactive.process(".explain (a r b) 0");
    CHECK(explained(collector) == "a r b\n"
                                  "   ├─ a p b c  [axiom]\n"
                                  "   └─ a q b  [axiom]");
}

TEST_CASE("explain: a rule that derives the narrow fact does not derive the wider one")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.auto-run
P279 ~ transitive
a P279 b c
a P279 m
m P279 b
(R ~ transitive, X R Y, Y R Z) => (X R Z)
)");
    collector.clear();
    interactive.process(".explain (a P279 b c) 0");
    CHECK(explained(collector) == "a P279 b c  [axiom]");

    interactive.run(true, false, false);
    collector.clear();
    interactive.process(".explain (a P279 b c) 0");
    CHECK(explained(collector) == "a P279 b c  [axiom]");
}

TEST_CASE("explain: depth limit and the ? companion idiom (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process("? (&6 + &7)");

        SUBCASE("bare .explain explains the last output node")
        {
            collector.clear();
            interactive.process(".explain");
            CHECK(any_output_contains(collector, "&13"));
        }
        SUBCASE("depth 1 truncates, depth 0 does not")
        {
            collector.clear();
            interactive.process(".explain ((&6 + &7) = &13) 1");
            CHECK(any_output_contains(collector, "depth limit"));

            collector.clear();
            interactive.process(".explain ((&6 + &7) = &13) 0");
            CHECK_FALSE(any_output_contains(collector, "depth limit"));
        } });
}

// The search keeps every result it finds, and a result the depth limit cut
// hinges on the location where it was discovered: a less deep starting point
// means the limit lies farther off. (:s a) is encountered twice beneath:
// descending three levels via one of the root's rules, whose instantiation
// also rests on the "asserted; no derivation found" leaf (:u a), and
// descending two levels through the alternate path. When the search met the
// deep route first, the root proceeded to the shallower path, which
// constitutes the proof it keeps, and reused (:s a) from the deeper
// location: its proof stopped at (:s1 a) with "[depth limit]" positioned one
// level above the limit. A result previously cut by the limit is now
// searched again at the point where a shallower position reaches it.
//
// The search tries the rules in ascending sequence according to their node ids, and a rule's
// identifier is a hash of its parts, meaning the order in which the rules are entered does not
// decide which route the search meets first. Both variants assign identical identifiers to the
// root's two rules, (:m X) => (:r X) and (:p X) => (:r X), since each name is created in the same
// order, and they interchange which one leads the deep way, ensuring that one variant meets the
// deep route first regardless of the hash value. The tree remains identical in both cases.
TEST_CASE("explain: a subproof the depth limit cut is searched again where it is reached higher up")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& deep, const std::string& shallow)
        {
            process_lines(interactive, R"(
(:m X) => (:r X)
(:p X) => (:r X)
:c a
:u a
(:k X) => (:u X)
(:c X) => (:s3 X)
(:s3 X) => (:s2 X)
(:s2 X) => (:s1 X)
(:s1 X) => (:s X)
(:s X) => (:e2 X)
)");
            interactive.process("(:e2 X, :u X) => (:" + deep + " X)");
            interactive.process("(:s X) => (:" + shallow + " X)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (:r a)");
            CHECK(explained(collector) == ":r a  [one of several justifications]\n"
                                          "   └─ :" + shallow + " a\n"
                                          "      └─ :s a\n"
                                          "         └─ :s1 a\n"
                                          "            └─ :s2 a  … [depth limit -- use '.explain <pattern> 0' for the full proof]");
        };
        SUBCASE("deep through m") { check("m", "p"); }
        SUBCASE("deep through p") { check("p", "m"); } });
}

// The reverse: a result discovered at an elevated level, without a cut, is
// reused wherever its fact recurs, and further down the tree its proof reaches
// past the limit. One of the root's rules meets (:q a) two levels beneath and
// proves it, and its instantiation rests on the "asserted; no derivation
// found" leaf (:u a); the one the root keeps meets (:q a) four levels below,
// at the limit. When the search met the near route first, the tree printed
// (:q a)'s proof in full at the limit, one level beneath it. The renderer now
// applies a cut at the limit, regardless of what the search had previously
// reused.
//
// The search tries the rules in ascending order according to their node ids, and a
// rule's id is a hash of its parts, meaning the order in which the rules are entered
// does not decide which route the search meets first. The two variants swap which of the
// root's rules, kept at identical ids, leads the near way.
TEST_CASE("explain: nothing is printed below the depth limit")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& near, const std::string& far)
        {
            process_lines(interactive, R"(
(:m X) => (:r X)
(:p X) => (:r X)
:c a
:u a
(:k X) => (:u X)
(:c X) => (:q X)
)");
            interactive.process("(:q X, :u X) => (:" + near + " X)");
            interactive.process("(:q X) => (:c2 X)");
            interactive.process("(:c2 X) => (:c1 X)");
            interactive.process("(:c1 X) => (:" + far + " X)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (:r a)");
            const std::string tree = explained(collector);
            CHECK(deepest_level(tree) == 4);
            CHECK(tree == ":r a  [one of several justifications]\n"
                          "   └─ :" + far + " a\n"
                          "      └─ :c1 a\n"
                          "         └─ :c2 a\n"
                          "            └─ :q a  … [depth limit -- use '.explain <pattern> 0' for the full proof]");

            // The same tree, unlimited, continues to the
            // axiom.
            collector.clear();
            interactive.process(".explain (:r a) 0");
            CHECK(any_output_contains(collector, ":c a [axiom]"));
        };
        SUBCASE("near through m") { check("m", "p"); }
        SUBCASE("near through p") { check("p", "m"); } });
}

// A single tree, a single fact across two levels: (:s a) serves as a premise
// for (:x a) via (:w a), descending three levels, and for (:y a), descending
// two levels. The tree prints the root's premises in the sequence dictated by
// the rule's conditions, thus in one of the two variants below the deep
// occurrence appears first; there, the depth limit cuts its expansion one
// level further down. "[see above]" at the higher occurrence pointed at that
// cut expansion, and the reader never saw what the limit permits in that
// location. A fact is now re-expanded at a point where it recurs at a higher
// level than a prior expansion that was cut, and "[see above]" directs only
// to one that shows at least as much.
TEST_CASE("explain: a fact first printed close to the limit is expanded again higher up")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& deep, const std::string& shallow)
        {
            process_lines(interactive, R"(
(:x X, :y X) => (:r X)
:c a
(:c X) => (:s3 X)
(:s3 X) => (:s2 X)
(:s2 X) => (:s1 X)
(:s1 X) => (:s X)
(:s X) => (:w X)
)");
            interactive.process("(:w X) => (:" + deep + " X)");
            interactive.process("(:s X) => (:" + shallow + " X)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (:r a)");
            const std::string tree = explained(collector);
            CHECK(deepest_level(tree) == 4);
            // The output of `(:s2 a)` appears solely at locations where
            // `(:s a)` has been expanded two levels above the specified
            // limit.
            CHECK(tree.find(":s2 a  … [depth limit") != std::string::npos);
        };
        SUBCASE("deep through x") { check("x", "y"); }
        SUBCASE("deep through y") { check("y", "x"); } });
}

// A result limited by depth is reused wherever its fact recurs, and the cut
// at the limit conceals the path its derivation took: beneath a fact the
// search was building, a result found earlier might contain that same fact,
// either cut at the limit or found at a different depth. The tree then
// printed the fact beneath itself -- (:k o) under (:m o) under (:k o) --
// which reads as a derivation that loops back on itself. Here, (:k o) and
// (:m o) mutually derive one another, and (:k o) is asserted; the root
// reaches (:m o) deep via (:a o), and (:k o) at a higher level. The two
// variants swap which of the root's rules takes which route, at identical
// ids, so one of them meets the deep route first regardless of the hash
// (see "a premise first reached through a cycle"), and the explanation is
// asked at every limit from 1 to 6. Each "[depth limit]" stands at the
// limit, not located above it.
TEST_CASE("explain: a depth limit never prints a fact below itself")
{
    const auto check = [](zelph::console::Interactive& interactive, zelph::io::OutputCollector& collector, const std::string& query)
    {
        for (std::size_t depth = 1; depth <= 6; ++depth)
        {
            CAPTURE(depth);
            collector.clear();
            interactive.process(".explain " + query + " " + std::to_string(depth));
            const std::string tree = explained(collector);
            CAPTURE(tree);
            CHECK(fact_under_itself(tree).empty());
            CHECK(deepest_level(tree) <= depth);
            for (const std::size_t level : limit_levels(tree))
                CHECK(level == depth);
        }
    };
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto session = [&](const std::string& deep, const std::string& high)
        {
            process_lines(interactive, R"(
(:p1 X) => (:r X)
(:p2 X) => (:r X)
:k o
:u o
(:z X) => (:u X)
(:k X) => (:m X)
(:m X) => (:k X)
(:m X) => (:a X)
)");
            interactive.process("(:a X, :u X) => (:" + deep + " X)");
            interactive.process("(:k X) => (:" + high + " X)");
            interactive.run(true, false, false);
            check(interactive, collector, "(:r o)");
        };
        SUBCASE("deep through p1") { session("p1", "p2"); }
        SUBCASE("deep through p2") { session("p2", "p1"); } });
}

// The same applies to a second premise that leads into the cycle: (:k7 o)
// serves as a premise of the root's rule on two occasions -- once directly
// and once via (:y1 o) -- and a result for it identified through one route
// held it, cut at the limit, while the alternate route was searched.
TEST_CASE("explain: a depth limit never prints a fact below itself through a second premise")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto session = [&](const std::string& joined, const std::string& deep)
        {
            process_lines(interactive, R"(
(:p1 X) => (:r X)
(:p2 X) => (:r X)
:k7 o
:u o
(:z X) => (:u X)
(:k7 X) => (:k4 X)
(:k4 X) => (:k7 X)
(:k4 X) => (:x1 X)
(:k7 X) => (:y1 X)
)");
            interactive.process("(:y1 X, :k7 X) => (:" + joined + " X)");
            interactive.process("(:x1 X, :u X) => (:" + deep + " X)");
            interactive.run(true, false, false);
            for (std::size_t depth = 1; depth <= 6; ++depth)
            {
                CAPTURE(depth);
                collector.clear();
                interactive.process(".explain (:r o) " + std::to_string(depth));
                const std::string tree = explained(collector);
                CAPTURE(tree);
                CHECK(fact_under_itself(tree).empty());
                for (const std::size_t level : limit_levels(tree))
                    CHECK(level == depth);
            }
        };
        SUBCASE("joined through p2") { session("p2", "p1"); }
        SUBCASE("joined through p1") { session("p1", "p2"); } });
}

// A genuine session in which the stdlib's cycles meet the limit: eml terms
// whose simplification marks the same subterms from several directions. At
// limit 12, (&1 eml &1) simp e appeared beneath itself, and
// "[depth limit]" was positioned above the limit.
TEST_CASE("explain: a depth limit never prints a fact below itself on eml terms")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, ".import eml\nx ~ symvar");
    // Each eml term of these forms over the leaves &1 and x, in this
    // order: a eml b, a eml (b eml c), (a eml b) eml z.
    const std::vector<std::string> leaves{"&1", "x"};
    std::vector<std::string>       pairs;
    for (const auto& a : leaves)
        for (const auto& b : leaves)
            pairs.push_back("(" + a + " eml " + b + ")");
    std::vector<std::string> terms = pairs;
    for (const auto& a : leaves)
        for (const auto& bc : pairs)
            terms.push_back("(" + a + " eml " + bc + ")");
    std::vector<std::string> right = leaves;
    right.insert(right.end(), pairs.begin(), pairs.end());
    for (const auto& ab : pairs)
        for (const auto& z : right)
            terms.push_back("(" + ab + " eml " + z + ")");
    for (const auto& t : terms)
        interactive.process(":simplify " + t);
    interactive.process(".deductions off");

    for (const std::string depth : {"12", "8", "25"})
    {
        CAPTURE(depth);
        collector.clear();
        interactive.process(".explain (((&1 eml &1) eml (&1 eml x)) expandsto ((exp of e) - (ln of (e - (ln of x))))) " + depth);
        const std::string tree = explained(collector);
        CAPTURE(tree);
        REQUIRE(tree.find("expandsto") != std::string::npos);
        CHECK(fact_under_itself(tree).empty());
        for (const std::size_t level : limit_levels(tree))
            CHECK(level == std::stoul(depth));
    }
}

// In a cycle, the search met a fact initially at great depth, following a
// route that rests on an "asserted; no derivation found" leaf, and reused
// it at a more elevated level while the cycle was still being searched; the
// result discovered deep within persisted in place, and "[depth limit]"
// appeared two levels above the limit. A result that the limit cut is
// searched again at the position where the tree shows it at a higher level.
TEST_CASE("explain: in a cycle a subproof the limit cut is searched again where it stands higher up")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto session = [&](const std::string& deep, const std::string& high)
        {
            process_lines(interactive, R"(
(:p1 X) => (:n X)
(:p2 X) => (:n X)
:c a
:u a
(:k X) => (:u X)
(:c X) => (:q4 X)
(:q4 X) => (:q3 X)
(:q3 X) => (:q2 X)
(:q2 X) => (:q X)
(:q X) => (:m X)
(:n X) => (:m X)
(:m X) => (:y X)
(:y X) => (:x X)
)");
            interactive.process("(:x X, :u X) => (:" + deep + " X)");
            interactive.process("(:m X) => (:" + high + " X)");
            interactive.run(true, false, false);
            for (std::size_t depth = 4; depth <= 8; ++depth)
            {
                CAPTURE(depth);
                collector.clear();
                interactive.process(".explain (:n a) " + std::to_string(depth));
                const std::string tree = explained(collector);
                CAPTURE(tree);
                CHECK(fact_under_itself(tree).empty());
                for (const std::size_t level : limit_levels(tree))
                    CHECK(level == depth);
            }
        };
        SUBCASE("deep through p1") { session("p1", "p2"); }
        SUBCASE("deep through p2") { session("p2", "p1"); } });
}

// A fact that no rule concludes remains complete in its current form; at the
// limit it was nonetheless labelled "[depth limit]" if the search had not
// encountered it previously, and "[axiom]" if it had -- both within the same
// tree.
// When operating under a depth limit, the search reuses results discovered at
// lower levels, and when it searches such a fact again at a higher level, each
// instantiation depending on the prior result waits for the new one, ensuring
// no fact appears beneath itself. In the cyclic NAND arithmetic described in
// the paper, that left the whole component of (&3 - &3) = &0 without an
// instantiation its fixpoint could ground, at every limit from 10 to 28 and
// once more at 35 and 40, and the fact was printed as
// "[asserted; no derivation found]" -- its complete proof spans 29 levels. The
// complete proof is now searched first, and the limit cuts what portion is
// printed.
TEST_CASE("explain: a depth limit does not hide a derivation the full search finds" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".semi-naive on"); // In check mode, each run within the session would be validated through classic passes
    process_lines(interactive, R"(
.deductions off
.import binary-nand-arithmetic
(1 nand 1) out X
(0 nand 1) out X
((1 d+ 1) tci 1) co X
(&12 * &34) = X
.import primes-naf
(:testprime &13) = X
)");

    for (const std::size_t depth : {10, 20, 40})
    {
        CAPTURE(depth);
        collector.clear();
        interactive.process(".explain ((&3 - &3) = &0) " + std::to_string(depth));
        const std::string tree = explained(collector);
        CHECK(tree.rfind("(&3 - &3) = &0\n", 0) == 0);
        CHECK(deepest_level(tree) >= 10);
        CHECK(fact_under_itself(tree).empty());
        for (const std::size_t level : limit_levels(tree))
            CHECK(level == depth);
    }
}

// Under a depth limit, the tree is the top of the complete proof: the search
// runs without the limit, and the limit merely cuts what gets displayed. The
// search that stopped at the limit reused results discovered at lower
// levels, and whenever a fact was searched again at a higher level, an
// instantiation depending on a prior result remained pending until the
// updated result arrived. This left certain facts -- those the complete
// proof derives within the window -- shown as
// "[asserted; no derivation found]" (:a c below, reached via :p c and :q c
// at level 3), the identical fact displayed as derived and as such a leaf in
// one tree (:p3 a), and "[see above]" referencing another proof object for
// the same fact, which read as a circle (cutref1). Each program beneath
// represents a fuzz case reduced to the essential mechanism; their complete
// proofs do not exceed the limit in height, hence the tree is the depth-0
// tree.
TEST_CASE("explain: under a depth limit the tree is the top of the complete proof")
{
    struct Case
    {
        const char* name;
        const char* program;
        const char* fact;
        const char* depth;
    };
    const Case cases[] = {
        {"a fact the complete proof derives within the window", R"(
((:a X), (:b X)) => (:r X)
(:r X) => (:a X)
(:p X) => (:a X)
(:b X) => (:p X)
(:q X) => (:p X)
((:p X), (:r X)) => (:b X)
(:r X) => (:b X)
:r c
:q c
)",
         "(:r c)",
         "3"},
        {"one fact, derived and a leaf", R"(
(:p5 X) => (:p3 X)
(:p3 X) => (:p4 X)
((:p1 X), (:p4 X), (:p5 X)) => (:p4 X)
(:p2 X) => (:p5 X)
((:p3 X), (:p4 X)) => (:p2 X)
:p2 a
)",
         "(:p2 a)",
         "3"},
        {"one fact, derived and a leaf, at the default depth", R"(
((X r1 Y), (Y r2 Z)) => (X r1 Z)
((X r0 Y), X != Y) => (Y r1 X)
((X r0 Y), (X r1 Y)) => (Y r2 X)
(X r1 Y) => (X r0 Y)
a r0 c
)",
         "(c r1 c)",
         "4"},
        {"a reference to another proof of the same fact", R"(
(X r2 Y) => (X r1 X)
(X r0 Y) => (X r2 X)
((X r0 Y), (Y r2 Z)) => (X r0 Z)
(X r3 Y) => (X r1 Y)
(X r0 Y) => (Y r0 X)
(X r0 Y) => (Y r2 X)
a r0 b
c r0 a
)",
         "(a r0 a)",
         "3"},
        {"an expansion cycle through a cut and a reference", R"(
((:p0 X), (:p3 X), (:p4 X)) => (:p1 X)
(:p3 X) => (:p2 X)
(:p3 X) => (:p3 X)
(:p0 X) => (:p2 X)
((:p2 X), (:p0 X)) => (:p1 X)
((:p5 X), (:p3 X)) => (:p2 X)
(:p0 X) => (:p3 X)
(:p4 X) => (:p4 X)
((:p3 X), (:p5 X)) => (:p0 X)
((:p0 X), (:p2 X), (:p3 X)) => (:p1 X)
(:p2 X) => (:p3 X)
:p5 a
:p2 a
:p3 a
)",
         "(:p1 a)",
         "3"},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.name);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, c.program);
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(std::string(".explain ") + c.fact + " 0");
        const std::string complete = explained(collector);
        REQUIRE(deepest_level(complete) <= static_cast<std::size_t>(std::stoul(c.depth)));

        collector.clear();
        interactive.process(std::string(".explain ") + c.fact + " " + c.depth);
        CHECK(explained(collector) == complete);
        const auto counts = interactive.graph()->last_explain_counts();
        CHECK(counts.searches == counts.facts);
    }

    // When the complete proof exceeds the limit in height, the tree is its
    // top.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, cases[0].program);
    interactive.run(true, false, false);
    collector.clear();
    interactive.process(".explain (:r c) 2");
    CHECK(explained(collector) == ":r c\n"
                                  "   ├─ :a c\n"
                                  "   │  └─ :p c  … [depth limit -- use '.explain <pattern> 0' for the full proof]\n"
                                  "   └─ :b c  [asserted; no derivation found]");
}

// When the depth limit cut the expansion of a fact, that fact is re-expanded
// at the point where it stands highest in the tree. Where it stands highest
// was taken from the proof, breadth first, and in a proof that holds several
// results for one fact -- the search within the limit keeps a result found
// deeper down -- such a position may lie below a fact printed as "[see
// above]", where the tree shows nothing: the fact's highest line was then a
// "[see above]" pointing at the cut expansion, and the fact was expanded
// again nowhere. The search within the limit runs here since the budget for
// the complete proof is set to perform only one search.
TEST_CASE("explain: a fact cut where it was first expanded is expanded again at its highest printed line")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.import binary-arithmetic
.import primes
(:testprime &113) = X
.deductions off
)");
    interactive.graph()->set_explain_budget(1);
    collector.clear();
    interactive.process(".explain (<0111000> canon &56) 12");
    const std::string tree = explained(collector);
    REQUIRE(tree.rfind("<0111000> canon &56\n", 0) == 0);
    CHECK(any_output_contains(collector, "Note: the search for a proof whose leaves are all axioms ran out of its budget"));
    CHECK(highest_only_referenced(tree).empty());
    CHECK(fact_under_itself(tree).empty());
}

// The complete proof is searched within a budget of fact searches; a proof
// that needs more before the queried fact is justified is searched within
// the depth limit instead, and a note positioned above the tree says that
// the tree does not represent the top of the complete proof. The counts
// include both searches: a fact that both of them entered was entered twice.
// A budget of two searches stands in here for a proof too large to search
// whole; the tree in question is then the one discovered through the search
// within the limit, featuring :a c as a leaf it reached deeper down first
// (see the test above).
TEST_CASE("explain: a proof too large to search whole is searched within the limit, and the tree says so")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
((:a X), (:b X)) => (:r X)
(:r X) => (:a X)
(:p X) => (:a X)
(:b X) => (:p X)
(:q X) => (:p X)
((:p X), (:r X)) => (:b X)
(:r X) => (:b X)
:r c
:q c
)");
    interactive.run(true, false, false);
    const auto graph = interactive.graph();
    const auto noted = [&]
    { return any_output_contains(collector, "Note: the search for a proof whose leaves are all axioms ran out of its budget"); };

    graph->set_explain_budget(2);
    collector.clear();
    interactive.process(".explain (:r c) 3");
    CHECK(noted());
    CHECK(explained(collector) == ":r c\n"
                                  "   ├─ :a c  [asserted; no derivation found]\n"
                                  "   └─ :b c  [asserted; no derivation found]");
    const auto counts = graph->last_explain_counts();
    CHECK(counts.limited);
    CHECK(counts.facts == 4);
    CHECK(counts.searches > counts.facts);
    CHECK(counts.most == 2);

    // There is no fallback available when there is no limit: the budget
    // does not apply.
    collector.clear();
    interactive.process(".explain (:r c) 0");
    CHECK_FALSE(noted());
    CHECK_FALSE(graph->last_explain_counts().limited);

    // By default, the budget searches this proof
    // whole.
    graph->set_explain_budget(0);
    collector.clear();
    interactive.process(".explain (:r c) 3");
    CHECK_FALSE(noted());
    CHECK_FALSE(graph->last_explain_counts().limited);
}

// The budget limits the search for the proof of the queried fact, not the
// search for a second justification: after the fact has acquired a clean
// proof, the annotation's bound applies (refer to "looking for a second
// justification costs no more than the proof"). In this case, the clean proof
// takes two searches, while the circular alternative via the chain demands a
// hundred additional ones, which the annotation is permitted to expend; a
// budget of ten searches leaves the tree the top of the complete proof.
TEST_CASE("explain: the budget ends where the queried fact holds a clean proof")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    std::string                 program  = ".deductions off\n(X nx Y, X st good) => (Y st good)\n(X st src) => (X st good)\nr st src\n";
    std::string                 previous = "r";
    for (int i = 0; i < 50; ++i)
    {
        program += previous + " nx c" + std::to_string(i) + "\n";
        previous = "c" + std::to_string(i);
    }
    program += previous + " nx r\n";
    process_lines(interactive, program);
    interactive.run(true, false, false);

    const auto graph = interactive.graph();
    graph->set_explain_budget(10);
    collector.clear();
    interactive.process(".explain (r st good) 4");
    CHECK(explained(collector) == "r st good\n"
                                  "   └─ r st src  [axiom]");
    CHECK_FALSE(any_output_contains(collector, "Note:"));
    CHECK_FALSE(graph->last_explain_counts().limited);
}

// The budget counts work -- the number of matches enumerated, the premises
// reached, the steps in a component's fixpoint -- but not the facts the
// search enters: a fact from a dense component incurs the same cost as its
// instantiations do. The transitive closure of a ring of a hundred forms
// one component comprising ten thousand facts, each the conclusion of a
// hundred instantiations, and under a depth limit of 1, its complete
// search took 2.6 s while remaining under 20 000 facts. Under any depth
// limit, the search for the complete proof now stops at the budget, and
// the tree is searched within the limit.
TEST_CASE("explain: a dense component under a depth limit costs bounded work" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".deductions off");
    const auto        graph = interactive.graph();
    const std::size_t n     = 100;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
            assert_fact(graph, "c" + std::to_string(i), "sub", "c" + std::to_string(j));
    interactive.process("(X sub Y, Y sub Z) => (X sub Z)");

    for (const char* depth : {"1", "4"})
    {
        CAPTURE(depth);
        collector.clear();
        const auto start = std::chrono::steady_clock::now();
        interactive.process(std::string(".explain (c0 sub c1) ") + depth);
        const double seconds = seconds_since(start);
        CAPTURE(seconds);
        CHECK(seconds < 1.5);
        const auto counts = graph->last_explain_counts();
        CHECK(counts.limited);
        CHECK(counts.work <= graph->explain_budget() + graph->explain_budget() / 10);
        CHECK(any_output_contains(collector, "Note: the search for a proof whose leaves are all axioms ran out of its budget"));
        CHECK(explained(collector).rfind("c0 sub c1", 0) == 0);
    }
}

// A condition that the join enumerates, and a negation that the search
// checks, start from the adjacency of the node they are anchored at, and
// reading it is work like any other: the facts of the condition's predicate
// are extracted from all the node's facts. Only the matches were counted,
// meaning a negation anchored at a hub read a hundred thousand adjacency
// entries per check for a single unit: a default-depth explanation took 5 s
// at 10 002 units and never fell back to the search within the limit. In this
// case, every step of a chain of a hundred checks a condition at a hub
// containing ten thousand facts from a different predicate, which a budget of
// 5 000 units is unable to cover. Without the hub, the same proof is searched
// whole, and the tree within the limit remains unchanged.
TEST_CASE("explain: the adjacency a condition is read from counts against the budget")
{
    struct Session
    {
        std::size_t whole{0}; // the work of the complete proof
        bool        limited{false};
        bool        noted{false};
        std::string tree; // at a depth of 4, within a budget of 5 000
    };
    const auto session = [](const std::string& rule, const std::string& matched, const bool hub)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n" + rule + "\n" + matched + "\n");
        auto* const           graph  = interactive.graph();
        constexpr std::size_t length = 100;
        for (std::size_t i = 0; i < length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "next", "n" + std::to_string(i + 1));
        for (std::size_t i = 0; i <= length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "ok", "yes");
        constexpr std::size_t hub_size = 10000;
        for (std::size_t k = 0; hub && k < hub_size; ++k)
            assert_fact(graph, "h", "other", "k" + std::to_string(k));

        Session s;
        interactive.process(".explain (n0 ok yes) 0");
        s.whole = graph->last_explain_counts().work;

        graph->set_explain_budget(5000);
        collector.clear();
        interactive.process(".explain (n0 ok yes) 4");
        s.limited = graph->last_explain_counts().limited;
        s.noted   = any_output_contains(collector, "Note: the search for a proof whose leaves are all axioms ran out of its budget");
        s.tree    = explained(collector);
        return s;
    };
    const auto check = [&](const std::string& rule, const std::string& matched)
    {
        const Session with = session(rule, matched, true);
        CAPTURE(with.whole);
        CHECK(with.whole >= 100 * (10000 / 64));
        CHECK(with.limited);
        CHECK(with.noted);

        const Session without = session(rule, matched, false);
        CAPTURE(without.whole);
        CHECK(without.whole < 5000);
        CHECK(with.whole < without.whole + 150 * (10000 / 64));
        CHECK_FALSE(without.limited);
        CHECK_FALSE(without.noted);
        CHECK(with.tree == without.tree);
    };
    SUBCASE("a negation") { check("((X next Y), (Y ok yes), ¬(h bad X)) => (X ok yes)", "zz bad qq"); }
    SUBCASE("a condition the join enumerates") { check("((X next Y), (Y ok yes), (h tag T)) => (X ok yes)", "h tag t0"); }
    // At the (h tag T) level, a match left exists as the search proceeds to
    // (n1 ok yes), and the search returns to it: there is no proof of the
    // chain where all leaves are axioms. A level having only a few remaining
    // candidates takes their matches in advance, so the hub is read just
    // once per step; set aside, it would be read again when the search
    // returns.
    SUBCASE("a condition with a match left") { check("((X next Y), (Y ok yes), (h tag T)) => (X ok yes)", "h tag t0\nh tag t1"); }
}

namespace
{
    // The `predicate`'s facts that a condition in the rules below finds
    // among its candidates and cannot match: the k-th one, in terms of
    // shape.
    using HubFacts = std::function<void(zelph::network::Reasoning*, std::size_t, const std::string&)>;

    // (h `predicate` (k p q)), where the condition asks for (h tag (a p T)).
    const HubFacts nested_objects = [](zelph::network::Reasoning* graph, const std::size_t k, const std::string& predicate)
    {
        const std::string lang  = graph->lang();
        const auto        inner = graph->fact(graph->node("k" + std::to_string(k), lang), graph->node("p", lang), {graph->node("q", lang)});
        graph->fact(graph->node("h", lang), graph->node(predicate, lang), {inner});
    };

    // (z `predicate` h) and (z `predicate` k), where the condition asks for
    // (Z tag h k): one object each, at any location where the condition is
    // anchored.
    const HubFacts single_objects = [](zelph::network::Reasoning* graph, const std::size_t k, const std::string& predicate)
    {
        assert_fact(graph, "z" + std::to_string(k), predicate, "h");
        assert_fact(graph, "z" + std::to_string(k), predicate, "k");
    };

    // (k `predicate` q), with the condition requiring (T tag T).
    const HubFacts distinct_ends = [](zelph::network::Reasoning* graph, const std::size_t k, const std::string& predicate)
    { assert_fact(graph, "k" + std::to_string(k), predicate, "q" + std::to_string(k)); };
}

// The candidates of a condition are the facts associated with its predicate
// at the node where it is anchored, and a candidate may possess the anchor in
// the role assigned by the condition yet still fail to match: its object
// differs from the requested statement, it has a different number of objects,
// or the variable repeated by the condition would represent two distinct
// nodes. Trying such a candidate incurs a cost several times greater than
// reading it, and only the act of reading was included in the count: a chain
// spanning a thousand steps, each trying ten thousand candidates, fell back
// to the search within the limit after 80 000 units and 1.1 seconds, and at a
// hundred thousand candidates per step after 2.5 seconds. In this case, each
// step in a chain of a hundred tries two thousand candidates in vain, a
// burden the budget of 10 000 units cannot sustain. The same facts under a
// different predicate are read the same way and subjected to no attempts, and
// the proof is searched whole.
TEST_CASE("explain: the candidates a condition tries in vain count against the budget")
{
    struct Session
    {
        std::size_t whole{0}; // the work of the complete proof
        bool        limited{false};
        bool        noted{false};
    };
    const auto session = [](const std::string& rule, const std::string& matched, const HubFacts& hub_facts, const std::string& predicate)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n" + rule + "\n" + matched + "\n");
        auto* const           graph  = interactive.graph();
        constexpr std::size_t length = 100;
        for (std::size_t i = 0; i < length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "next", "n" + std::to_string(i + 1));
        for (std::size_t i = 0; i <= length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "ok", "yes");
        for (std::size_t k = 0; k < 2000; ++k)
            hub_facts(graph, k, predicate);

        Session s;
        interactive.process(".explain (n0 ok yes) 0");
        s.whole = graph->last_explain_counts().work;

        graph->set_explain_budget(10000);
        collector.clear();
        interactive.process(".explain (n0 ok yes) 4");
        s.limited = graph->last_explain_counts().limited;
        s.noted   = any_output_contains(collector, "Note: the search for a proof whose leaves are all axioms ran out of its budget");
        CHECK(explained(collector).rfind("n0 ok yes\n", 0) == 0);
        return s;
    };
    const auto check = [&](const std::string& rule, const std::string& matched, const HubFacts& hub_facts)
    {
        const Session tried = session(rule, matched, hub_facts, "tag");
        const Session other = session(rule, matched, hub_facts, "other");
        CAPTURE(tried.whole);
        CAPTURE(other.whole);
        CHECK(tried.whole > 4 * other.whole);
        CHECK(tried.limited);
        CHECK(tried.noted);
        CHECK_FALSE(other.limited);
        CHECK_FALSE(other.noted);
    };
    SUBCASE("a nested object") { check("((X next Y), (Y ok yes), (h tag (a p T))) => (X ok yes)", "h tag (a p t0)", nested_objects); }
    SUBCASE("a nested object under a negation") { check("((X next Y), (Y ok yes), ¬(h tag (X p T))) => (X ok yes)", "zz tag qq", nested_objects); }
    SUBCASE("another number of objects") { check("((X next Y), (Y ok yes), (Z tag h k)) => (X ok yes)", "w tag h k", single_objects); }
    SUBCASE("a repeated variable") { check("((X next Y), (Y ok yes), (T tag T)) => (X ok yes)", "t0 tag t0", distinct_ends); }
}

namespace
{
    // A chain of a thousand steps, each checking `rule`'s condition at the
    // hub h, and ten thousand facts pertaining to h in the role that the
    // condition does not assign: (k_i `predicate` h) when the condition lists
    // h as its subject, (h `predicate` k_i) when it lists h as its object.
    // The time and memory usage of the depth-0 explanation are compared with
    // those from the same session where those facts possess a different
    // predicate: the hub's adjacency is read the same way there, for the same
    // units, and nothing in it is tried.
    void check_hub_in_other_role(const std::string& rule, const std::string& matched, const std::string& predicate, const bool hub_is_subject)
    {
        const auto session = [&](const std::string& hub_predicate, long& grown)
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, ".deductions off\n" + rule + "\n" + matched + "\n");
            auto* const           graph  = interactive.graph();
            constexpr std::size_t length = 1000;
            for (std::size_t i = 0; i < length; ++i)
                assert_fact(graph, "n" + std::to_string(i), "next", "n" + std::to_string(i + 1));
            for (std::size_t i = 0; i <= length; ++i)
                assert_fact(graph, "n" + std::to_string(i), "ok", "yes");
            for (std::size_t k = 0; k < 10000; ++k)
            {
                if (hub_is_subject)
                    assert_fact(graph, "h", hub_predicate, "k" + std::to_string(k));
                else
                    assert_fact(graph, "k" + std::to_string(k), hub_predicate, "h");
            }

            double best = 1e9;
            for (int round = 0; round < 3; ++round)
            {
                const auto explain = [&]
                {
                    const auto start = std::chrono::steady_clock::now();
                    interactive.process(".explain (n0 ok yes) 0");
                    best = std::min(best, seconds_since(start));
                };
#ifdef __linux__
                if (round == 0)
                {
                    grown = peak_growth_kb(explain);
                    continue;
                }
#endif
                explain();
            }
            CHECK(explained(collector).rfind("n0 ok yes\n", 0) == 0);
            return best;
        };
        long         grown_other = 0;
        long         grown_same  = 0;
        const double other       = session("other", grown_other);
        const double same        = session(predicate, grown_same);
        CAPTURE(other);
        CAPTURE(same);
        CHECK(same < 3 * other);
        CAPTURE(grown_other);
        CAPTURE(grown_same);
        CHECK(grown_same < 64 * 1024);
        CHECK(grown_same < grown_other + 16 * 1024);
    }
}

// The identical chain, with the hub serving as the object of ten thousand
// facts tied to the specific predicate that the condition anchored at it asks
// for. The hub's adjacency is read as before, for the same units, yet no fact
// it is solely the object of can be matched. Each was tried and dismissed
// regardless, requiring eight times the duration of the reading the units
// represented. And a join holds its candidates while the search processes
// each match: once the matches were taken individually, every open step in
// the proof held all of them -- for a proof stretching a thousand steps at a
// hub of a hundred thousand, amounting to 1.8 GB. A hub of a different
// predicate is read in the same way and tries nothing, thus serving as the
// measure.
TEST_CASE("explain: the facts a hub is only the object of are neither tried nor held" * doctest::test_suite("slow"))
{
    SUBCASE("a negation") { check_hub_in_other_role("((X next Y), (Y ok yes), ¬(h bad X)) => (X ok yes)", "zz bad qq", "bad", false); }
    SUBCASE("a condition the join enumerates") { check_hub_in_other_role("((X next Y), (Y ok yes), (h tag T)) => (X ok yes)", "h tag t0", "tag", false); }
}

// In reverse: a condition with a free subject starts from its object, which
// here is a hub, the subject of ten thousand facts tied to the condition's
// predicate. None of these facts can match, and they were tried and held
// just as the prior facts were: a thousand steps at a hub of ten thousand
// held 230 MB, and a hundred thousand held 1.8 GB, compared to 28 MB when
// the hub's facts had a different predicate; the negation took ten times
// the duration.
TEST_CASE("explain: the facts a hub is only the subject of are neither tried nor held" * doctest::test_suite("slow"))
{
    SUBCASE("a negation") { check_hub_in_other_role("((X next Y), (Y ok yes), ¬(Z bad h)) => (X ok yes)", "zz bad qq", "bad", true); }
    SUBCASE("a condition the join enumerates") { check_hub_in_other_role("((X next Y), (Y ok yes), (Z bad h)) => (X ok yes)", "w bad h", "bad", true); }
}

#ifdef __linux__
// The candidates a condition cannot match even though the anchor has its role
// in them (refer to "the candidates a condition tries in vain count against
// the budget"). A level of a join takes its matches sequentially, remaining
// open as the search works on the premise that the match provided, and it
// held every candidate yet to come: a chain spanning a thousand steps, each
// involving ten thousand of them, held 220 MB to 270 MB at depth 0, where the
// search that tried them all before securing a match, and kept the matches,
// held 25 MB to 30 MB. A level keeps nothing it cannot match while a premise
// is being searched now. The same facts under a different predicate are not
// included among the candidates, so that session serves as the measure.
TEST_CASE("explain: the candidates a condition cannot match are not held while its premise is searched" * doctest::test_suite("slow"))
{
    const auto check = [](const std::string& rule, const std::string& matched, const HubFacts& hub_facts)
    {
        const auto grown = [&](const std::string& predicate)
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, ".deductions off\n" + rule + "\n" + matched + "\n");
            auto* const           graph  = interactive.graph();
            constexpr std::size_t length = 1000;
            for (std::size_t i = 0; i < length; ++i)
                assert_fact(graph, "n" + std::to_string(i), "next", "n" + std::to_string(i + 1));
            for (std::size_t i = 0; i <= length; ++i)
                assert_fact(graph, "n" + std::to_string(i), "ok", "yes");
            for (std::size_t k = 0; k < 10000; ++k)
                hub_facts(graph, k, predicate);
            const long kb = peak_growth_kb([&]
                                           { interactive.process(".explain (n0 ok yes) 0"); });
            CHECK(explained(collector).rfind("n0 ok yes\n", 0) == 0);
            return kb;
        };
        const long other = grown("other");
        const long tried = grown("tag");
        CAPTURE(other);
        CAPTURE(tried);
        CHECK(tried < other + 16 * 1024);
    };
    SUBCASE("a nested object") { check("((X next Y), (Y ok yes), (h tag (a p T))) => (X ok yes)", "h tag (a p t0)", nested_objects); }
    SUBCASE("another number of objects") { check("((X next Y), (Y ok yes), (Z tag h k)) => (X ok yes)", "w tag h k", single_objects); }
    SUBCASE("a repeated variable") { check("((X next Y), (Y ok yes), (T tag T)) => (X ok yes)", "t0 tag t0", distinct_ends); }
}
#endif

// What a condition anchored at its object must still take: a fact in which
// its object serves as the subject solely when the object is also one of its
// own objects. A fact can have its subject among its objects only as its sole
// object, (h bad h); its edges then show no object apart from the subject.
// Yet a fact that is used as a predicate, as (h bad h) is within (x (h bad h)
// y), possesses an edge originating from each of its users as well, depicted
// similarly to how an object's edge is drawn, and the statement is read apart
// from them. Here (h bad h) matches (Z bad h), with Z = h, beside such a user
// as well, whereas (h bad k) does not: the negation is defeated by the first
// and holds alongside the second, and the join finds its match exclusively in
// the first.
TEST_CASE("explain: a condition anchored at its object takes a fact its object is the subject of only where it is the fact's object too")
{
    const auto tree = [](const std::string& rule, const std::string& matched, const std::string& hub_facts)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n" + rule + "\n" + matched + "\n" + hub_facts + "n0 next n1\nn1 ok yes\nn0 ok yes\n");
        collector.clear();
        interactive.process(".explain (n0 ok yes) 0");
        return explained(collector);
    };
    const std::string none = "n0 ok yes  [asserted; no derivation found]";
    SUBCASE("a negation")
    {
        const std::string rule = "((X next Y), (Y ok yes), ¬(Z bad h)) => (X ok yes)";
        CHECK(tree(rule, "zz bad qq", "h bad k\n") == "n0 ok yes\n"
                                                      "   ├─ n1 ok yes  [asserted; no derivation found]\n"
                                                      "   ├─ n0 next n1  [axiom]\n"
                                                      "   └─ ¬(Z bad h)  [absent]");
        CHECK(tree(rule, "zz bad qq", "h bad h\n") == none);
        CHECK(tree(rule, "zz bad qq", "h bad h\nx (h bad h) y\n") == none);
    }
    SUBCASE("a condition the join enumerates")
    {
        const std::string rule    = "((X next Y), (Y ok yes), (Z bad h)) => (X ok yes)";
        const std::string derived = "n0 ok yes\n"
                                    "   ├─ n1 ok yes  [asserted; no derivation found]\n"
                                    "   ├─ n0 next n1  [axiom]\n"
                                    "   └─ :bad h  [axiom]";
        CHECK(tree(rule, "", "h bad k\n") == none);
        CHECK(tree(rule, "", "h bad h\n") == derived);
        CHECK(tree(rule, "", "h bad h\nx (h bad h) y\n") == derived);
    }
}

// A statement within a condition matches a wider one too, whose objects
// include its own: (a p b c) states (a p b), hence ((a p b) bad Z) is valid
// for ((a p b c) bad (a p b)), and (Z bad (a p b)) is valid for ((a p b) bad
// (a p b c)). A condition anchored at a statement -- a pattern the join's
// bindings ground, or a variable bound to one -- can thus match a fact where
// the statement plays the alternate role, and only at an atom does the role
// decide whether a fact can match. Taking solely the facts where the grounded
// (a p b) serves as the subject, the negation held beside ((a p b c) bad (a p
// b)), and the explanation printed it as absent, where every evaluation of
// the rule finds it matched.
TEST_CASE("explain: a condition anchored at a statement takes the facts that match it through a wider one")
{
    const auto tree = [](const std::string& rule, const std::string& facts)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n" + rule + "\nzz bad qq\nn0 next n1\nn1 ok yes\nn0 ok yes\n" + facts);
        collector.clear();
        interactive.process(".explain (n0 ok yes) 0");
        return explained(collector);
    };
    const std::string none = "n0 ok yes  [asserted; no derivation found]";
    SUBCASE("at its subject")
    {
        const std::string rule = "((X next Y), (Y ok yes), (X w W), ¬((a p W) bad Z)) => (X ok yes)";
        CHECK(tree(rule, "n0 w b\na p b\n(a p b c) bad (a p b)\n") == none);
        CHECK(tree(rule, "n0 w b\na p b\n") == "n0 ok yes\n"
                                               "   ├─ n0 next n1  [axiom]\n"
                                               "   ├─ n1 ok yes  [asserted; no derivation found]\n"
                                               "   ├─ n0 w b  [axiom]\n"
                                               "   └─ ¬((a p b) bad Z)  [absent]");
    }
    SUBCASE("at its object")
    {
        const std::string rule = "((X next Y), (Y ok yes), (X w V), ¬(Z bad V)) => (X ok yes)";
        CHECK(tree(rule, "n0 w (a p b)\n(a p b) bad (a p b c)\n") == none);
        CHECK(tree(rule, "n0 w (a p b)\n(a p b) bad k\n") == "n0 ok yes\n"
                                                             "   ├─ n1 ok yes  [asserted; no derivation found]\n"
                                                             "   ├─ n0 next n1  [axiom]\n"
                                                             "   ├─ n0 w (a p b)  [axiom]\n"
                                                             "   └─ ¬(Z bad (a p b))  [absent]");
    }
}

// A proof whose magnitude matches that of the documentation's own remains
// within the allocated budget, and its tree is the top of the complete proof.
// The polynomial identity presented in the mathematics overview takes 2 458
// fact searches, the highest number among all explanations in the
// documentation's sessions except for those involving the Jacobian.
TEST_CASE("explain: a proof the size of the documentation's is searched whole" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.import math
<x> ~ polyring
? $( (1+x)*(1-x) ) ≡ $( 1 - x^2 )
<a1 a2 a3 a4 b1 b2 b3 b4> ~ polyring
? $( (a1^2+a2^2+a3^2+a4^2) * (b1^2+b2^2+b3^2+b4^2) ) ≡ $( (a1*b1-a2*b2-a3*b3-a4*b4)^2 + (a1*b2+a2*b1+a3*b4-a4*b3)^2 + (a1*b3-a2*b4+a3*b1+a4*b2)^2 + (a1*b4+a2*b3-a3*b2+a4*b1)^2 )
? $( x^2-1 ) ≡ $( (x-1)*(x+1) )
)");
    collector.clear();
    interactive.process(".explain 3");
    CHECK_FALSE(any_output_contains(collector, "Note:"));
    CHECK_FALSE(interactive.graph()->last_explain_counts().limited);
    const std::string cut = "  … [depth limit -- use '.explain <pattern> 0' for the full proof]";
    CHECK(explained(collector) == "($( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))) = proven\n"
                                  "   ├─ $( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))  [axiom]\n"
                                  "   ├─ (:topoly $( x ^ &2 - &1 )) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)\n"
                                  "   │  ├─ $( x ^ &2 - &1 ) aspoly (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)\n"
                                  "   │  │  ├─ :needstopoly $( x ^ &2 - &1 )"
                                      + cut + "\n"
                                              "   │  │  ├─ ((x poly <(pos zint &0) (pos zint &0) (pos zint &1)>) psub (pos zint &1)) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)"
                                      + cut + "\n"
                                              "   │  │  ├─ &1 aspoly (pos zint &1)"
                                      + cut + "\n"
                                              "   │  │  └─ (x ^ &2) aspoly (x poly <(pos zint &0) (pos zint &0) (pos zint &1)>)"
                                      + cut + "\n"
                                              "   │  └─ :topoly $( x ^ &2 - &1 )\n"
                                              "   │     └─ $( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))  [axiom]\n"
                                              "   └─ (:topoly ((x - &1) * (x + &1))) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)\n"
                                              "      ├─ ((x - &1) * (x + &1)) aspoly (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)\n"
                                              "      │  ├─ :needstopoly ((x - &1) * (x + &1))"
                                      + cut + "\n"
                                              "      │  ├─ ((x poly <(neg zint &1) (pos zint &1)>) pmul (x poly <(pos zint &1) (pos zint &1)>)) = (x poly <(neg zint &1) (pos zint &0) (pos zint &1)>)"
                                      + cut + "\n"
                                              "      │  ├─ (x + &1) aspoly (x poly <(pos zint &1) (pos zint &1)>)"
                                      + cut + "\n"
                                              "      │  └─ (x - &1) aspoly (x poly <(neg zint &1) (pos zint &1)>)"
                                      + cut + "\n"
                                              "      └─ :topoly ((x - &1) * (x + &1))\n"
                                              "         └─ $( x ^ &2 - &1 ) ≡ ((x - &1) * (x + &1))  [axiom]");
}

// The tree searched within the limit may contain multiple results for a
// single fact, and a "[see above]" referred, through the fact, to
// whichever was printed last: in this case, an expansion of (c r1 a)
// that shows (c r0 a), an ancestor of the line, positioned beneath it --
// a derivation that reads as a circle. A line points exclusively to an
// expansion of the same result now; a distinct result of the fact is
// printed as itself.
TEST_CASE("explain: in a tree searched within the limit [see above] refers to the same result")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
((X r0 Y), (Y r0 Z), X != Y) => (X r1 Z)
(X r1 Y) => (Y r0 X)
((X r0 Y), (X r1 Y)) => (Y r2 X)
(X r1 Y) => (X r0 Y)
b r0 a
c r0 b
)");
    interactive.run(true, false, false);
    interactive.graph()->set_explain_budget(1);
    collector.clear();
    interactive.process(".explain (a r2 c) 3");
    const std::string tree = explained(collector);
    CHECK(any_output_contains(collector, "Note: the search for a proof whose leaves are all axioms ran out of its budget"));
    CHECK(see_above_through_ancestor(tree).empty());
    CHECK(fact_under_itself(tree).empty());
}

TEST_CASE("explain: an axiom at the depth limit is labelled an axiom")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(:hd X, :gf X) => (:r X)
:c o
(:c X) => (:g2 X)
(:g2 X) => (:gf X)
(:gf X) => (:hd X)
(X val Y) => (X hasval Y)
x val 7
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (:r o)");
        const std::string tree = explained(collector);
        CAPTURE(tree);
        CHECK(tree.find(":c o  … [depth limit") == std::string::npos);
        CHECK(tree.find(":c o  [axiom]") != std::string::npos);

        collector.clear();
        interactive.process(".explain x hasval 7 1");
        CHECK(explained(collector) == "x hasval 7\n   └─ x val 7  [axiom]"); });
}

TEST_CASE("explain: a premise first reached through a cycle is proved another way")
{
    // (:p a) possesses two routes of derivation: one originating from a
    // premise grounded in the asserted (:s a), and another stemming from a
    // premise that only (:w a) derives -- the fact currently under proof
    // construction. Upon encountering the cyclic route first, the search
    // abandoned that premise, though it retained the instantiation: the proof
    // reported the premise as "asserted; no derivation found". The proof of
    // Eq. (5) in the EML paper met this once eml had been reduced per its
    // definition.
    //
    // The search examines rules in ascending sequence according to their node
    // ids, and a rule's identifier is a hash of its components, meaning the
    // order in which rules are entered does not decide which route the search
    // meets first. The two variants assign identical identifiers to the
    // competing rules, (:q1 X) => (:p X) and (:q2 X) => (:p X), since each
    // name is created in the same order, and they interchange which one
    // becomes the cyclic element, thus ensuring that one variant meets the
    // cycle first regardless of the hash value.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& grounded, const std::string& cyclic)
        {
            process_lines(interactive, "(:q1 X) => (:p X)\n(:q2 X) => (:p X)\n(:s X) => (:" + grounded + " X)\n(:w X) => (:" + cyclic + " X)\n(:p X) => (:w X)\n:s a\n");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (:w a) 0");
            CHECK(explained(collector) == ":w a\n"
                                          "   └─ :p a\n"
                                          "      └─ :" + grounded + " a\n"
                                          "         └─ :s a  [axiom]");
        };
        SUBCASE("q2 grounded") { check("q2", "q1"); }
        SUBCASE("q1 grounded") { check("q1", "q2"); } });
}

// A cycle with no external support: (:b a) is asserted, (:g a) is derived from
// it, and each rule also derives the one from the other, meaning every
// derivation the search finds for either one passes through the other. Without
// provenance, there is no indication of which one was asserted. The member via
// which the search entered the cycle shows the first instantiation it found on
// the other members, and these end the proof as leaves,
// "[asserted; no derivation found]": they hold, and every derivation found for
// them runs back through the cycle. Asked from either endpoint, that amounts
// to a single step to the opposite endpoint, and no output appears that
// traverses the cycle. The rule used to make the entry member itself the leaf,
// which answered the derived (:g a) as "asserted" even though the asserted
// fact it derives from was just one step away, and similarly for every derived
// symmetric, transitive, or inherited fact within a cycle (see the cases
// below).
TEST_CASE("explain: a cycle without support from outside ends one step after the fact the search entered it by")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(:g X) => (:b X)
(:b X) => (:g X)
(:g X) => (:h X)
:b a
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (:b a) 0");
        CHECK(explained(collector) == ":b a\n   └─ :g a  [asserted; no derivation found]");

        collector.clear();
        interactive.process(".explain (:g a) 0");
        CHECK(explained(collector) == ":g a\n   └─ :b a  [asserted; no derivation found]");

        collector.clear();
        interactive.process(".explain (:h a) 0");
        CHECK(explained(collector) == R"(:h a
   └─ :g a
      └─ :b a  [asserted; no derivation found])"); });
}

// The identical situation beneath the root: (:p6 a) and (:p7 a) mutually
// derive one another, with only (:p7 a) being asserted. The search enters the
// cycle at (:p6 a), progressing toward (:p0 a), and (:p6 a) previously ended
// there as "[asserted; no derivation found]".
TEST_CASE("explain: a cycle entered below the root shows the step into it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
:p0 a
:p7 a
(:p6 X) => (:p7 X)
(:p0 X, :p6 X) => (:p5 X)
(:kp0 X) => (:p0 X)
(:kp7 X) => (:p7 X)
(:p6 X) => (:p0 X)
(:p7 X) => (:p6 X)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (:p5 a) 0");
        const std::string tree = explained(collector);
        CHECK(tree.find(":p6 a  [asserted; no derivation found]") == std::string::npos);
        CHECK(tree.find(":p7 a  [asserted; no derivation found]") != std::string::npos);
        CHECK(fact_under_itself(tree).empty()); });
}

// Facts of the type that the Wikidata rules derive, each within a cycle
// involving the fact from which it is derived: the mirror of a symmetric
// statement, a membership passed down through a subclass cycle, and a
// transitive edge next to a cycle -- with a fixed predicate and with the
// variable predicate from the transitivity rule. Each was answered
// "[asserted; no derivation found]", even though the facts it is derived
// from are asserted and located just one step away.
TEST_CASE("explain: a derived fact in a cycle shows the instantiation that derives it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a symmetric predicate named by a fact")
        {
            process_lines(interactive, R"(
(R is symmetric, X R Y) => (Y R X)
friend is symmetric
alice friend bob
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (bob friend alice) 0");
            const std::string tree = explained(collector);
            CHECK(tree.rfind("bob friend alice\n", 0) == 0);
            CHECK(tree.find("alice friend bob  [asserted; no derivation found]") != std::string::npos);
            CHECK(tree.find("friend is symmetric") != std::string::npos);
        }
        SUBCASE("a symmetric rule")
        {
            process_lines(interactive, R"(
(X "is opposite of" Y) => (Y "is opposite of" X)
hot "is opposite of" cold
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (cold \"is opposite of\" hot) 0");
            CHECK(explained(collector) == "cold \"is opposite of\" hot\n   └─ hot \"is opposite of\" cold  [asserted; no derivation found]");
        }
        SUBCASE("a membership inherited over a subclass cycle")
        {
            process_lines(interactive, R"(
(X ~ K, K "is subclass of" U) => (X ~ U)
k1 "is subclass of" k2
k2 "is subclass of" k1
k2 "is subclass of" k3
item ~ k1
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (item ~ k3) 0");
            const std::string tree = explained(collector);
            CHECK(tree.find("item ~ k2  [asserted") == std::string::npos);
            CHECK(tree.find("item ~ k1  [asserted; no derivation found]") != std::string::npos);
            CHECK(tree.find("k2 \"is subclass of\" k3  [axiom]") != std::string::npos);
        }
        SUBCASE("a transitive edge next to a cycle")
        {
            process_lines(interactive, R"(
(X sub Y, Y sub Z) => (X sub Z)
a sub b
b sub a
b sub c
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (a sub c) 0");
            const std::string tree = explained(collector);
            CHECK(tree.rfind("a sub c\n", 0) == 0);
            CHECK(fact_under_itself(tree).empty());
        }
        SUBCASE("a transitive edge next to a cycle, with the predicate a variable")
        {
            process_lines(interactive, R"(
(R ~ "transitive relation", X R Y, Y R Z) => (X R Z)
P279 ~ "transitive relation"
q1 P279 q2
q2 P279 q3
q3 P279 q2
)");
            interactive.run(true, false, false);

            for (const char* depth : {"0", "4"})
            {
                CAPTURE(depth);
                collector.clear();
                interactive.process(std::string(".explain (q1 P279 q3) ") + depth);
                const std::string tree = explained(collector);
                CHECK(tree.rfind("q1 P279 q3\n", 0) == 0);
                CHECK(tree.find("q1 P279 q2") != std::string::npos);
                CHECK(fact_under_itself(tree).empty());
            }
        } });
}

TEST_CASE("explain: a premise that is not well-founded is not searched again on every path")
{
    // Investigating every potential instantiation, as necessitated by the
    // case above, revealed a premise that lacked a well-founded proof along a
    // particular path, leading to a fresh search initiated from the beginning
    // each time a different path reached it: only a well-founded proof was
    // kept. The simplification of the compiled eml form of x / y marks the
    // same subterms from multiple directions, and with the stdlib available
    // at that time, the complete proof for its outcome did not finish within
    // five minutes; depth 90 took 55 s. Each fact is now searched exactly
    // once per explanation, and interdependent facts are resolved together,
    // as a strongly connected component.
    //
    // Due to the current stdlib, this input no longer separates the per-path
    // search from the one currently present (about 3 s against 0.2 s per
    // explain), meaning the time bound here serves only to prevent a gross
    // regression. The work itself is pinned by "every intermediate result is
    // searched once", on inputs that do not require the stdlib. This case still
    // enforces the tree structure at both depths: the result, absence of a
    // "asserted; no derivation found" leaf, and no depth limit exceeding the 90
    // levels limit.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const std::string compiled = "(((&1 eml ((&1 eml (&1 eml ((&1 eml x) eml &1))) eml &1)) eml (((&1 eml ((&1 eml (&1 eml ((&1 eml &1) eml &1))) eml &1)) eml ((&1 eml ((&1 eml (((&1 eml ((&1 eml (&1 eml ((&1 eml &1) eml &1))) eml &1)) eml ((&1 eml ((&1 eml y) eml &1)) eml &1)) eml &1)) eml &1)) eml &1)) eml &1)) eml &1)";
    process_lines(interactive, R"(
.import eml
x ~ symvar
y ~ symvar
)");
    interactive.process("? :simplify " + compiled);
    REQUIRE(any_output_contains(collector, ") = (exp of ((ln of x) - (ln of y)))"));

    for (const std::string depth : {"90", "0"})
    {
        CAPTURE(depth);
        collector.clear();

        const auto start = std::chrono::steady_clock::now();
        interactive.process(".explain (" + compiled + " simp (exp of ((ln of x) - (ln of y)))) " + depth);
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(elapsed.count() < 10.0);
        CHECK(any_output_contains(collector, "simp (exp of ((ln of x) - (ln of y)))"));
        CHECK_FALSE(any_output_contains(collector, "no derivation found"));
        CHECK_FALSE(any_output_contains(collector, "depth limit"));
    }
}

TEST_CASE("explain: an asserted premise ends a proof, the path does not")
{
    // (:n a) possesses two derivations: one that proceeds through a premise
    // that only (:n a) derives, through (:w a), and another that relies on a
    // premise that is based on the asserted (:c a). Since a rule exists which
    // could produce (:c a), it prints as "asserted; no derivation found" and
    // the search previously took this label for the outcome of a path cut.
    // Consequently, both instantiations were deemed not well-founded, the
    // first one was retained, and (:w a), severed by the cycle, was displayed
    // as asserted even though it was derived exclusively through rules.
    //
    // As shown in the test previously, two variants keep the competing rules,
    // (:d X) => (:n X) and (:m X) => (:n X), at identical ids and interchange
    // which of them is the cyclic one, ensuring that one variant meets the
    // cycle first regardless of the hash.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& grounded, const std::string& cyclic)
        {
            process_lines(interactive, "(:d X) => (:n X)\n(:m X) => (:n X)\n:c a\n(:k X) => (:c X)\n(:c X) => (:" + grounded + " X)\n(:n X) => (:w X)\n(:w X) => (:" + cyclic + " X)\n");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (:n a) 0");
            CHECK(explained(collector) == ":n a\n"
                                          "   └─ :" + grounded + " a\n"
                                          "      └─ :c a  [asserted; no derivation found]");
        };
        SUBCASE("d grounded") { check("d", "m"); }
        SUBCASE("m grounded") { check("m", "d"); } });
}

TEST_CASE("explain: a derived premise the search cannot rebuild does not displace an axiom")
{
    // One premise of (:t w) was obtained under the condition that ¬(:block w)
    // held true. As soon as (:block w) becomes true, no instantiation of that
    // rule remains valid, and the premise prints as
    // "asserted; no derivation found", mirroring the status of an asserted
    // fact. The file logic.md describes this state for rules whose negation is
    // subsequently reversed by a fact. (:t w) also stems from an axiom. When
    // every such leaf was deemed founded, allowing asserted facts to terminate
    // a proof, the search stopped at the derived premise when it met it first.
    // It then printed a derived fact as though it were asserted, replacing the
    // proof originating from the axiom.
    //
    // Two variants keep the competing rules, (:alt A) => (:t A) and
    // (:q A) => (:t A), at identical ids and swap which of their premises
    // the negation derives, ensuring that one variant meets the derived
    // premise first regardless of the hash (see "a premise first reached
    // through a cycle").
    //
    // This binary is required to request the REPL's default mode: check
    // mode turns a negation that has come to be true into an error, whereas
    // the REPL keeps the fact.
    const auto check = [](const std::string& derived, const std::string& axiom)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive on");

        process_lines(interactive, R"(
(:alt A) => (:t A)
(:q A) => (:t A)
(:t A) => (:u A)
)");
        interactive.process("((:start A), ¬(:block A)) => (:" + derived + " A)");
        interactive.process(":start w");
        interactive.process(":" + axiom + " w");
        interactive.run(true, false, false);
        interactive.process(":block w");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (:" + derived + " w) 0");
        REQUIRE(explained(collector) == ":" + derived + " w  [asserted; no derivation found]");

        collector.clear();
        interactive.process(".explain (:u w) 0");
        const std::string leaf = ":" + axiom + " w  [axiom]";
        CHECK(explained(collector) == ":u w\n   └─ :t w\n      └─ " + leaf);
    };
    SUBCASE("the negation derives q") { check("q", "alt"); }
    SUBCASE("the negation derives alt") { check("alt", "q"); }
}

// During inference, nothing is recorded, and a prune eliminates solely the
// fact it refers to: (a q c) remains even after (b p c) is gone, and it
// continues to respond to queries. The search finds no instantiation that
// currently holds, and since the rule's consequence matches the fact, the
// tree concludes with "[asserted; no derivation found]", a description that
// .help .explain describes for exactly this scenario. It must not say
// [axiom]: that label claims no rule could have derived the fact.
TEST_CASE("explain: a derived fact whose premise was pruned is not shown as an axiom")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y, Y p Z) => (X q Z)
a p b
b p c
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".prune-facts (b p c)");
        REQUIRE(any_output_contains(collector, "Pruned 1"));

        collector.clear();
        interactive.process("X q Y");
        REQUIRE(answers_contain(collector, "a q c"));

        collector.clear();
        interactive.process(".explain (a q c) 0");
        CHECK(explained(collector) == "a q c  [asserted; no derivation found]"); });
}

TEST_CASE("explain: a derivative is explained without trying every way to its numerals")
{
    // Because the simplifier tests its numeral leaves, a numeral within a
    // derivative undergoes testing from two sides: as a leaf in the
    // expression under differentiation and as a leaf in the raw derivative.
    // The second approach returns to the derivative itself, so the path cuts
    // it; the first leads to the asserted (T diffby x). With the asserted
    // premise counted as unfounded, neither way was accepted, no result was
    // kept for future use, and every path through the proof searched both
    // directions anew: this explanation required 164 s rather than one, and
    // printed facts that only rules can derive, such as (:needssimp ...), as
    // if asserted.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    process_lines(interactive, R"(
.import math
x ~ symvar
(((((&3 * (x ^ &4)) + (&2 * (x ^ &3))) + (&5 * (x ^ &2))) + (&7 * x)) * (ln of x)) diffby x
)");
    const std::string derivative = "(($( (&3 * x ^ &4 + &2 * x ^ &3 + &5 * x ^ &2 + &7 * x) * ln(x) ) diffby x) = $( (&12 * x ^ &3 + &6 * x ^ &2 + &10 * x + &7) * ln(x) + (&3 * x ^ &4 + &2 * x ^ &3 + &5 * x ^ &2 + &7 * x) * (&1 / x) ))";
    collector.clear();
    interactive.process("((((((&3 * (x ^ &4)) + (&2 * (x ^ &3))) + (&5 * (x ^ &2))) + (&7 * x)) * (ln of x)) diffby x) = D");
    REQUIRE(answers_contain(collector, derivative.substr(1, derivative.size() - 2)));

    collector.clear();
    const auto start = std::chrono::steady_clock::now();
    interactive.process(".explain " + derivative + " 0");
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(elapsed.count() < 10.0);
    CHECK(any_output_contains(collector, "(&1 / x)"));
    std::size_t asserted = 0;
    for (const auto& e : collector.events())
    {
        std::istringstream text(e.text);
        for (std::string line; std::getline(text, line);)
        {
            if (line.find("no derivation found") == std::string::npos) continue;
            ++asserted;
            CAPTURE(line);
            CHECK(line.find(":needs") == std::string::npos);
            CHECK(line.find(" isnumeral ") == std::string::npos);
            CHECK(line.find(" canonnum ") == std::string::npos);
        }
    }
    // Not vacuous: the session asserts (T diffby x) and (x ~ symvar), and a
    // rule might generate either one.
    CHECK(asserted > 0);
}

// The expense of the per-path search remained unavoidable: a result that
// depended on the path was searched again on each path that reached it --
// 54 583 searches of 522 unique facts for this fifth-degree derivative,
// taking 60 s, and 18 s for the back-edge ladder beneath. The search now
// enters each fact exactly once per explanation and resolves a cycle for its
// whole component, thus, without a depth limit, the number of searches
// equals the number of facts. A time bound applies too, because a count
// cannot discern the cost incurred by each individual search.
TEST_CASE("explain: every intermediate result is searched once")
{
    SUBCASE("a fifth-degree polynomial times ln x")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        const std::string polynomial = "((((((&2 * (x ^ &6)) + (&3 * (x ^ &5))) + (&4 * (x ^ &4))) + (&5 * (x ^ &3))) + (&6 * (x ^ &2))) * (ln of x))";
        process_lines(interactive, ".import math\nx ~ symvar\n" + polynomial + " diffby x");
        collector.clear();
        interactive.process("(" + polynomial + " diffby x) = D");
        const std::vector<std::string> answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);

        collector.clear();
        const auto start = std::chrono::steady_clock::now();
        interactive.process(".explain (" + answers.front() + ") 0");
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(elapsed.count() < 10.0);
        CHECK(any_output_contains(collector, "(&1 / x)"));
        const auto counts = interactive.graph()->last_explain_counts();
        CHECK(counts.facts > 100);
        CHECK(counts.searches == counts.facts);
    }
    SUBCASE("a ladder whose rungs are joined by back edges")
    {
        // Two columns p and q, each rung derived from both cells of the rung
        // immediately above and looping back; solely the top rung is asserted.
        // Every fact resides within a cycle alongside every other, thus the
        // entire structure forms a single component, and it lacks any support
        // originating from outside: the SCC rule ends the proof one step
        // beneath the queried fact (see "a cycle without support from
        // outside").
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        std::string ladder = ":p16 a\n:q16 a\n(:k X) => (:p16 X)\n(:k X) => (:q16 X)\n";
        for (int i = 0; i < 16; ++i)
        {
            const std::string up = std::to_string(i + 1), here = std::to_string(i);
            ladder += "(:p" + up + " X) => (:p" + here + " X)\n(:q" + up + " X) => (:p" + here + " X)\n";
            ladder += "(:p" + up + " X) => (:q" + here + " X)\n(:q" + up + " X) => (:q" + here + " X)\n";
        }
        for (int i = 0; i < 16; ++i)
        {
            const std::string up = std::to_string(i + 1), here = std::to_string(i);
            ladder += "(:p" + here + " X) => (:p" + up + " X)\n(:q" + here + " X) => (:q" + up + " X)\n";
        }
        interactive.process(".deductions off");
        process_lines(interactive, ladder);
        interactive.run(false, false, false);

        collector.clear();
        const auto start = std::chrono::steady_clock::now();
        interactive.process(".explain (:p0 a) 0");
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(elapsed.count() < 10.0);
        CHECK(explained(collector) == ":p0 a\n   └─ :p1 a  [asserted; no derivation found]");
        const auto counts = interactive.graph()->last_explain_counts();
        CHECK(counts.facts == 34);
        CHECK(counts.searches == counts.facts);
    }
}

// The search went down one call frame per proof level, four C++ frames deep,
// and a proof spanning roughly 2 000 levels -- a chain of facts each derived
// from the prior one, or the jac-s2 session from tutorial-jacobian.md at depth
// 0 -- overflowed the stack and took the process down with it. The search now
// maintains its own stack. A tree extending a hundred thousand levels deep is
// of no use when printed, and .explain states this instead.
//
// Each join in this chain previously initiated with (X ok Z), where the sole
// bound end was the hub z, thereby examining every `ok` fact for each one it
// explained: a quadratic expense in addition to the depth. It now begins
// where the bindings restrict it, at (X next n_i).
TEST_CASE("explain: a proof deeper than the call stack is searched without a crash")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run");
    interactive.process("(X next Y, X ok Z) => (Y ok Z)");

    // Constructed via the API: 200 000 facts as script lines take
    // minutes.
    auto* const                graph = interactive.graph();
    const std::string          lang  = graph->lang();
    const zelph::network::Node next  = graph->node("next", lang);
    const zelph::network::Node ok    = graph->node("ok", lang);
    const zelph::network::Node z     = graph->node("z", lang);
    zelph::network::Node       last  = graph->node("n0", lang);
    graph->fact(last, ok, {z});
    constexpr std::size_t steps = 100000;
    for (std::size_t i = 1; i <= steps; ++i)
    {
        const zelph::network::Node node = graph->node("n" + std::to_string(i), lang);
        graph->fact(last, next, {node});
        graph->fact(node, ok, {z});
        last = node;
    }
    const zelph::network::Node target = graph->fact(last, ok, {z});

    const auto start = std::chrono::steady_clock::now();
    {
        const auto proof = graph->explain(target, 0);
        REQUIRE(proof != nullptr);
        std::size_t                      levels = 0;
        const zelph::network::ProofNode* p      = proof.get();
        while (p != nullptr && p->status == zelph::network::ProofNode::Status::Derived)
        {
            const zelph::network::ProofNode* below = nullptr;
            for (const auto& premise : p->premises)
                if (premise->fact != 0 && graph->predicate_of(premise->fact) == ok) below = premise.get();
            p = below;
            ++levels;
        }
        CHECK(levels == steps);
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed.count() < 60.0);

    collector.clear();
    interactive.process(".explain (n" + std::to_string(steps) + " ok z) 0");
    CHECK(any_output_contains(collector, "too deep"));

    collector.clear();
    interactive.process(".explain (n" + std::to_string(steps) + " ok z) 3");
    CHECK(explained(collector) == "n100000 ok z\n"
                                  "   ├─ n99999 ok z\n"
                                  "   │  ├─ n99998 ok z\n"
                                  "   │  │  ├─ n99997 ok z  … [depth limit -- use '.explain <pattern> 0' for the full proof]\n"
                                  "   │  │  └─ n99997 next n99998  [axiom]\n"
                                  "   │  └─ n99998 next n99999  [axiom]\n"
                                  "   └─ n99999 next n100000  [axiom]");
}

// The safeguard preventing the output of a tree with thousands of levels
// names the proof's height and the bound a max-depth must keep. When the
// max-depth exceeded the bound, it named the max-depth as the proof's
// height and recommended a max-depth, even though one was already provided.
TEST_CASE("explain: a tree too deep to print names the proof's height and the bound")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run");
    interactive.process("(X next Y, X ok Z) => (Y ok Z)");
    auto* const graph = interactive.graph();
    assert_fact(graph, "n0", "ok", "z");
    for (std::size_t i = 1; i <= 6000; ++i)
    {
        assert_fact(graph, "n" + std::to_string(i - 1), "next", "n" + std::to_string(i));
        assert_fact(graph, "n" + std::to_string(i), "ok", "z");
    }

    // The height of the proof is established when the complete proof is
    // being searched: a budget that holds the chain of 6000.
    graph->set_explain_budget(std::size_t{1} << 30);
    for (const char* depth : {"0", "5001", "6000"})
    {
        CAPTURE(depth);
        collector.clear();
        interactive.process(std::string(".explain (n6000 ok z) ") + depth);
        CHECK(any_output_contains(collector, "6000 levels deep"));
        CHECK(any_output_contains(collector, "5000 or less"));
    }

    collector.clear();
    interactive.process(".explain (n6000 ok z) 5000");
    CHECK(deepest_level(explained(collector)) == 5000);
}

// A path condition on a predicate the rules derive, along a chain: the
// walk from X to `good` traversed the whole adjacency of the hub `good`
// for each fact in the chain, and the facts being searched were gathered
// once more for each walk -- the square of the chain's length, 8 s at
// 10 000.
TEST_CASE("explain: a walk to a hub costs no more than the steps it takes")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run");
    process_lines(interactive, "(X st src) => (X st good)\n(X nx Y, X st good, X st⁺ good) => (Y st good)\n");
    auto* const graph = interactive.graph();
    assert_fact(graph, "a0", "st", "src");
    constexpr std::size_t length = 20000;
    for (std::size_t i = 0; i <= length; ++i)
    {
        assert_fact(graph, "a" + std::to_string(i), "st", "good");
        if (i < length) assert_fact(graph, "a" + std::to_string(i), "nx", "a" + std::to_string(i + 1));
    }

    collector.clear();
    const auto start = std::chrono::steady_clock::now();
    interactive.process(".explain (a" + std::to_string(length) + " st good) 0");
    CHECK(seconds_since(start) < 10.0);
    CHECK(any_output_contains(collector, "levels deep"));
}

#ifdef __linux__
// The identical chain, for what the search holds. Every walk kept a copy of
// the facts it had walked around -- each fact of its predicate being
// searched, for each step within this chain all facts located above it --
// for the duration of its instantiation, and an instantiation lives as long
// as the frame of its fact: the square of the chain's length, held
// simultaneously. Ten thousand steps took 1.2 GB at depth 0, and 0.8 GB at
// the default depth, where the budget stops the search after 80 000 units
// of work; twenty thousand took 4.7 GB. A further walk takes the facts
// being searched exactly as they stand when it is taken.
TEST_CASE("explain: a walk to a hub holds no more than the steps it takes")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run");
    process_lines(interactive, "(X st src) => (X st good)\n(X nx Y, X st good, X st⁺ good) => (Y st good)\n");
    auto* const graph = interactive.graph();
    assert_fact(graph, "a0", "st", "src");
    constexpr std::size_t length = 10000;
    for (std::size_t i = 0; i <= length; ++i)
    {
        assert_fact(graph, "a" + std::to_string(i), "st", "good");
        if (i < length) assert_fact(graph, "a" + std::to_string(i), "nx", "a" + std::to_string(i + 1));
    }

    for (const std::string depth : {" 0", ""})
    {
        CAPTURE(depth);
        const long grown = peak_growth_kb([&]
                                          { interactive.process(".explain (a" + std::to_string(length) + " st good)" + depth); });
        CAPTURE(grown);
        CHECK(grown < 256 * 1024);
    }
}
#endif

// The facts traversed during a walk that a rule concludes are children of
// the step's proof, matching the walk's length. Before the tree is printed,
// the proofs terminating at a leaf within a cycle are gathered across the
// whole proof, and that pass collected a proof's children again each time
// the proof came back to the top of its stack: the square of their number.
// Here, the walk traverses 40 000 derived edges, none of which are
// displayed, and printing the tree of four lines required double the time of
// searching its proof. The renderer maintains no internal counter, so its
// contribution is assessed relative to the search within the same process:
// .explain performs the same search as explain() does, and outputs the
// result.
TEST_CASE("explain: printing a walk over many derived edges costs little beside the search" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, "((X start C), (C sub⁺ D), (D marked yes)) => (X at D)\n(X base Y) => (X sub Y)\n");
    auto* const           graph  = interactive.graph();
    const std::string     lang   = graph->lang();
    constexpr std::size_t length = 40000;
    const std::string     last   = "c" + std::to_string(length);
    for (std::size_t i = 0; i < length; ++i)
    {
        assert_fact(graph, "c" + std::to_string(i), "base", "c" + std::to_string(i + 1));
        assert_fact(graph, "c" + std::to_string(i), "sub", "c" + std::to_string(i + 1));
    }
    assert_fact(graph, "x", "start", "c0");
    assert_fact(graph, last, "marked", "yes");
    const zelph::network::Node fact = graph->fact(graph->node("x", lang), graph->node("at", lang), {graph->node(last, lang)});

    double search = 1e9;
    double whole  = 1e9;
    for (int round = 0; round < 3; ++round)
    {
        auto start = std::chrono::steady_clock::now();
        graph->explain(fact, 1);
        search = std::min(search, seconds_since(start));

        collector.clear();
        start = std::chrono::steady_clock::now();
        interactive.process(".explain (x at " + last + ") 1");
        whole = std::min(whole, seconds_since(start));
        CHECK(explained(collector) == "x at c40000\n"
                                      "   ├─ c40000 marked yes  [axiom]\n"
                                      "   ├─ x start c0  [axiom]\n"
                                      "   └─ (c0 sub c40000) closure one-or-more  [closure]");
    }
    CAPTURE(search);
    CAPTURE(whole);
    CHECK(whole < 1.5 * search + 0.1);
}

// The identical walk, for the duration the search dedicates to it. The edges
// of a walk that a rule concludes are placed among the premises of the step,
// each occurring once, and whether an edge had been placed already was checked
// for every edge placed before it: the square of the walk's length, not
// counted by the budget. Eight times the length took thirty-nine times the
// duration, three seconds and a half at 160 000 edges for a tree consisting of
// four lines. The budget of a single unit sends the explanation straight to
// the search within the limit, which takes the root's walk, places its edges,
// and cuts them at the limit; the work it counts increases solely with the
// length, so the time is compared between two lengths within the same process.
TEST_CASE("explain: the edges of a long walk are placed in time linear in its length" * doctest::test_suite("slow"))
{
    const auto searched = [](const std::size_t length)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((X start C), (C sub⁺ D), (D marked yes)) => (X at D)\n(X base Y) => (X sub Y)\n");
        auto* const       graph = interactive.graph();
        const std::string lang  = graph->lang();
        const std::string last  = "c" + std::to_string(length);
        for (std::size_t i = 0; i < length; ++i)
        {
            assert_fact(graph, "c" + std::to_string(i), "base", "c" + std::to_string(i + 1));
            assert_fact(graph, "c" + std::to_string(i), "sub", "c" + std::to_string(i + 1));
        }
        assert_fact(graph, "x", "start", "c0");
        assert_fact(graph, last, "marked", "yes");
        const zelph::network::Node fact = graph->fact(graph->node("x", lang), graph->node("at", lang), {graph->node(last, lang)});

        graph->set_explain_budget(1);
        double best = 1e9;
        for (int round = 0; round < 3; ++round)
        {
            const auto start = std::chrono::steady_clock::now();
            graph->explain(fact, 1);
            best = std::min(best, seconds_since(start));
        }
        CHECK(graph->last_explain_counts().limited);
        return best;
    };
    const double shorter = searched(20000);
    const double longer  = searched(160000);
    CAPTURE(shorter);
    CAPTURE(longer);
    CHECK(longer < 16 * shorter);
}

TEST_CASE("explain: a deep proof on the math stack is searched without a crash" * doctest::test_suite("slow"))
{
    // During the jac-s2 session, the docs pass examined
    // tutorial-jacobian.md; 8 out of 12 sampled facts crashed the process at
    // depth 0.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".semi-naive on"); // In check mode, each run during the session would be verified through classic passes
    process_lines(interactive, R"(
.deductions off
.import math
? :topoly $( (2+0*0)^3*(-1) + 2*0^2*(2+0*0)*(8+3*0*0) )
? :topoly $( 8*0 + 3*0*(2+0*0)^2*(-1) + 6*0*0^2*(8+3*0*0) )
? :topoly $( 8*0 - (6*0^2*0 + 0^3*(-1)) )
? :topoly $( (2+1*(-3))^3*26 + 2*(-3)^2*(2+1*(-3))*(8+3*1*(-3)) )
? :topoly $( 8*(-3) + 3*1*(2+1*(-3))^2*26 + 6*1*(-3)^2*(8+3*1*(-3)) )
? :topoly $( 8*1 - (6*1^2*(-3) + 1^3*26) )
? :topoly $( (2+(-1)*3)^3*26 + 2*3^2*(2+(-1)*3)*(8+3*(-1)*3) )
? :topoly $( 8*3 + 3*(-1)*(2+(-1)*3)^2*26 + 6*(-1)*3^2*(8+3*(-1)*3) )
? :topoly $( 8*(-1) - (6*(-1)^2*3 + (-1)^3*26) )
<a b c> ~ polyring
? $( (2+a*b)^3*c + 2*b^2*(2+a*b)*(8+3*a*b) ) diffby a
? $( 8*a - (6*a^2*b + a^3*c) ) diffby c
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (F diffby X)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (F diffby Y)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (F diffby Z)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (G diffby X)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (G diffby Y)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (G diffby Z)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (H diffby X)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (H diffby Y)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (H diffby Z)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil))), (F diffby X) = _P, (F diffby Y) = _Q, (F diffby Z) = _R, (G diffby X) = _S, (G diffby Y) = _T, (G diffby Z) = _U, (H diffby X) = _V, (H diffby Y) = _W, (H diffby Z) = _N) => (((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) detis ((( _P * ((_T * _N) - (_U * _W))) - (_Q * ((_S * _N) - (_U * _V)))) + (_R * ((_S * _W) - (_T * _V)))))
(M detis D) => (D topoly D)
(M detis D, (D topoly D) = P) => (M jdet P)
< $( (2+a*b)^3*c + 2*b^2*(2+a*b)*(8+3*a*b) ) $( 8*b + 3*a*(2+a*b)^2*c + 6*a*b^2*(8+3*a*b) ) $( 8*a - (6*a^2*b + a^3*c) ) > jac3 <a b c>
.run
)");
    collector.clear();
    interactive.process(".explain (&40 lcmp &10) 0");
    // The tree contains roughly 25 000 lines, exceeding the capacity of the
    // sugar-aware helpers.
    const std::string tree = explained(collector);
    CHECK(tree.rfind("&40 lcmp &10\n", 0) == 0);
    CHECK(tree.find("depth limit") == std::string::npos);
}

// Within a depth limit, a fact that is encountered again at a higher level
// than any earlier search of it undergoes re-searching only when reached
// from a strictly shallower depth, so at most max-depth times. Previously,
// the search would restart at the depth where the fact was met, even if
// that depth was deeper, and 528 facts took 15 293 searches at limit 40 --
// three times slower than the search per path it replaced. On this input,
// the re-searches stay well below twice the number of facts at limits 40
// and 60, slightly exceeding that threshold at limit 20.
TEST_CASE("explain: under a depth limit a fact is searched again only from higher up" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const std::string polynomial = "((((((&2 * (x ^ &6)) + (&3 * (x ^ &5))) + (&4 * (x ^ &4))) + (&5 * (x ^ &3))) + (&6 * (x ^ &2))) * (ln of x))";
    process_lines(interactive, ".import math\nx ~ symvar\n" + polynomial + " diffby x");
    collector.clear();
    interactive.process("(" + polynomial + " diffby x) = D");
    const std::vector<std::string> answers = collect_answers(collector);
    REQUIRE(answers.size() == 1);

    for (const std::size_t depth : {20, 40, 60})
    {
        CAPTURE(depth);
        collector.clear();
        interactive.process(".explain (" + answers.front() + ") " + std::to_string(depth));
        const auto counts = interactive.graph()->last_explain_counts();
        CAPTURE(counts.searches);
        CAPTURE(counts.facts);
        CHECK(counts.most <= depth);
        CHECK(counts.searches <= 3 * counts.facts);
        CHECK(fact_under_itself(explained(collector)).empty());
    }
}

// A ring composed of facts, each derived from its pair of adjacent facts.
// Each fact lies on the loop, causing the search to traverse the entire
// circumference: one iteration per fact, but each step joined (X st good)
// -- every fact of the ring -- before the edge that binds X, thus the ring
// incurred a cost equal to the square of its length, and a ring of 1 800
// overflowed the stack capacity. The join commences at the point where the
// bindings constrain it, while the search keeps its independent stack.
// Nothing beyond the ring supports it: the queried fact shows the
// instantiation it was discovered through, based on its two adjacent facts.
TEST_CASE("explain: a ring of facts derived from their neighbours is resolved in linear time")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run");
    interactive.process("(X nx Y, Y nx Z, X st good, Z st good) => (Y st good)");

    auto* const           graph = interactive.graph();
    constexpr std::size_t size  = 4000;
    for (std::size_t i = 0; i < size; ++i)
    {
        assert_fact(graph, "c" + std::to_string(i), "nx", "c" + std::to_string((i + 1) % size));
        assert_fact(graph, "c" + std::to_string(i), "st", "good");
    }

    collector.clear();
    const auto start = std::chrono::steady_clock::now();
    interactive.process(".explain (c0 st good) 0");
    CHECK(seconds_since(start) < 10.0);

    const std::string tree = explained(collector);
    CHECK(tree.rfind("c0 st good\n", 0) == 0);
    CHECK(tree.find("c3999 st good  [asserted; no derivation found]") != std::string::npos);
    CHECK(tree.find("c1 st good  [asserted; no derivation found]") != std::string::npos);
    const auto counts = graph->last_explain_counts();
    CHECK(counts.searches == counts.facts);
}

// The root annotation decides whether a further instantiation holds without
// the root, and constructs that instantiation to find out. In this setup,
// each one traverses a chain looping back to the root, which the root's own
// proof, (r st src), never needed: a hundred chains each comprising a
// hundred facts took 80 s (each step joined every `st good` fact as well),
// while a single chain of 3 000 overflowed the stack. Seeking a further
// justification now incurs a cost no greater than the proof's, with a lower
// bound, and the join starts at the edge. No annotation either way: no
// further justification exists.
TEST_CASE("explain: looking for a second justification costs no more than the proof")
{
    const auto check = [](const std::size_t chains, const std::size_t length)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".auto-run");
        process_lines(interactive, "(X nx Y, X st good) => (Y st good)\n(X st src) => (X st good)\n");

        auto* const graph = interactive.graph();
        assert_fact(graph, "r", "st", "src");
        assert_fact(graph, "r", "st", "good");
        for (std::size_t k = 0; k < chains; ++k)
        {
            const std::string chain = "c" + std::to_string(k) + "_";
            assert_fact(graph, "r", "nx", chain + "0");
            for (std::size_t i = 0; i < length; ++i)
            {
                assert_fact(graph, chain + std::to_string(i), "st", "good");
                assert_fact(graph, chain + std::to_string(i), "nx", i + 1 < length ? chain + std::to_string(i + 1) : "r");
            }
        }

        collector.clear();
        const auto start = std::chrono::steady_clock::now();
        interactive.process(".explain (r st good) 0");
        CHECK(seconds_since(start) < 10.0);
        CHECK(explained(collector) == "r st good\n   └─ r st src  [axiom]");

        // The bound holds within the search of a single premise too: a
        // single alternative whose premise heads a chain of thousands of
        // facts was searched to its end after its search had begun --
        // 20 002 searches conducted on a chain comprising ten thousand.
        CHECK(interactive.graph()->last_explain_counts().searches < 5000);
    };
    SUBCASE("a hundred chains of a hundred") { check(100, 100); }
    SUBCASE("one chain of three thousand") { check(1, 3000); }
    SUBCASE("one chain of ten thousand") { check(1, 10000); }
}

// The list of rules in force remains unchanged across successive .explain
// operations unless a rule appears, disappears, or comes to be mentioned, and
// building it cost a scan per rule: with thousands of rules a generator made,
// each explanation of an axiom used to take seconds. It is now preserved from
// one explanation to the next. The identifier of a fact is the hash of its
// content, meaning two runs over the same program hold the same graph -- yet
// the order in which the search meets a node's facts reflected the sequence
// in which the forward pass constructed the structures, and a parallel run
// constructs them in a different sequence each time. The step shown by the
// SCC rule within the transitive closure of a ring, the first instantiation
// recorded, varied across runs: four different trees emerged in five runs of
// the same program. The search now lists the matches of a condition in the
// order of the facts' identifiers, and a walk takes its edges in that order,
// so the tree depends solely on the graph.
TEST_CASE("explain: the tree does not depend on the order in which a parallel run built the graph" * doctest::test_suite("slow"))
{
    const auto session = []
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        std::string                 program = ".deductions off\nsub ~ trans\n(R ~ trans, X R Y, Y R Z) => (X R Z)\n";
        for (int i = 0; i < 100; ++i)
            program += "c" + std::to_string(i) + " sub c" + std::to_string((i + 1) % 100) + "\n";
        process_lines(interactive, program);
        interactive.run(true, false, false);
        std::string trees;
        for (const char* query : {".explain (c0 sub c1) 2", ".explain (c7 sub c3) 1"})
        {
            collector.clear();
            interactive.process(query);
            trees += explained(collector) + "\n\n";
        }
        return trees;
    };
    const std::string first = session();
    CHECK(session() == first);
    CHECK(session() == first);
}

// The sequence of matches for a condition follows the order of the facts'
// ids. This sequence requires all candidate facts to be considered, yet does
// not demand every match to be processed: each match was enumerated, and its
// premise resolved, before the initial attempt, meaning a join whose first
// solution already gives a clean proof incurred a cost equivalent to the
// number of matches in the condition: x belongs to two thousand groups, each
// flagged, and the proof for (x ok yes) took 2 015 units of work; a hundred
// thousand groups required 100 015, exceeding the budget, thus a
// default-depth explanation fell back to the search within the limit.
// Candidate facts are now arranged in order before the first match is
// selected, and matches are taken one at a time: the proof shows the
// identical group, the one whose fact carries the lowest id, and the root
// stops at its second justification.
TEST_CASE("explain: a join tries its first match without enumerating the others")
{
    const auto check = [](const bool marked, const std::string& tree)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n((X in G), (G flag yes)) => (X ok yes)\n" + std::string(marked ? "(G mark yes) => (G flag yes)\n" : ""));
        const auto graph = interactive.graph();
        for (int i = 0; i < 2000; ++i)
        {
            assert_fact(graph, "x", "in", "g" + std::to_string(i));
            assert_fact(graph, "g" + std::to_string(i), "flag", "yes");
            if (marked) assert_fact(graph, "g" + std::to_string(i), "mark", "yes");
        }
        assert_fact(graph, "x", "ok", "yes");

        collector.clear();
        interactive.process(".explain (x ok yes) 0");
        CHECK(explained(collector) == tree);
        CAPTURE(graph->last_explain_counts().work);
        CHECK(graph->last_explain_counts().work < 200);

        // Within a depth limit, a budget that the proof needs a fraction of
        // keeps the search for the complete proof.
        graph->set_explain_budget(1000);
        collector.clear();
        interactive.process(".explain (x ok yes)");
        CHECK(explained(collector) == tree);
        CHECK_FALSE(graph->last_explain_counts().limited);
        CHECK_FALSE(any_output_contains(collector, "ran out of its budget"));
    };
    SUBCASE("each premise an axiom")
    {
        check(false, "x ok yes  [one of several justifications]\n"
                     "   ├─ x in g129  [axiom]\n"
                     "   └─ g129 flag yes  [axiom]");
    }
    // Each flag is derived by a rule as well, so the search continues to
    // search it while the join remains idle, and a join that sets its
    // enumeration aside meanwhile still takes no match it is not asked
    // for.
    SUBCASE("a premise searched first")
    {
        check(true, "x ok yes  [one of several justifications]\n"
                    "   ├─ x in g1318  [axiom]\n"
                    "   └─ g1318 flag yes\n"
                    "      └─ g1318 mark yes  [axiom]");
    }
}

// At a join level featuring numerous candidates, the enumeration is set aside
// while the search works on the premise of the match it gave, resuming after
// the fact that match came from when the search requests the subsequent one;
// resumed twice, it proceeds to consume the rest of its matches before the
// search continues (see Join::suspend). Each group's flag is asserted and
// derived via a rule no instantiation of which holds, except within a single
// group where one instantiation does: the search examines groups in sequence
// according to their facts' ids, searching each flag until it arrives at that
// specific one. The position it occupies determines how far the enumeration
// has progressed: still active, resumed once, resumed twice, among the
// matches taken ahead, and the final one. When more than 256 candidates
// exist, they are ordered byte by byte (sort_ids in unification.cpp),
// necessitating verification of the order across as many groups as well.
TEST_CASE("explain: a join set aside while a premise is searched goes on with the next match")
{
    const auto tree = [](const int count, const std::size_t position)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n((X in G), (G flag yes)) => (X ok yes)\n(G mark yes) => (G flag yes)\nx ok yes\n");
        auto* const              graph = interactive.graph();
        std::vector<std::string> groups;
        for (int i = 0; i < count; ++i)
        {
            groups.push_back("g" + std::to_string(i));
            assert_fact(graph, "x", "in", groups.back());
            assert_fact(graph, groups.back(), "flag", "yes");
        }
        std::sort(groups.begin(), groups.end(), [&](const std::string& a, const std::string& b)
                  { return fact_node(graph, "x", "in", a) < fact_node(graph, "x", "in", b); });
        const std::string& group = groups[position];
        assert_fact(graph, group, "mark", "yes");
        collector.clear();
        interactive.process(".explain (x ok yes) 0");
        CHECK(explained(collector) == "x ok yes  [one of several justifications]\n"
                                      "   ├─ x in "
                                          + group + "  [axiom]\n"
                                                    "   └─ "
                                          + group + " flag yes\n"
                                                    "      └─ "
                                          + group + " mark yes  [axiom]");
    };
    for (const int count : {200, 600})
        for (const std::size_t position : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t(count - 1)})
        {
            CAPTURE(count);
            CAPTURE(position);
            tree(count, position);
        }
}

// The search returns to a join for each match here: since no group's flag has
// a derivation whose leaves are all axioms, the root attempts each one in
// turn, and each flag is searched first. A level revisited reads its
// candidates once more; if revisited after every search, it would read them
// once per match, resulting in the square of their count. Revisited twice, it
// takes the remaining matches ahead at the next search, and the work
// increases with the number of groups.
TEST_CASE("explain: a join the search comes back to after every premise reads its candidates a bounded number of times")
{
    const auto work = [](const int groups)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n((X in G), (G flag yes)) => (X ok yes)\n(G mark yes) => (G flag yes)\nx ok yes\n");
        auto* const graph = interactive.graph();
        for (int i = 0; i < groups; ++i)
        {
            assert_fact(graph, "x", "in", "g" + std::to_string(i));
            assert_fact(graph, "g" + std::to_string(i), "flag", "yes");
        }
        interactive.process(".explain (x ok yes) 0");
        CHECK(graph->last_explain_counts().searches > static_cast<std::size_t>(groups));
        return graph->last_explain_counts().work;
    };
    const std::size_t few  = work(200);
    const std::size_t many = work(2000);
    CAPTURE(few);
    CAPTURE(many);
    CHECK(many < 15 * few);
}

#ifdef __linux__
// At each level of a join, the condition's matches are processed one by one,
// and the level remains open while the search works on the premise of the
// match it gave last, meaning a proof maintains a level open for each step. A
// level that exhausted all its matches still preserved its enumeration: in a
// chain spanning seventy thousand steps with two such levels per step, the
// depth-0 explanation held 455 MB, whereas the search that enumerated every
// match before testing any, and retained nothing, used only 289 MB. Among the
// facts read at h is the rule's own condition, (h tag T), which yields no
// match: following (h tag t0), the level of (h tag T) retains that one
// candidate and no match. A level with so few candidates remaining processes
// their matches in advance and lets go, so this test fails to indicate
// whether a level recognizes that its candidates give no match; the next one
// does. A ground condition is confirmed via lookup and enumerates nothing,
// thus serving as the measure: replacing (h tag T) with (h tag t0), whose
// single match is (h tag t0), yields an identical proof.
TEST_CASE("explain: a level of a join with no match left holds nothing while its premise is searched" * doctest::test_suite("slow"))
{
    const auto grown = [](const std::string& condition)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n((X next Y), (Y ok yes), " + condition + ") => (X ok yes)\nh tag t0\n");
        auto* const           graph  = interactive.graph();
        constexpr std::size_t length = 70000;
        for (std::size_t i = 0; i < length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "next", "n" + std::to_string(i + 1));
        for (std::size_t i = 0; i <= length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "ok", "yes");
        const long kb = peak_growth_kb([&]
                                       { interactive.process(".explain (n0 ok yes) 0"); });
        CHECK(any_output_contains(collector, "levels deep"));
        CHECK(graph->last_explain_counts().searches == 2 * length + 2);
        return kb;
    };
    const long enumerated = grown("(h tag T)");
    const long ground     = grown("(h tag t0)");
    CAPTURE(enumerated);
    CAPTURE(ground);
    CHECK(enumerated < ground + 16 * 1024);
    CHECK(enumerated < 370 * 1024);
}
#endif

// A level whose outstanding candidates cannot yield a match releases its
// enumeration before the search goes on to a premise (Join::suspend,
// Unification::exhausted), regardless of their quantity. Here, these serve
// as conditions for other rules: (h tag T) is read at h, and likewise (h tag
// T0) through (h tag T999), which Unification skips without testing. Set
// aside, instead, the level was revisited when the search returned to it,
// and the adjacency of h was read a second time in vain. The search returns
// to each step: (n200 ok yes) is asserted, and the rule's consequence
// matches it with no instantiation that holds, meaning no proof of the chain
// has leaves that are all axioms, and each step seeks a further
// instantiation. The thousand rules incur a work cost of 2 998 units beyond
// the chain without them, h being read once per step, and 6 198 read twice.
//
// The sequence in which candidates remain to be processed is determined by
// the order of their ids. The fact (h tag tK) takes, selecting from a set of
// sixteen names, the one that gives it the lowest id, thereby ensuring that
// the majority of the rules' conditions are positioned afterwards -- more
// than a level takes ahead (Join::suspend). The names are established before
// the rules, guaranteeing that both sessions choose the identical one.
TEST_CASE("explain: a level whose candidates left are conditions of rules is not read again")
{
    constexpr std::size_t length = 200;
    constexpr std::size_t rules  = 1000;
    struct Explained
    {
        std::string tree;
        std::size_t work;
    };
    const auto explain = [&](const bool conditions)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n.auto-run\n((X next Y), (Y ok yes), (h tag T)) => (X ok yes)\n");
        auto* const graph = interactive.graph();
        std::string tag   = "t0";
        for (int i = 1; i < 16; ++i)
            if (const std::string name = "t" + std::to_string(i); fact_node(graph, "h", "tag", name) < fact_node(graph, "h", "tag", tag)) tag = name;
        if (conditions)
        {
            std::string program;
            for (std::size_t k = 0; k < rules; ++k)
                program += "((X q" + std::to_string(k) + " Y), (h tag T" + std::to_string(k) + ")) => (X r" + std::to_string(k) + " Y)\n";
            process_lines(interactive, program);

            std::size_t following = 0;
            for (const zelph::network::Node n : graph->get_right(graph->node("h", graph->lang())))
                if (graph->is_rule_pattern(n) && n > fact_node(graph, "h", "tag", tag)) ++following;
            REQUIRE(following > 64);
        }
        assert_fact(graph, "h", "tag", tag);
        for (std::size_t i = 0; i < length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "next", "n" + std::to_string(i + 1));
        for (std::size_t i = 0; i <= length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "ok", "yes");
        collector.clear();
        interactive.process(".explain (n0 ok yes) 0");
        return Explained{explained(collector), graph->last_explain_counts().work};
    };
    const Explained with    = explain(true);
    const Explained without = explain(false);
    CHECK(with.tree == without.tree);
    CAPTURE(with.work);
    CAPTURE(without.work);
    // Scanning h's adjacency yields one unit for every 64 entries (refer to
    // scan_per_unit in reasoning_explain.cpp); performed once each step, and
    // never twice.
    constexpr std::size_t read = rules / 64;
    CHECK(with.work > without.work + length * read / 2);
    CHECK(with.work < without.work + 3 * length * read / 2);
}

// The same applies to candidates located in the `=>` position that are
// rules with all their variables contained within their conditions:
// Unification passes over them (is_rule_text in unification.cpp), and so
// must exhausted(), or the level holding them is set aside and again and
// again reads the adjacency of (h tag k). The condition (C => (h tag k))
// matches one ground rule, whose condition selects, from a set of sixteen
// names, the one that gives the rule the lowest id, thereby ensuring that
// the majority of the thousand rules follow it; those preceding it are
// tested at each step along the way.
TEST_CASE("explain: a level whose candidates left are rules with variables in their conditions is not read again")
{
    constexpr std::size_t length = 200;
    constexpr std::size_t rules  = 1000;
    struct Explained
    {
        std::string tree;
        std::size_t work;
        std::size_t preceding; // rules the level attempts before the ground one
    };
    const auto explain = [&](const bool texts)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".deductions off\n.auto-run\n((X next Y), (Y ok yes), (C => (h tag k))) => (X ok yes)\n");
        auto* const graph       = interactive.graph();
        const auto  consequence = fact_node(graph, "h", "tag", "k");
        const auto  ground_rule = [&](const std::string& name)
        { return zelph::network::Zelph::create_hash(graph->core.Causes, fact_node(graph, name, "w", "v"), zelph::network::adjacency_set{consequence}); };
        std::string subject = "z0";
        for (int i = 1; i < 16; ++i)
            if (const std::string name = "z" + std::to_string(i); ground_rule(name) < ground_rule(subject)) subject = name;
        std::size_t preceding = 0;
        if (texts)
        {
            std::string program;
            for (std::size_t k = 0; k < rules; ++k)
                program += "((X q" + std::to_string(k) + " Y), (X s" + std::to_string(k) + " Y)) => (h tag k)\n";
            process_lines(interactive, program);

            std::size_t following = 0;
            for (const zelph::network::Node n : graph->get_right(consequence))
            {
                if (!graph->var_in_closure(n)) continue;
                if (n > ground_rule(subject))
                    ++following;
                else
                    ++preceding;
            }
            REQUIRE(following > 64);
        }
        interactive.process("(" + subject + " w v) => (h tag k)");
        for (std::size_t i = 0; i < length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "next", "n" + std::to_string(i + 1));
        for (std::size_t i = 0; i <= length; ++i)
            assert_fact(graph, "n" + std::to_string(i), "ok", "yes");
        collector.clear();
        interactive.process(".explain (n0 ok yes) 0");
        return Explained{explained(collector), graph->last_explain_counts().work, preceding};
    };
    const Explained with    = explain(true);
    const Explained without = explain(false);
    CHECK(with.tree == without.tree);
    CAPTURE(with.work);
    CAPTURE(without.work);
    CAPTURE(with.preceding);
    // A unit for every 64 entries in the adjacency read, as previously
    // described, and one for every four candidates that were attempted
    // without success (refer to tries_per_unit in
    // reasoning_explain.cpp); once per step.
    const std::size_t read = rules / 64 + with.preceding / 4;
    CHECK(with.work > without.work + length * read / 2);
    CHECK(with.work < without.work + 3 * length * read / 2);
}

// After the root has expended the work its bound allows on an additional
// justification, it ceases further exploration. It continued enumerating
// its remaining instantiations regardless and concluded those whose
// premises needed no search, thus the bound did not bound the enumeration,
// and whether the annotation surfaced depended on the order of the rules.
TEST_CASE("explain: the root looks no further once its bound for a second justification is spent")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    std::string                 program = ".deductions off\n";
    for (int i = 0; i < 7; ++i)
        program += "d" + std::to_string(i) + " dd d" + std::to_string(i) + "\n";
    program += "(X qnx g) => (X qreach g)\n((X qnx Y), (Y qreach g)) => (X qreach g)\n(X qreach g) => (X qgood g)\n"
               "((X qa Y), (Y qisg Y)) => (X qgood Y)\n(X qa Y) => (X qgood Y)\nr qa g\ng qisg g\n";
    process_lines(interactive, program);
    const auto  graph    = interactive.graph();
    std::string previous = "r";
    for (int i = 1; i <= 3000; ++i)
    {
        assert_fact(graph, previous, "qnx", "n" + std::to_string(i));
        previous = "n" + std::to_string(i);
    }
    assert_fact(graph, previous, "qnx", "g");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process(".explain (r qgood g) 0");
    const std::string tree = explained(collector);
    CHECK(tree.rfind("r qgood g\n", 0) == 0);
    CHECK(tree.find("one of several") == std::string::npos);
    CHECK(graph->last_explain_counts().searches < 5000);
}

TEST_CASE("explain: the rules in force are read again only when they change")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".deductions off");
    interactive.process(".auto-run");
    std::string script = "(A knows B) => ((X p B) => (X q B))\n";
    for (int i = 0; i < 3000; ++i)
        script += "tom knows b" + std::to_string(i) + "\nx" + std::to_string(i) + " p b" + std::to_string(i) + "\n";
    process_lines(interactive, script);
    interactive.run(false, false, false);

    collector.clear();
    auto start = std::chrono::steady_clock::now();
    interactive.process(".explain (x10 p b10) 0");
    const double first = seconds_since(start);
    REQUIRE(explained(collector) == "x10 p b10  [axiom]");

    double again = first;
    for (int i = 11; i < 14; ++i)
    {
        start = std::chrono::steady_clock::now();
        interactive.process(".explain (x" + std::to_string(i) + " p b" + std::to_string(i) + ") 0");
        again = std::min(again, seconds_since(start));
    }
    CAPTURE(first);
    CAPTURE(again);
    CHECK(again * 5 < first);

    // A rule entered since: the list is read once
    // more.
    process_lines(interactive, "(X r Y) => (X p Y)\nx10 r b10\n");
    collector.clear();
    interactive.process(".explain (x10 p b10) 0");
    CHECK(explained(collector) == "x10 p b10\n   └─ x10 r b10  [axiom]");

    // And the rules that have
    // been removed since.
    interactive.process(".remove-rules");
    collector.clear();
    interactive.process(".explain (x10 p b10) 0");
    CHECK(explained(collector) == "x10 p b10  [axiom]");
}

// Whether a rule is in force hinges on whether its condition and consequence
// are mentioned, and that evaluation replicated the whole adjacency structure
// of a node to detect a single edge. Each rule constructed by a generator
// maintains the template's inner variable, so that variable's adjacency holds
// two nodes for every rule, and reading the rules in force scaled with the
// square of their number: two seconds when reaching twenty thousand. A rule
// produced by a second generator was explained by joining the first generator
// as well, across all its solutions, even though its template is incapable of
// generating that rule, and this operation required over a second per
// invocation.
TEST_CASE("explain: twenty thousand generated rules are read and told apart quickly" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".deductions off");
    interactive.process(".auto-run"); // each line corresponds to one run: completing a single run across all of them takes minutes
    std::string script = "(A knows B) => ((X p B) => (X q B))\n(R is transitive) => ((X R Y, Y R Z) => (X R Z))\nr is transitive\na r b\nb r c\n";
    for (int i = 0; i < 20000; ++i)
        script += "tom knows b" + std::to_string(i) + "\nx" + std::to_string(i) + " p b" + std::to_string(i) + "\n";
    process_lines(interactive, script);
    interactive.run(false, false, false);

    collector.clear();
    auto start = std::chrono::steady_clock::now();
    interactive.process(".explain (x10000 q b10000) 0");
    const double first = seconds_since(start);
    CAPTURE(first);
    CHECK(explained(collector) == "x10000 q b10000\n   └─ x10000 p b10000  [axiom]");
    CHECK(first < 0.6);

    collector.clear();
    start = std::chrono::steady_clock::now();
    interactive.process(".explain ((X r Y, Y r Z) => (X r Z)) 0");
    const double generated = seconds_since(start);
    CAPTURE(generated);
    CHECK(any_output_contains(collector, "r is transitive  [axiom]"));
    CHECK(generated < 0.6);
}

// The interpretation of a rule is decided by the tags attached to its
// condition, not by the rule node itself: a ground condition is jointly
// held by the asserted fact and every rule that writes it, and any
// subsequent rule that writes it negated tags it for all of them. The list
// of rules kept from one explanation to the next was validated solely
// against the rule nodes, hence a rule kept its positive interpretation
// even when a fresh session reads it as negated, as occurs during a forward
// pass.
TEST_CASE("explain: a condition that comes to be negated is read again")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, ":a n\n(:a n) => (:r n)\n");
    interactive.run(true, false, false);
    collector.clear();
    interactive.process(".explain (:r n) 0");
    REQUIRE(explained(collector) == ":r n\n   └─ :a n  [axiom]");

    process_lines(interactive, "¬(:a n) => (:z n)\n");
    collector.clear();
    interactive.process(".explain (:r n) 0");
    CHECK(explained(collector) == ":r n  [asserted; no derivation found]");
}

// A lattice in which the search meets shared subproofs deep down first,
// subject to a depth limit: each fact is searched again from every less deep
// location, up to max-depth times, and each of these searches joined (Y st
// good) against all of the lattice before the edges that bind Y -- 16 s at
// limit 50, with a fourteenth of that time allocated to anything else. The
// join starts at the edges now.
// The rules read for one explanation are kept for the next as long as nothing
// they were read from has altered. The count of edges from the nodes they were
// read from did not indicate this: eliminating two statements regarding the
// ground condition (a p b) and writing a rule that negates it put the count to
// its prior state, the rule was adopted along with its positive condition, and
// the explanation printed a step that the rule currently in force does not
// permit -- a step that a session lacking the earlier explanation would not
// have produced.
TEST_CASE("explain: a rule read before edges were removed and others added is read again")
{
    const auto session = [](const bool explain_first)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
(a p b) note z
(a p b) note w
(a p b) note v
((a p b), (X q Y)) => (X r Y)
x q y
)");
        if (explain_first) interactive.process(".explain (x r y) 0");
        process_lines(interactive, R"(
.prune-facts (a p b) note z
.prune-facts (a p b) note w
(¬(a p b), (X s Y)) => (X t Y)
)");
        collector.clear();
        interactive.process(".explain (x r y) 0");
        return explained(collector);
    };
    CHECK(session(true) == session(false));
    CHECK(session(true) == "x r y  [asserted; no derivation found]");
}

// A load writes into the graph without a removal the rules read for an
// explanation could see: .load merges a file into the session node by node,
// such that a node acquires the edges the file holds. g2 is g1 with the two
// statements regarding (a p b) pruned and a rule written that negates it,
// thereby maintaining the count of edges of (a p b) unchanged, so only the
// epoch of the graph can tell that the rule now reads negated -- and only a
// load that cleared the graph advanced it. The rule was adopted with its
// positive condition, and the explanation printed a step the rule in force
// does not license, over a premise it labelled "[axiom; negated by a rule]".
// The removals were made in the session that saved g2, so the session that
// loads it never saw them; both loads are ordinary commands.
TEST_CASE("explain: a rule read before a .load is read again after it")
{
    namespace fs      = std::filesystem;
    const fs::path g1 = fs::temp_directory_path() / "zelph_explain_load_g1.bin";
    const fs::path g2 = fs::temp_directory_path() / "zelph_explain_load_g2.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
(a p b) note z
(a p b) note w
((a p b), (X q Y)) => (X r Y)
x q y
)");
        interactive.process(".save " + g1.generic_string());
        process_lines(interactive, R"(
.prune-facts (a p b) note z
.prune-facts (a p b) note w
(¬(a p b), (X s Y)) => (X t Y)
)");
        interactive.process(".save " + g2.generic_string());
    }
    const auto session = [&](const bool explain_first)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + g1.generic_string());
        if (explain_first) interactive.process(".explain (x r y) 0");
        interactive.process(".load " + g2.generic_string());
        collector.clear();
        interactive.process(".explain (x r y) 0");
        return explained(collector);
    };
    CHECK(session(true) == session(false));
    CHECK(session(true) == "x r y  [asserted; no derivation found]");
    fs::remove(g1);
    fs::remove(g2);
}

// The rules read for one explanation name the nodes they were read from to
// the graph, and a removal notes those of them it touches, nothing else: a
// prune of a hundred thousand other facts neither grows that record nor hides
// the two removals that matter among it. Stamping every node a removal
// touched cost a predicate-wide prune of the Wikidata class hierarchy by 3 to
// 5 percent, and a capped record of them would have needed to be discarded
// entirely once full.
TEST_CASE("explain: a rule is read again after a removal touched it among many others")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
a p b
(a p b) note z
(a p b) note w
(a p b) note v
((a p b), (X q Y)) => (X r Y)
x q y
)");
    const auto graph = interactive.graph();
    for (int i = 0; i < 100000; ++i)
        assert_fact(graph, "f" + std::to_string(i), "s", "g");

    collector.clear();
    interactive.process(".explain (x r y) 0");
    REQUIRE(explained(collector) == "x r y\n   ├─ x q y  [axiom]\n   └─ a p b  [axiom]");

    process_lines(interactive, R"(
.prune-facts S s g
.prune-facts (a p b) note z
.prune-facts (a p b) note w
(¬(a p b), (X s Y)) => (X t Y)
)");
    collector.clear();
    interactive.process(".explain (x r y) 0");
    CHECK(explained(collector) == "x r y  [asserted; no derivation found]");
}

TEST_CASE("explain: a lattice under a depth limit is explained without joining every fact per step")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run");
    process_lines(interactive, "(X st src) => (X st good)\n(X l Y, X r Z, Y st good, Z st good) => (X st good)\n");

    auto* const           graph  = interactive.graph();
    constexpr std::size_t height = 60;
    const auto            cell   = [](const std::size_t i, const std::size_t j)
    { return "n" + std::to_string(i) + "_" + std::to_string(j); };
    for (std::size_t i = 0; i <= height; ++i)
    {
        for (std::size_t j = 0; j <= i; ++j)
        {
            assert_fact(graph, cell(i, j), "st", "good");
            if (i == height)
            {
                assert_fact(graph, cell(i, j), "st", "src");
                continue;
            }
            assert_fact(graph, cell(i, j), "l", cell(i + 1, j));
            assert_fact(graph, cell(i, j), "r", cell(i + 1, j + 1));
        }
    }
    for (std::size_t i = 0; i <= 50; ++i)
    {
        const std::string a = "a" + std::to_string(i);
        assert_fact(graph, a, "st", "good");
        assert_fact(graph, a, "l", cell(0, 0));
        assert_fact(graph, a, "r", i < 50 ? "a" + std::to_string(i + 1) : cell(0, 0));
    }

    collector.clear();
    const auto start = std::chrono::steady_clock::now();
    interactive.process(".explain (a0 st good) 50");
    CHECK(seconds_since(start) < 8.0);
    CHECK(fact_under_itself(explained(collector)).empty());
    CHECK(graph->last_explain_counts().most <= 50);
}

TEST_CASE("explain: shared subproofs print once")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // (a q b) is DERIVED and used twice: directly as a premise of
        // (a done b), and again as the premise of (a r b). Hash-consing
        // makes both occurrences the same node, so the second one must
        // reference the first instead of re-printing its subtree.
        // Axiom leaves are NOT subject to this: "[axiom]" already is the
        // complete expansion and stays readable when repeated.
        interactive.process("(X p Y) => (X q Y)");
        interactive.process("(X q Y) => (X r Y)");
        interactive.process("(X q Y, X r Y) => (X done Y)");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (a done b) 0");
        CHECK(any_output_contains(collector, "[see above]")); });
}

TEST_CASE("explain: NAF premises render as absent (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import primes-naf");
        interactive.process("? :testprime &7");

        collector.clear();
        interactive.process(".explain (:testprime &7) = prime 0");
        CHECK(any_output_contains(collector, "[absent]")); });
}

TEST_CASE("explain: a numeral object is not mistaken for the depth argument")
{
    // A trailing all-digit token is read as max-depth. That reading must
    // not swallow the fact's own OBJECT: ".explain <subject> <pred> 0"
    // would otherwise leave a two-component statement behind, which the
    // AST builder rejects with a leaked arity error. Every digit-level
    // fact of the arithmetic modules has this shape, so the failure was
    // reachable from the very first thing a reader is likely to inspect.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // (1 gate 1) is derived, so that a depth limit has something to cut:
        // when an asserted premise reaches the limit, it is considered
        // complete and prints as [axiom].
        interactive.process("(A gate B) => ((A gate B) out 0)");
        interactive.process("(A gated B) => (A gate B)");
        interactive.process("1 gated 1");
        interactive.run(true, false, false);

        SUBCASE("the numeral stays part of the pattern")
        {
            collector.clear();
            interactive.process(".explain (1 gate 1) out 0");
            CHECK(any_output_contains(collector, "1 gate 1"));
            CHECK_FALSE(any_output_contains(collector, "arity mismatch"));
            CHECK_FALSE(any_output_contains(collector, "cannot parse"));
        }
        SUBCASE("the parenthesized spelling keeps working")
        {
            collector.clear();
            interactive.process(".explain ((1 gate 1) out 0)");
            CHECK(any_output_contains(collector, "1 gate 1"));
            CHECK_FALSE(any_output_contains(collector, "cannot parse"));
        }
        SUBCASE("an explicit depth is still honoured")
        {
            // Reading (1) resolves here, so the trailing token IS the depth
            // -- and depth 1 must cut the tree below the root.
            collector.clear();
            interactive.process(".explain ((1 gate 1) out 0) 1");
            CHECK(any_output_contains(collector, "depth limit"));
        } });
}

TEST_CASE("explain: a NAF premise is printed bound, and negated exactly once")
{
    // The rule's negated condition is stored as a pattern node tagged
    // (~ negation), which node_to_string already renders as "¬(...)".
    // The renderer used to add a SECOND "¬(...)" around it and passed no
    // bindings, so the honest premise "¬(plant2 is green)" came out as
    // "¬(¬(A is green))" -- the opposite claim, with an unbound variable.
    // Variables that occur ONLY inside the negation stay unbound on
    // purpose: that is what "for no D" quantifies over.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A is yellow, ¬(A is green)) => (A notgreen green)");
        interactive.process("plant is green");
        interactive.process("plant is yellow");
        interactive.process("plant2 is yellow");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain plant2 notgreen green");
        CHECK(any_output_contains(collector, "¬(plant2 is green)  [absent]"));
        CHECK_FALSE(any_output_contains(collector, "¬(¬")); });
}

TEST_CASE("explain: a negation with a variable free inside it is checked, not assumed")
{
    // ¬(A q B) is satisfied when NO fact (x q _) is present: B is quantified
    // within the negation, and the join never establishes a binding for it.
    // The absence test formerly required identifying the ground fact under
    // the bindings, finding the pattern still carrying B, and treating that
    // as absence -- thus causing a fact whose negated premise is true to be
    // displayed as derived, accompanied by `¬(x q B) [absent]` next to the
    // fact (x q y) that refutes it. The identity fallback in symbolic-core,
    // ¬(T rw S), precisely matches this shape, and the paper quotes its
    // .explain.
    //
    // The assertion of (x r x) occurs here instead of being inferred: since
    // the rule was never fired for it, the sole possible justification is
    // eliminated by the negation, and no aspect of the evaluation sequence
    // plays a role.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A p A, ¬(A q B)) => (A r A)");
        interactive.process("x q y");
        interactive.process(":p x");
        interactive.process(":r x");
        interactive.process(":p w");
        interactive.run(true, false, false);

        SUBCASE("a negation some fact satisfies disqualifies the justification")
        {
            collector.clear();
            interactive.process(".explain (:r x)");
            CHECK(any_output_contains(collector, "no derivation found"));
            CHECK_FALSE(any_output_contains(collector, "[absent]"));
        }
        SUBCASE("a negation no fact satisfies is still reported absent")
        {
            collector.clear();
            interactive.process(".explain (:r w)");
            CHECK(any_output_contains(collector, "¬(w q B)  [absent]"));
        } });
}

// A negated path condition is satisfied by a walk that finds no path, thus
// requiring verification through walking, just as the forward pass does. The
// search tested it as a pattern, which never yields a match -- a walk lacks a
// fact node -- and consequently continued to output ¬((x r y) closure
// one-or-more) [absent] even after (x r y) established the existence of the
// path. (x q y) remains, since every fact persists, yet the justification
// under which it was derived no longer holds.
//
// The REPL's default mode: in check mode, a negation that has become true
// triggers an error, whereas the REPL maintains the fact.
TEST_CASE("explain: a negated path condition is walked, not looked up")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".semi-naive on");

    process_lines(interactive, R"(
(X p Y, ¬(X r⁺ Y)) => (X q Y)
x p y
)");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process(".explain (x q y) 0");
    REQUIRE(any_output_contains(collector, "¬((x r y) closure one-or-more) [absent]"));

    interactive.process("x r y");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process(".explain (x q y) 0");
    CHECK(explained(collector) == "x q y  [asserted; no derivation found]");
}

// A path condition is satisfied by a walk, printed as a [closure] leaf that
// does not undergo additional search -- and the walk can traverse the very
// fact under explanation. (a sub b) is asserted, and (x in b) is derived
// from (x in a) through walking a sub⁺ b, whose sole edge is (a sub b)
// itself; (a sub b) is subsequently derived from (x in b) in turn. The tree
// showed (a sub b) derived from a walk on itself. The edges within a walk
// now count as premises for the step: (x in b) and (a sub b) form a cycle,
// resolved as one, and the edge that ends the proof appears beneath the
// walk.
TEST_CASE("explain: a path condition does not walk over the fact being explained")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((X in C), (C sub⁺ D)) => (X in D)
((X in D), (X anchor A), (D marked yes)) => (A sub D)
x anchor a
x in a
b marked yes
a sub b
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (a sub b) 0");
        const std::string tree = explained(collector);
        CHECK(tree.rfind("a sub b\n", 0) == 0);
        CHECK(tree.find("[closure]") == std::string::npos);
        CHECK(tree.find("x in b  [asserted; no derivation found]") != std::string::npos);

        // When the walk is not dependent on the fact, it stays a
        // premise.
        collector.clear();
        interactive.process(".explain (x in b) 0");
        CHECK(explained(collector) == "x in b\n"
                                      "   ├─ x in a  [asserted; no derivation found]\n"
                                      "   └─ (a sub b) closure one-or-more  [closure]\n"
                                      "      └─ a sub b  [asserted; no derivation found]"); });
}

// A walk can traverse not only the facts under investigation but also a
// derived edge whose sole derivation relies on the fact explained, a fact
// involving multiple objects, or one of two possible routes. The edges
// comprising a walk count as premises for the step now -- unless its proof
// ends at a leaf within a cycle (refer to the section below), an edge is not
// printed, and the walk remains confined to a single [closure] line -- thus,
// an edge requiring the fact explained puts the step into its cycle, and
// among the two routes, either can carry the walk. Each case printed the root
// derived from a walk over itself, or a derivable fact as
// "[asserted; no derivation found]".
TEST_CASE("explain: the edges of a walk count as its premises")
{
    SUBCASE("an edge derived only from the fact explained")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X in C), (C sub⁺ D)) => (X in D)
((X in D), (X anchor A), (D marked yes)) => (A r D)
(A r D) => (A sub D)
x anchor a
x in a
b marked yes
a r b
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (a r b) 0");
        const std::string tree = explained(collector);
        CHECK(tree.find("[closure]") == std::string::npos);
        CHECK(tree.find("x in b  [asserted; no derivation found]") != std::string::npos);
    }
    SUBCASE("an edge of a longer walk derived only from the fact explained")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X at C), (C sub⁺ D)) => (X at D)
((X at D), (X anchor A), (D marked yes)) => (A sub D)
((A sub b), (A twin C)) => (C sub b)
x anchor a
x at a
b marked yes
a twin c
a sub c
a sub b
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (a sub b) 0");
        const std::string tree = explained(collector);
        CHECK(tree.find("(c sub b) closure") == std::string::npos);
        CHECK(tree.find("x at b  [asserted; no derivation found]") != std::string::npos);
    }
    SUBCASE("a fact with several objects")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X at C), (C sub⁺ D)) => (X at D)
((X at D), (X anchor A), (D marked yes), (A pair E)) => (A sub D E)
x anchor a
x at a
a pair q
b marked yes
a sub b q
)");
        interactive.run(true, false, false);
        for (const char* depth : {"0", "3"})
        {
            CAPTURE(depth);
            collector.clear();
            interactive.process(std::string(".explain (a sub b q) ") + depth);
            const std::string tree = explained(collector);
            CHECK(tree.find("[closure]") == std::string::npos);
            CHECK(tree.find("x at b  [asserted; no derivation found]") != std::string::npos);
        }
    }
    SUBCASE("either of two routes")
    {
        // (x at b) holds through (x at a) and a walk originating at a and
        // ending at b, either directly via (a sub b) or via the sequence
        // a -> c -> b, where (c sub b) is derived from (e sub b). The dual
        // ordering of the link facts makes the search meet the routes in
        // either order.
        for (const bool a_first : {true, false})
        {
            CAPTURE(a_first);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
((X at C), (C sub⁺ D), (D marked yes)) => (X at D)
((X at D), (X anchor A), (D marked yes)) => (A sub D)
((A link C), (A sub b)) => (C sub b)
((C sub b), (X at b)) => (C reach b)
x anchor a
x at a
b marked yes
)");
            process_lines(interactive, a_first ? "a link c\ne link c\n" : "e link c\na link c\n");
            process_lines(interactive, "e sub b\na sub c\n");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".explain (c reach b) 0");
            const std::string tree = explained(collector);
            CHECK(tree.find("x at b  [asserted; no derivation found]") == std::string::npos);
            CHECK(tree.find("x at a") != std::string::npos);
        }
    }
}

// A walk taken as the shortest one around the facts being searched can still
// traverse an edge that stems solely from the step's own conclusion, an edge
// that is not being searched yet at the time the walk is taken: (a sub b)
// relies on (x at b), and (c sub b) depends on (a sub b). The step thus became
// circular, and a fact that is derivable was printed as
// "[asserted; no derivation found]", despite the existence of a longer walk
// through edges of their own -- a sub c, c sub b <= c base b in the first
// program, a -> d -> e -> b in the subsequent one -- which derives it. Among
// the walks that exclude the facts being searched, one was taken and no
// others; among those that go through them, four were. A walk whose edges turn
// out to be searched, and which causes the step to pause, is now succeeded by
// a walk circumventing them, provided such a walk exists, and each fact being
// searched on the first walk receives a walk that bypasses it.
TEST_CASE("explain: a walk around the step's own conclusion gives way to one that is not")
{
    SUBCASE("the shortest walk avoiding the facts searched runs over the root")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X at C), (C sub⁺ D), (D marked yes)) => (X at D)
((X at D), (X anchor A), (D marked yes)) => (A sub D)
((A link C), (A sub b)) => (C sub b)
(C base D) => (C sub D)
((C sub b), (X at b)) => (X reach b)
x anchor a
x at a
b marked yes
a link c
a sub c
c base b
)");
        interactive.run(true, false, false);
        for (const char* query : {".explain (x reach b) 0", ".explain (x at b) 0", ".explain (a sub b) 0"})
        {
            CAPTURE(query);
            collector.clear();
            interactive.process(query);
            const std::string tree = explained(collector);
            CHECK(tree.find("a sub b  [asserted; no derivation found]") == std::string::npos);
            CHECK(tree.find("x at b  [asserted; no derivation found]") == std::string::npos);
            CHECK(fact_under_itself(tree).empty());
        }
    }
    SUBCASE("the shortest walk runs over an edge the root derives")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X at C), (C sub⁺ D), (D marked yes)) => (X at D)
((X at D), (X anchor A), (D marked yes)) => (A sub D)
((A sub b), (A twin C)) => (C sub b)
x anchor a
x at a
b marked yes
a twin c
a sub c
a sub d
d sub e
e sub b
a sub b
)");
        interactive.run(true, false, false);
        for (const char* depth : {"0", "3"})
        {
            CAPTURE(depth);
            collector.clear();
            interactive.process(std::string(".explain (a sub b) ") + depth);
            const std::string tree = explained(collector);
            CHECK(tree.rfind("a sub b\n", 0) == 0);
            CHECK(tree.find("x at b  [asserted; no derivation found]") == std::string::npos);
            CHECK(tree.find("   └─ x at b\n") != std::string::npos);
        }
    }
    SUBCASE("the walk that leaves out the root is the fifth")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X at C), (C sub⁺ D), (D marked yes)) => (X at D)
((X at D), (X anchor A), (D marked yes)) => (A sub D)
((s sub n1), (n3 sub b)) => (x anchor n3)
(n1 sub n2) => (s sub n1)
(n2 sub n3) => (n1 sub n2)
(s sub u1) => (n2 sub n3)
(n3 sub b) => (s sub u1)
(A base B) => (A sub B)
x anchor n3
x at s
b marked yes
s base n1
n1 base n2
n2 base n3
s base u1
s sub v1
v1 sub n1
n1 sub v2
v2 sub n2
n2 sub v3
v3 sub n3
u1 sub u2
u2 sub u3
u3 sub u4
u4 sub u5
u5 sub b
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (n3 sub b) 0");
        const std::string tree = explained(collector);
        CHECK(tree.find("x at b  [asserted; no derivation found]") == std::string::npos);
        CHECK(tree.find("   └─ x at b\n") != std::string::npos);
        CHECK(fact_under_itself(tree).empty());

        // When explained independently, (x at b) traverses (n3 sub b), which
        // is solely derived by (x at b), and the walk around it runs over (s
        // sub u1). This particular fact is being searched -- it is entered
        // beneath (n2 sub n3), and its instantiation over (n3 sub b) precedes
        // the one over (s base u1) -- yet it is clean, and the walk passed
        // around it too, thus nothing was found, and (n3 sub b) was printed as
        // "[asserted; no derivation found]".
        collector.clear();
        interactive.process(".explain (x at b) 0");
        CHECK(explained(collector) == "x at b\n"
                                      "   ├─ b marked yes  [axiom]\n"
                                      "   ├─ x at s  [asserted; no derivation found]\n"
                                      "   └─ (s sub b) closure one-or-more  [closure]");
        auto* const graph = interactive.graph();
        const auto  proof = graph->explain(fact_node(graph, "x", "at", "b"), 0);
        REQUIRE(proof != nullptr);
        CHECK(first_walk(*proof) == std::vector<zelph::network::Node>{fact_node(graph, "s", "sub", "u1"), fact_node(graph, "u1", "sub", "u2"), fact_node(graph, "u2", "sub", "u3"), fact_node(graph, "u3", "sub", "u4"), fact_node(graph, "u4", "sub", "u5"), fact_node(graph, "u5", "sub", "b")});
    }

    // (sK sub uK) holds through either (sK base uK) or (sK link uK), with
    // one of these depending on (mK sub bK), which only the root derives.
    // The search for (sK sub uK) occurs beneath (sK sub mK), and when the
    // circular instantiation comes first, it stays on the search stack even
    // though the other one provides grounding, just as (sK sub mK) does. The
    // walk around (mK sub bK) went around every fact being searched,
    // including those already deemed clean, resulting in no findings, and
    // thus the root's step was printed over (mK sub bK) as
    // "[asserted; no derivation found]". Which instantiation comes first is
    // determined by the rule order, ensuring the network holds both role
    // assignments. `route`: the walk the root's step follows, '%'
    // representing K.
    const auto walks_around_root = [](const std::string& added, const std::vector<std::pair<std::string, std::string>>& route)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X at C), (C sub⁺ D), (D marked yes)) => (X at D)
(A base B) => (A sub B)
(A link B) => (A sub B)
(x1 at b1) => (m1 sub b1)
(m1 sub b1) => (s1 link u1)
(s1 sub u1) => (s1 sub m1)
(x2 at b2) => (m2 sub b2)
(m2 sub b2) => (s2 base u2)
(s2 sub u2) => (s2 sub m2)
x1 at s1
b1 marked yes
s1 base u1
x2 at s2
b2 marked yes
s2 link u2
)");
        process_lines(interactive, added);
        interactive.run(true, false, false);
        auto* const graph = interactive.graph();
        for (const std::string k : {"1", "2"})
        {
            CAPTURE(k);
            collector.clear();
            const auto numbered = [&](std::string text)
            {
                std::replace(text.begin(), text.end(), '%', k.front());
                return text;
            };
            interactive.process(".explain (x" + k + " at b" + k + ") 0");
            CHECK(explained(collector) == numbered("x% at b%\n"
                                                   "   ├─ b% marked yes  [axiom]\n"
                                                   "   ├─ x% at s%  [asserted; no derivation found]\n"
                                                   "   └─ (s% sub b%) closure one-or-more  [closure]"));
            std::vector<zelph::network::Node> walk;
            for (const auto& [from, to] : route)
                walk.push_back(fact_node(graph, numbered(from), "sub", numbered(to)));
            const auto proof = graph->explain(fact_node(graph, "x" + k, "at", "b" + k), 0);
            REQUIRE(proof != nullptr);
            CHECK(first_walk(*proof) == walk);
        }
    };
    const std::vector<std::pair<std::string, std::string>> by_u{{"s%", "u%"}, {"u%", "w%"}, {"w%", "b%"}};
    SUBCASE("a fact being searched that is already clean is not walked around")
    {
        walks_around_root("u1 sub w1\nw1 sub b1\nu2 sub w2\nw2 sub b2\n", by_u);
    }
    // The sole traversal circumventing (mK sub bK) proceeds across (sK sub
    // mK), which the traversal encountered likewise. It is clean, thus it
    // does not make the step pause, and the walk around the edges met leaves
    // it in.
    SUBCASE("a fact being searched that is already clean stays on the walk")
    {
        walks_around_root("m1 sub z1\nz1 sub b1\nm2 sub z2\nz2 sub b2\n", {{"s%", "m%"}, {"m%", "z%"}, {"z%", "b%"}});
    }
    // The fact that grounds (sK sub uK) is asserted, and a rule concludes it
    // that lacks any instantiation: (sK sub uK) is founded, not clean, thus
    // it remains unsettled while it is being searched, and a walk around
    // every fact being searched that is not settled finds nothing either.
    // The walk around the edges met alone runs over it.
    SUBCASE("a founded fact being searched is walked over where nothing else is left")
    {
        walks_around_root("u1 sub w1\nw1 sub b1\nu2 sub w2\nw2 sub b2\n(s1 pre u1) => (s1 base u1)\n(s2 pre u2) => (s2 link u2)\n", by_u);
    }
}

// The facts traversed by a walk are premises of the step, and they are not
// displayed alongside the step. If the SCC rule ends the proof at one of
// these premises, that leaf was also not printed: the tree showed a step
// originating from axioms and a [closure], where the walk traverses a fact
// that only the step's conclusion itself derives. A walk edge whose proof
// ends at a leaf of the SCC rule on a cycle is now printed beneath the
// [closure] line, complete with its label, ensuring that every such leaf
// appears within the tree.
TEST_CASE("explain: a fact a walk steps over that ends the proof is printed below the walk")
{
    SUBCASE("the edge is the leaf")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(X r1 Y) => (Y r0 X)
((X r0 Y), (Y r1 Z)) => (X r2 Z)
((X r1 Y), ((X r0 Y) closure zero-or-more)) => (X r2 X)
((X r0 Y), ((Y r1 X) closure one-or-more)) => (X r1 Y)
(X r2 Y) => (Y r0 X)
((X r2 Y), ((X r1 Y) closure zero-or-more)) => (X r0 X)
a r2 c
c r0 b
c r1 a
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (c r1 a) 0");
        CHECK(explained(collector) == "c r1 a\n"
                                      "   ├─ c r0 a  [asserted; no derivation found]\n"
                                      "   └─ (a r1 c) closure one-or-more  [closure]\n"
                                      "      └─ a r1 c  [asserted; no derivation found]");
    }
    SUBCASE("the edge is derived from the leaf")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X in C), (C sub⁺ D)) => (X in D)
((X in D), (X anchor A), (D marked yes)) => (A r D)
(A r D) => (A sub D)
x anchor a
x in a
b marked yes
x in b
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (x in b) 0");
        const std::string tree = explained(collector);
        CHECK(tree.rfind("x in b\n"
                         "   ├─ x in a  [asserted; no derivation found]\n"
                         "   └─ (a sub b) closure one-or-more  [closure]\n"
                         "      └─ a sub b",
                         0)
              == 0);
        CHECK(tree.find("[asserted; no derivation found]", tree.find("(a sub b) closure")) != std::string::npos);
        CHECK(fact_under_itself(tree).empty());
    }
}

// Only a walk edge whose proof ends at a leaf ON A CYCLE is printed
// beneath its [closure] line. A fact not part of any cycle forms a
// standalone component, and if it lacks a derivation -- being either an
// axiom or an asserted fact no instantiation of which holds -- the SCC
// rule assigns it as the leaf of that component. This kind of leaf was
// marked identically to a leaf within a real cycle, so every walk edge
// that a rule concludes was printed with its whole proof: an edge
// derived from an axiom, and, with a rule in force whose
// consequence involves a variable predicate (the inverse rule of
// wikidata.zph), every asserted edge in a class-hierarchy walk was shown
// as "[asserted; no derivation found]". Whether an edge was printed
// additionally depended on the order of the search, as an axiom taken
// without a search was not marked.
TEST_CASE("explain: a walk edge whose proof ends on no cycle is not printed")
{
    SUBCASE("an edge derived from an axiom")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X start C), (C sub⁺ D)) => (X at D)
(X base Y) => (X sub Y)
x start a
a base b
)");
        interactive.run(true, false, false);
        for (const char* depth : {"0", "2"})
        {
            CAPTURE(depth);
            collector.clear();
            interactive.process(std::string(".explain (x at b) ") + depth);
            CHECK(explained(collector) == "x at b\n"
                                          "   ├─ x start a  [axiom]\n"
                                          "   └─ (a sub b) closure one-or-more  [closure]");
        }
    }
    SUBCASE("an asserted edge that a rule with a variable predicate matches")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X inst C), (C sub⁺ D)) => (X inst D)
((R inverse S), (X R Y)) => (Y S X)
parent inverse child
a sub b
x inst a
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (x inst b) 0");
        CHECK(explained(collector) == "x inst b\n"
                                      "   ├─ x inst a  [asserted; no derivation found]\n"
                                      "   └─ (a sub b) closure one-or-more  [closure]");
    }
    SUBCASE("an axiom reached with and without a search")
    {
        // (a n m) holds a proof via (a q m), which remains
        // ungrounded by any instantiation, before taking (a base m)
        // without conducting a search; (m base b) undergoes searching. Both
        // walk edges are similarly derived from an axiom, and solely the
        // second one was printed.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X start C), (C n M), (C sub⁺ D), (D marked yes)) => (X at D)
(X base Y) => (X sub Y)
(X w Y) => (X q Y)
(X base Y) => (X n Y)
(X q Y) => (X n Y)
x start a
a q m
a base m
m base b
b marked yes
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (x at b) 0");
        const std::string tree = explained(collector);
        CHECK(tree.find("(a sub b) closure one-or-more  [closure]") != std::string::npos);
        CHECK(tree.find("a sub m") == std::string::npos);
        CHECK(tree.find("m sub b") == std::string::npos);
    }
    SUBCASE("an edge on a cycle that does not lead back to the step is printed")
    {
        // Each of (a sub b) and (b super a) is derived
        // from the other.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X inst C), (C sub⁺ D)) => (X inst D)
(X sub Y) => (Y super X)
(Y super X) => (X sub Y)
a sub b
x inst a
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (x inst b) 0");
        CHECK(explained(collector) == "x inst b\n"
                                      "   ├─ x inst a  [asserted; no derivation found]\n"
                                      "   └─ (a sub b) closure one-or-more  [closure]\n"
                                      "      └─ a sub b\n"
                                      "         └─ b super a  [asserted; no derivation found]");
    }
    SUBCASE("an edge whose only instantiation needs itself is printed")
    {
        // A cycle involving a single fact: the component features one
        // member, and the leaf resides on a cycle regardless.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
((X inst C), (C sub⁺ D)) => (X inst D)
((X sub Y), (X ok Y)) => (X sub Y)
a ok b
a sub b
x inst a
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (x inst b) 0");
        CHECK(explained(collector) == "x inst b\n"
                                      "   ├─ x inst a  [asserted; no derivation found]\n"
                                      "   └─ (a sub b) closure one-or-more  [closure]\n"
                                      "      └─ a sub b  [asserted; no derivation found]");
    }
}

// The facts traversed by a walk lie beneath its [closure] line, positioned
// two levels beneath the step, and the depth limit cuts them just as it
// does any other premise. Regardless of the level, the group of them was
// opened, so at the limit they were printed one level below it: a derived
// edge bearing "… [depth limit]", a leaf carrying its label, and the trees
// at max-depth d and d + 1 remained identical. A proof whose edges are
// concealed by the limit is counted as cut, just as one whose premise
// stands at the limit is, and thus it is expanded again at a higher point
// within the tree.
TEST_CASE("explain: the facts a walk steps over are cut at the depth limit like premises")
{
    SUBCASE("an edge derived from a leaf of its cycle")
    {
        // Each of (a p b) and (b p a) is derived
        // from the other.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(X p Y) => (Y p X)
((X start C), (C p⁺ D)) => (X at D)
x start a
a p b
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (x at b) 1");
        CHECK(explained(collector) == "x at b\n"
                                      "   ├─ x start a  [axiom]\n"
                                      "   └─ (a p b) closure one-or-more  [closure]");
        CHECK_FALSE(interactive.graph()->last_explain_counts().limited);

        collector.clear();
        interactive.process(".explain (x at b) 2");
        CHECK(explained(collector) == "x at b\n"
                                      "   ├─ x start a  [axiom]\n"
                                      "   └─ (a p b) closure one-or-more  [closure]\n"
                                      "      └─ a p b  … [depth limit -- use '.explain <pattern> 0' for the full proof]");
    }
    SUBCASE("an edge that is the leaf")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(X r1 Y) => (Y r0 X)
((X r0 Y), (Y r1 Z)) => (X r2 Z)
((X r1 Y), ((X r0 Y) closure zero-or-more)) => (X r2 X)
((X r0 Y), ((Y r1 X) closure one-or-more)) => (X r1 Y)
(X r2 Y) => (Y r0 X)
((X r2 Y), ((X r1 Y) closure zero-or-more)) => (X r0 X)
a r2 c
c r0 b
c r1 a
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (c r1 a) 1");
        CHECK(explained(collector) == "c r1 a\n"
                                      "   ├─ c r0 a  [asserted; no derivation found]\n"
                                      "   └─ (a r1 c) closure one-or-more  [closure]");

        // The complete proof is held
        // across two levels.
        collector.clear();
        interactive.process(".explain (c r1 a) 0");
        const std::string complete = explained(collector);
        collector.clear();
        interactive.process(".explain (c r1 a) 2");
        CHECK(explained(collector) == complete);
    }
    SUBCASE("a proof whose edges the limit hides is expanded again higher up")
    {
        // At level 3, (c r1 a) is printed first, through (c d2 a)
        // and (c d1 a), where the limit hides the edge of its walk, and
        // subsequently at level 1 as the root's second premise; the
        // premises are printed in the sequence dictated by the root rule's
        // conditions. The edge was printed at level 5, positioned beneath
        // the limit, and the second line stated "[see above]". With the
        // edge cut yet the expansion not counted as cut, that line
        // referred to an expansion which conceals the edge.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(X r1 Y) => (Y r0 X)
((X r0 Y), (Y r1 Z)) => (X r2 Z)
((X r1 Y), ((X r0 Y) closure zero-or-more)) => (X r2 X)
((X r0 Y), ((Y r1 X) closure one-or-more)) => (X r1 Y)
(X r2 Y) => (Y r0 X)
((X r2 Y), ((X r1 Y) closure zero-or-more)) => (X r0 X)
(X r1 Y) => (X d1 Y)
(X d1 Y) => (X d2 Y)
((X d2 Y), (X r1 Y)) => (X top Y)
a r2 c
c r0 b
c r1 a
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (c top a) 4");
        CHECK(explained(collector) == "c top a\n"
                                      "   ├─ c d2 a\n"
                                      "   │  └─ c d1 a\n"
                                      "   │     └─ c r1 a\n"
                                      "   │        ├─ c r0 a  [asserted; no derivation found]\n"
                                      "   │        └─ (a r1 c) closure one-or-more  [closure]\n"
                                      "   └─ c r1 a\n"
                                      "      ├─ c r0 a  [asserted; no derivation found]\n"
                                      "      └─ (a r1 c) closure one-or-more  [closure]\n"
                                      "         └─ a r1 c  [asserted; no derivation found]");
    }
}

TEST_CASE("explain: a quoted multi-word predicate resolves")
{
    // zelph PRINTS a predicate containing spaces quoted, so pasting that
    // line back into .explain must work. The command tokenizer strips the
    // quotes, and rejoining the tokens with blanks turned `a "is not" b`
    // into the four-component `a is not b`, which denotes a different
    // node -- the fact was reported as not asserted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a \"is not\" b");

        SUBCASE(".explain finds it")
        {
            collector.clear();
            interactive.process(".explain a \"is not\" b");
            CHECK(any_output_contains(collector, "[axiom]"));
            CHECK_FALSE(any_output_contains(collector, "not asserted"));
        }
        SUBCASE("the .why alias behaves identically")
        {
            collector.clear();
            interactive.process(".why a \"is not\" b");
            CHECK(any_output_contains(collector, "[axiom]"));
        } });
}

TEST_CASE("explain: a quoted name without spaces resolves too")
{
    // Whitespace used to be the only surviving evidence that a token had
    // been quoted, so only a name with a space could be re-quoted for the
    // parser. Every other name that zelph prints quoted -- and the quoting
    // work made that a large set -- could not be pasted back: `x>y` was
    // handed to the parser bare and read as the three atoms `x > y`.
    // The tokenizer now reports each token in parser form as well.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // One representative per reason a name gets quoted: a reserved
        // character, each of the tokens the grammar reads by its first
        // character, and a name carrying a quote of its own.
        const char* names[] = {"x>y", ":foo", "&12", "_foo", "A", "a,b", "«Le Monde»"};
        int         i       = 0;
        for (const char* name : names)
        {
            const std::string subj = "s" + std::to_string(++i);
            interactive.process(subj + " rel obj" + std::to_string(i));
            interactive.process(".name obj" + std::to_string(i) + " \"" + name + "\"");

            collector.clear();
            interactive.process(".explain (" + subj + " rel \"" + name + "\")");
            CHECK(any_output_contains(collector, "[axiom]"));
            CHECK_FALSE(any_output_contains(collector, "cannot parse fact pattern"));
        }

        // A name carrying a quote, which needs the escape on both sides.
        interactive.process("sq rel objq");
        interactive.process(".name objq \"The \\\"Big\\\" One\"");
        collector.clear();
        interactive.process(".explain (sq rel \"The \\\"Big\\\" One\")");
        CHECK(any_output_contains(collector, "[axiom]")); });
}

TEST_CASE("pruning: a quoted name without spaces reaches the pattern")
{
    // Same cause, worse consequence: .prune-facts refused the pattern
    // outright ("Could not parse pattern"), so the fact zelph printed could
    // not be deleted by pasting the line back.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("sub rel obj");
        interactive.process(".name obj \"x>y\"");
        interactive.process("keep rel other");

        collector.clear();
        interactive.process(".prune-facts sub rel \"x>y\"");
        CHECK(any_output_contains(collector, "Pruned 1 matching facts"));

        collector.clear();
        interactive.process("S rel O");
        CHECK(answers_contain(collector, "keep rel other"));
        CHECK_FALSE(any_output_contains(collector, "x>y")); });
}

// The search stops at the first justification it can rebuild, which is a
// deliberate cost decision -- and it used to be invisible, so a tree over a
// fact reached two ways read as THE derivation of it. That is a stronger claim
// than the engine makes, and it is the one an auditability argument rests on.
// A second verified instantiation is now looked for at the root and named.
TEST_CASE("explain: a fact reached two ways says so")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X target Y)
(X q Y) => (X target Y)
a p b
a q b
)");
        interactive.run(false, false, false);

        collector.clear();
        interactive.process(".explain (a target b) 0");
        CHECK(any_output_contains(collector, "one of several justifications"));
        // The one it does show is still a complete, checkable derivation.
        CHECK(any_output_contains(collector, "[axiom]")); });
}

TEST_CASE("explain: a second justification through the same rule says so too")
{
    // (:q a) holds via (a p b) and also via (a p c): two solutions of a
    // single join. The root ended the join at the first proof, thus
    // allowing only a second rule to generate the annotation.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (:q X)
a p b
a p c
)");
        interactive.run(false, false, false);

        collector.clear();
        interactive.process(".explain (:q a) 0");
        CHECK(any_output_contains(collector, "one of several justifications"));
        CHECK(any_output_contains(collector, "[axiom]")); });
}

TEST_CASE("explain: the same premises with swapped bindings are not a second justification")
{
    // X = v1, Y = v2 and X = v2, Y = v1 represent two solutions of the
    // join operation, yet share a single justification: the identical rule
    // applied to the same pair of facts.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A prop X, A prop Y, X != Y) => (A ~ several)
a prop v1
a prop v2
)");
        interactive.run(false, false, false);

        collector.clear();
        interactive.process(".explain (a ~ several) 0");
        CHECK(any_output_contains(collector, "a prop v1 [axiom]"));
        CHECK_FALSE(any_output_contains(collector, "one of several justifications")); });
}

// A further instantiation of the root counts solely when it does not rest
// on the root itself. In both trees below, a second instantiation exists
// whose join holds: (:r a) via (:p a), which only (:r a) derives, and
// (:q b) via (b p b), which only (:q b) derives. The annotation used to
// tally every instantiation whose join held, thereby signalling a rule
// that connects the fact back to itself as an additional path to reach it.
TEST_CASE("explain: a rule leading from the fact back to itself is not a second justification")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(:p X) => (:r X)
(:c X) => (:r X)
(:r X) => (:p X)
:c a
(X p Y) => (X q X)
(X q Y) => (X p Y)
b p c
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (:r a) 0");
        CHECK(explained(collector) == R"(:r a
   └─ :c a  [axiom])");

        collector.clear();
        interactive.process(".explain (b q b) 0");
        CHECK(explained(collector) == R"(:q b
   └─ b p c  [asserted; no derivation found])"); });
}

// The counterpart, which keeps the case above from being purchased via
// counting less: an additional instantiation via the root's own cycle counts
// when its premises hold independently of the root. (:p a) is derived from
// (:r a) by one of its rules, and by the other from (:q a), which rests on an
// asserted fact that a rule could also derive. Once the search encounters the
// return path through (:r a) first, (:p a) becomes part of the root's cycle,
// and whether it holds without the root is settled only upon resolution of the
// cycle. Two variants keep (:p a)'s rules at identical ids and exchange which
// one is cyclic, ensuring that one variant meets it first regardless of the
// hash (see "a premise first reached through a cycle").
TEST_CASE("explain: a second justification through the fact's own cycle still counts")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& cyclic, const std::string& grounded)
        {
            process_lines(interactive, R"(
(:f1 X) => (:p X)
(:f2 X) => (:p X)
(:e1 X) => (:q X)
(:e2 X) => (:q X)
(:k X) => (:e1 X)
(:k X) => (:e2 X)
(:p X) => (:r X)
(:c X) => (:r X)
:c a
(:r X) => (:e1 X)
:e2 a
)");
            interactive.process("(:r X) => (:" + cyclic + " X)");
            interactive.process("(:q X) => (:" + grounded + " X)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (:r a) 0");
            CHECK(explained(collector) == ":r a  [one of several justifications]\n"
                                          "   └─ :c a  [axiom]");
        };
        SUBCASE("f1 cyclic") { check("f1", "f2"); }
        SUBCASE("f2 cyclic") { check("f2", "f1"); } });
}

// A rule's ground condition is present as a node solely due to the rule
// being written; no one claimed it, and the forward pass never matches it.
// The search resolved a condition via lookup and still located that node:
// (:h a) was proved through the rule pattern (:g a), printed "[rule pattern;
// not asserted]", or announced it as a second justification alongside the
// real one, and the same occurred for (:z a) one level further up. On the
// maths stack, it displaced a genuine derivation of (&0 cmp &0) following
// the tutorial-identities example. Two variants keep the competing rules at
// identical ids and swap which of their conditions serves as the pattern,
// ensuring that one variant meets the pattern first regardless of the hash
// (see "a premise first reached through a cycle").
TEST_CASE("explain: a rule's ground condition is not a premise")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& pattern, const std::string& held)
        {
            process_lines(interactive, R"(
(:g X) => (:h X)
(:k X) => (:h X)
(:h X) => (:z X)
)");
            interactive.process("(:" + pattern + " a) => (:m a)");
            interactive.process(":" + held + " a");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".explain (:h a) 0");
            CHECK(explained(collector) == ":h a\n"
                                          "   └─ :" + held + " a  [axiom]");

            collector.clear();
            interactive.process(".explain (:z a) 0");
            CHECK(explained(collector) == ":z a\n"
                                          "   └─ :h a\n"
                                          "      └─ :" + held + " a  [axiom]");
        };
        SUBCASE("g is the pattern") { check("g", "k"); }
        SUBCASE("k is the pattern") { check("k", "g"); } });
}

// The case on the maths stack: polynomial.zph writes the ground
// condition ((pos zint &0) pneg (pos zint &0)), and following the
// tutorial-identities example, the proof for (&0 cmp &0) ran through it
// even though a genuine derivation is present.
TEST_CASE("explain: a rule's ground condition is not a premise on the math stack" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.import math
<x> ~ polyring
? $( x^2 - 1 ) ≡ $( (x-1)*(x+1) )
)");
    REQUIRE(any_output_contains(collector, "= proven"));

    collector.clear();
    interactive.process(".explain (&0 cmp &0) 0");
    CHECK(any_output_contains(collector, "&0 cmp &0"));
    CHECK_FALSE(any_output_contains(collector, "rule pattern"));
}

// The other side of the case above, and where the rule draws its line: (:r
// o) is derived from the asserted (:c o), and also from (:k o), which is
// asserted as well, but which (:r o) derives in turn. Without provenance the
// search cannot tell that (:k o) was asserted rather than derived from the
// root alone, so an instantiation counts as a further justification only
// when its premises are grounded without the root's own component -- and
// this one is not announced.
TEST_CASE("explain: an asserted premise that the root also derives is not a second justification")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(:p1 X) => (:r X)
(:p2 X) => (:r X)
:c o
:k o
(:c X) => (:p1 X)
(:k X) => (:p2 X)
(:r X) => (:k X)
)");
        interactive.run(true, false, false);

        for (const char* depth : {"0", "4"})
        {
            CAPTURE(depth);
            collector.clear();
            interactive.process(std::string(".explain (:r o) ") + depth);
            CHECK(explained(collector) == ":r o\n   └─ :p1 o\n      └─ :c o  [axiom]");
        } });
}

// The root's further justification (:r x) <= (:c x) rests on (:c x), which
// holds in the graph and is not derived from the root: its sole derivation,
// via (:d x), requires (:c x) itself. Whether the annotation said this
// depended on the sequence in which the rules were input. In one order, the
// search resolved (:c x) and (:d x) as a cycle of their own and included
// (:c x) in the count; in the other, it met them while (:r x) remained open,
// resolved all four together, and then no member of the root's group counted
// for the annotation. A member that the root does not derive now counts
// regardless of which group the search assigned it to.
// A further instantiation counts only if its premises hold without involving
// the root. Members of the root's cycle that hold all the same through a
// support not traversing the root count as given -- however, a member reached
// from the root through the instantiations the search recorded does not, no
// matter what else it rests on. Considering as given every member that a
// fixpoint with only the root grounded leaves ungrounded counted (:f c),
// which rests on (:x c) and thus ultimately on the root, together with
// (:p c), which the root does not derive; and in a cycle, that fixpoint
// grounds almost nothing, so every member of a ring counted. Each tree below
// has exactly one justification that does not rest on the root, or none at
// all.
TEST_CASE("explain: a premise the root derives is not a second justification, also beside one it does not")
{
    struct Case
    {
        const char* name;
        const char* program;
        const char* fact;
        const char* tree;
    };
    const Case cases[] = {
        {"a member resting on the root and on one it does not derive", R"(
(:a X) => (:q X)
(:f X) => (:q X)
((:p X), (:x X)) => (:f X)
(:q X) => (:x X)
((:q X), (:p X)) => (:p X)
:a c
:p c
)",
         "(:q c)",
         ":q c\n   └─ :a c  [axiom]"},
        {"a cycle without support from outside", R"(
(:f X) => (:q X)
((:f X), (:p X)) => (:q X)
((:p X), (:x X)) => (:f X)
(:q X) => (:x X)
((:q X), (:p X)) => (:p X)
:q c
:p c
)",
         "(:q c)",
         ":q c\n   └─ :f c  [asserted; no derivation found]"},
        {"a premise derived from the root and an asserted fact", R"(
(:a X) => (:r X)
(:m X) => (:r X)
((:r X), (:s X)) => (:m X)
(:m X) => (:s X)
:a n
:s n
)",
         "(:r n)",
         ":r n\n   └─ :a n  [axiom]"},
        {"a premise derived from the root and an input fact", R"(
(:g X, :h X) => (:f X)
(:k X) => (:g X)
(:f X) => (:g X)
(:f X) => (:h X)
:k a
:h a
)",
         "(:g a)",
         ":g a\n   └─ :k a  [axiom]"},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.name);
        for (const char* depth : {"0", "3", "4"})
        {
            CAPTURE(depth);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, c.program);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(std::string(".explain ") + c.fact + " " + depth);
            CHECK(explained(collector) == c.tree);
        }
    }

    // A ring governed by transitivity: each additional instantiation of
    // (a0 sub a1) needs (a0 sub Y), and the sole outgoing edge from a0 is
    // the root.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, "(X sub Y, Y sub Z) => (X sub Z)\na0 sub a1\na1 sub a2\na2 sub a3\na3 sub a4\na4 sub a0\n");
    interactive.run(true, false, false);
    collector.clear();
    interactive.process(".explain (a0 sub a1) 0");
    const std::string tree = explained(collector);
    CHECK(tree.rfind("a0 sub a1\n", 0) == 0);
    CHECK(tree.find("one of several") == std::string::npos);
}

// Whether a member of the root's component holds in the absence of the
// root was decided by asking whether any instantiation recorded for it
// connects to the root -- even one that depends on the member in question:
// (:p0 a) <= (:p0 a), (:p2 a) does not support (:p0 a), regardless of what
// (:p2 a) rests on. The annotation was lost when (:p0 a) represents a
// second path to the root. An instantiation that depends on the member
// itself is now excluded from that evaluation. In addition, the shape the
// help's phrasing must encompass: a premise the root derives counts if the
// search grounded it independently of the root, as (:p3 a) <= (:p0 a) is.
TEST_CASE("explain: an instantiation that needs the member itself does not tie it to the root")
{
    SUBCASE("the member's only instantiation contains it")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(:p0 X) => (:p2 X)
((:p0 X), (:p2 X)) => (:p0 X)
(:p1 X) => (:p2 X)
:p0 a
:p1 a
)");
        interactive.run(true, false, false);
        for (const char* depth : {"0", "3"})
        {
            CAPTURE(depth);
            collector.clear();
            interactive.process(std::string(".explain (:p2 a) ") + depth);
            CHECK(explained(collector) == ":p2 a  [one of several justifications]\n"
                                          "   └─ :p1 a  [axiom]");
        }
    }
    SUBCASE("a premise the root derives, grounded without it")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(:p1 X) => (:p3 X)
(:p0 X) => (:p3 X)
(:p3 X) => (:p2 X)
(:p2 X) => (:p1 X)
(:p3 X) => (:p1 X)
:p0 a
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (:p1 a) 0");
        CHECK(explained(collector) == ":p1 a  [one of several justifications]\n"
                                      "   └─ :p2 a\n"
                                      "      └─ :p3 a\n"
                                      "         └─ :p0 a  [axiom]");
    }
}

TEST_CASE("explain: a second justification counts whichever order the rules were entered in")
{
    const std::vector<std::string> rules{"(:r X) => (:a X)", "(:c X) => (:r X)", "(:d X) => (:c X)", "(:a X) => (:r X)", "(:b X) => (:a X)", "((:c X), (:a X)) => (:d X)"};
    const auto                     check = [&](const std::vector<std::size_t>& order)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        for (const std::size_t i : order)
            interactive.process(rules[i]);
        process_lines(interactive, ":b x\n:c x\n");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (:r x) 0");
        CHECK(explained(collector) == ":r x  [one of several justifications]\n   └─ :a x\n      └─ :b x  [axiom]");
    };
    SUBCASE("resolved with the root") { check({0, 1, 2, 3, 4, 5}); }
    SUBCASE("resolved on its own") { check({0, 1, 4, 3, 5, 2}); }
}

// The counterpart: one derivation must not grow the annotation, or it says
// nothing. The premise here is reachable by exactly one rule.
TEST_CASE("explain: a fact reached one way is not annotated")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X target Y)
a p b
)");
        interactive.run(false, false, false);

        collector.clear();
        interactive.process(".explain (a target b) 0");
        CHECK(any_output_contains(collector, "a p b"));
        CHECK_FALSE(any_output_contains(collector, "one of several justifications")); });
}

// Only the ROOT is scanned for a second justification, and that is a cost
// decision: looking at every level would turn a linear walk into a quadratic
// one. It is invisible in the two tests above, because there the fact reached
// twice IS the root -- so this is the case that pins it, and it is the one a
// later change would break silently.
//
// The same fact, in both positions, on one graph. `a mid b` is reached through
// p and through q, so as the root of its own tree it says so. Standing as the
// premise of `a target b` -- whose own derivation is unique -- it carries no
// annotation, because nothing looked. Asking both of ONE graph is what makes
// this a statement about the position rather than about the fact: a change that
// starts annotating deeper nodes makes the second half fail, and a change that
// stops annotating at all makes the first half fail.
TEST_CASE("explain: the annotation is the root's, not the premise's")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X mid Y)
(X q Y) => (X mid Y)
(X mid Y) => (X target Y)
a p b
a q b
)");
        interactive.run(false, false, false);

        collector.clear();
        interactive.process(".explain (a mid b) 0");
        REQUIRE(any_output_contains(collector, "one of several justifications"));

        collector.clear();
        interactive.process(".explain (a target b) 0");
        // The premise is in the tree and expanded -- so the annotation's
        // absence is about where it stands, not about the tree stopping short.
        CHECK(any_output_contains(collector, "a mid b"));
        CHECK(any_output_contains(collector, "[axiom]"));
        CHECK_FALSE(any_output_contains(collector, "one of several justifications")); });
}

// A trailing numeral is read as the depth first. If the rest denotes a
// fact that does not exist within the graph, yet the entire argument
// refers to a fact that does, the numeral is the fact's own
// object: ".explain y val 7 3" answered "Fact is not asserted" regarding
// the asserted (y val 7 3). A depth exceeding the capacity of a number
// used to abort the line with a bare "stoul".
TEST_CASE("explain: a trailing numeral is the object when only that reading names a fact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X val Y) => (X hasval Y)
x val 7
y val 7 3
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain y val 7 3");
        CHECK(explained(collector) == "y val 7 3  [axiom]");

        // The reading that is documented continues to prevail
        // whenever it names a fact.
        collector.clear();
        interactive.process(".explain x hasval 7 0");
        CHECK(explained(collector) == "x hasval 7\n   └─ x val 7  [axiom]");

        std::string error;
        try
        {
            interactive.process(".explain x hasval 7 99999999999999999999999");
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        CHECK(error.find("max-depth") != std::string::npos);
        CHECK(error.find("stoul") == std::string::npos); });
}

// The form without an argument explains the last node printed, and a tree
// prints multiple nodes: following ".explain 3", ".explain 0" explained
// the tree's last line -- an axiom -- rather than the answer, as
// math/index.md tells the reader to expect. A tree is not an answer, and
// it leaves the node to explain as it originally stood.
TEST_CASE("explain: a tree does not change what the argument-less form explains")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, ".import binary-arithmetic\n? (&6 + &7)");

    collector.clear();
    interactive.process(".explain 1");
    REQUIRE(explained(collector).rfind("(&6 + &7) = &13\n", 0) == 0);

    collector.clear();
    interactive.process(".explain 0");
    CHECK(explained(collector).rfind("(&6 + &7) = &13\n", 0) == 0);
}

// A rule featuring a conjunction, named by its text, resolved during
// certain invocations but not during others: ".explain" returned results
// three times and subsequently declared "Fact is not asserted", while
// ".node" found it once and then encountered failure. The text must
// denote the identical node each time it is read.
TEST_CASE("explain: a rule written as text denotes the same rule on every call")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R is partialorder) => ((X R Y, Y R X) => (X sameas Y))
divides is partialorder
)");
        interactive.run(true, false, false);

        for (int i = 0; i < 6; ++i)
        {
            CAPTURE(i);
            collector.clear();
            interactive.process(".explain (((X divides Y), (Y divides X)) => (X sameas Y))");
            CHECK(any_output_contains(collector, "divides is partialorder  [axiom]"));
            CHECK_FALSE(any_output_contains(collector, "not asserted"));
        }
        for (int i = 0; i < 3; ++i)
        {
            CAPTURE(i);
            collector.clear();
            CHECK_NOTHROW(interactive.process(".node (((X divides Y), (Y divides X)) => (X sameas Y))"));
        } });
}

TEST_CASE("explain: a rejected reading of the argument stays silent")
{
    // cmd_explain TRIES readings of its argument; a failing one is a
    // normal outcome. janet_dostring prints its stack trace before it
    // returns the error status, so the speculative evaluation has to
    // suppress Janet's own reporting -- otherwise a successful .explain
    // is preceded by an "arity mismatch" trace that looks like a crash.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A gate B) => ((A gate B) out 0)");
        interactive.process("1 gate 1");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain ((1 gate 1) out 0)");
        CHECK(any_output_contains(collector, "[axiom]"));
        CHECK_FALSE(any_output_contains(collector, "arity mismatch")); });
}

TEST_CASE("explain: a transient variable does not take a name lookup with it")
{
    // .explain evaluates its pattern inside a scratch cluster and drops it
    // again, so the variables it builds are removed. They are NAMED while
    // they live, though, and the name map that answers ".node A" used to be
    // handed to the newest owner -- which was the transient one. Dropping it
    // erased the entry, so a read-only command turned a working lookup into
    // "No node found with name 'A'" while every rule went on displaying A,
    // and the two name maps disagreed from then on, in the session and in a
    // .bin saved from it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("(A p B) => (A q B)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".node A");
        CHECK(any_output_contains(collector, "Variable: yes"));
        const std::string before = last_out_text(collector);

        collector.clear();
        interactive.process(".explain (A p B)");

        collector.clear();
        interactive.process(".node A");
        CHECK(any_output_contains(collector, "Variable: yes"));
        CHECK(last_out_text(collector) == before);

        // The control for the branch next to it: a REAL node keeps its name
        // against a variable of the same letter, which is what a single-letter
        // Wikidata label depends on.
        interactive.process(".name a en \"B\"");
        interactive.process(".lang en");
        collector.clear();
        interactive.process("B p C");
        collector.clear();
        interactive.process(".node B");
        CHECK(any_output_contains(collector, "Variable: no"));
        interactive.process(".lang zelph"); });
}

TEST_CASE("help: an alias is documented under its canonical command")
{
    // One table drives both the dispatch registration and ".help <alias>".
    // While ".why" was registered separately, it was a working command
    // that ".help .why" claimed not to know.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process(".help .why");
        CHECK(any_output_contains(collector, "alias: .why"));
        CHECK_FALSE(any_output_contains(collector, "Unknown command")); });
}

// The renderer and its documentation gradually diverge without notice: the
// help listed three of the labels a tree can contain, and it asserted that
// each derived fact keeps an instantiation that holds -- though a pruned
// premise or a negation defeated later breaks this, resulting in
// "[asserted; no derivation found]" appearing on a derived fact, a label
// the help never explained.
TEST_CASE("help: every label .explain prints is explained in .help .explain")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".help .explain");
    for (const char* label : {"[axiom]", "[axiom; negated by a rule]", "[asserted; no derivation found]", "[rule pattern; not asserted]", "[rule mentioned; not in force]", "[closure]", "[absent]", "[depth limit", "[see above]", "[one of several justifications]"})
    {
        CAPTURE(label);
        CHECK(any_event_contains(collector, label));
    }
    CHECK_FALSE(any_event_contains(collector, "full provenance"));
}

TEST_CASE("help: a topic may be named with or without its dot")
{
    // The listing prints every command with its dot, so that is what gets
    // pasted -- but the bare name is at least as natural to type, and
    // ".help deductions" answered "Unknown command: deductions" about a
    // command that not only exists but is one of the most used.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process(".help deductions");
        CHECK(any_output_contains(collector, "Sets the deduction printing mode"));
        CHECK_FALSE(any_output_contains(collector, "Unknown command"));

        // Aliases resolve bare too, and an unknown topic is still an error.
        collector.clear();
        interactive.process(".help why");
        CHECK(any_output_contains(collector, "alias: .why"));

        collector.clear();
        // The refusal goes to the error channel, not to Out.
        interactive.process(".help nosuchthing");
        CHECK(any_event_contains(collector, "Unknown command")); });
}

TEST_CASE("explain: an argument that names a node is not a parse failure")
{
    // One message covered four different situations, and the one it named --
    // a parse failure -- was the only one it usually was NOT. It matters most
    // where it is most natural to type: the engine reports a contradiction
    // with its premises on the "⇐" line, and ".explain !" answered that the
    // argument might not parse. It parses; a contradiction is simply not a
    // fact, and it materializes nothing, so there is nothing left to
    // reconstruct afterwards.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto message_of = [&interactive](const char* line)
        {
            try
            {
                interactive.process(line);
            }
            catch (const std::exception& ex)
            {
                return std::string(ex.what());
            }
            return std::string{};
        };

        interactive.process("(X p Y) => !");
        interactive.process("a p b");
        interactive.run(true, false, false);

        CHECK(message_of(".explain !").find("contradiction marker") != std::string::npos);
        CHECK(message_of(".explain a").find("is a node, not a fact") != std::string::npos);
        CHECK(message_of(".explain ~").find("is a node, not a fact") != std::string::npos);

        // A name that denotes nothing keeps the message that fits it.
        CHECK(message_of(".explain nosuchnode").find("cannot parse fact pattern") != std::string::npos);

        // And the working case is untouched.
        collector.clear();
        interactive.process(".explain a p b");
        CHECK(any_output_contains(collector, "[axiom]")); });
}
