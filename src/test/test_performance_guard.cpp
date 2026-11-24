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

// A performance guard that costs no wall-clock measurement.
//
// WHY THIS EXISTS. On 1 August 2026 a correctness fix quietly tripled the
// runtime of the whole math stack: revoking a rule-pattern mark deleted the
// marking fact through the general removal path, which DISARMS the
// genuine-structure store for the rest of the session, after which every
// fs_cache miss paid a full adjacency reconstruction. Nothing failed, every
// answer stayed correct, and the interleaved A/B timings that each batch ran
// could not see it -- they compare two binaries against each other and are
// blind to the baseline itself moving. It took two days and a bisect to find.
//
// The counters below would have caught it the moment it was committed:
// `genuine walks` went from 0 to 667540 and `fs_cache misses` rose by half.
// They count WORK, so they are hardware-independent -- no quiet machine, no
// CPU pinning, no coordination with other sessions.
//
// WHAT IT CANNOT SEE. Cost per operation. A new lock on a hot path, a
// cache-hostile data structure, a more expensive test per candidate: all of
// those leave every counter below untouched while the workload gets slower.
// (During the same investigation a lock-contention hypothesis was written,
// measured and discarded for exactly that reason.) For a change that
// plausibly alters the cost of an operation rather than the number of them,
// there is no substitute for a timed A/B run on an idle machine.
//
// WHEN IT GOES RED. Read the printed value first. A change that legitimately
// alters how much work the math stack does -- a new stdlib rule, a different
// evaluation order -- moves these numbers, and then the baseline constants
// here are what needs updating, in one line each. A change that moves them by
// a factor is a regression until proven otherwise.
//
// On 30 September 2026, the baselines were re-taken for version 1.1.0,
// involving three runs, all of which were bit-identical except for fs_cache
// misses (spread of ±10 out of 428k).
//
// On 10 October 2026, they were re-taken after polynomial.zph gained
// the rule that reports a pouter cycle: this rule derives nothing in
// this context, yet a new rule shifts node numbering, thereby altering
// the order of evaluation (deduce_calls 39866 to 39869, fs_cache misses
// falling from about 428k to 425k). Three runs, bit-identical except
// for fs_cache misses (spread of 1 out of 425k).
//
// On 5 November 2026, they were re-taken after a condition in which the
// relation, subject, and objects were all bound came to be answered by a
// single exact probe rather than a walk across the subject's adjacency, and
// a condition bound at both ends came to be read at the smaller endpoint.
// The derivation remained unchanged (deduce_calls 39869, facts_created
// 15406); the number of candidates attempted dropped from 726870 + 72709 to
// 650147 + 72709, and fs_cache misses decreased from approximately 416.9k
// to 388.2k. The dump gained the `anchors:` line, and two ceilings read it:
// the adjacency entries the anchors walked, 2876857, and the entries the
// partial anchors' climbs read, 1678159. The walk represents the cost that
// scanned(seq) cannot account for, as it only tracks the candidates a walk
// kept: without the probe, the anchors walked 8.13M entries in this
// workload, as measured by an instrumented build of the engine before the
// change, and in the stdlib's comparisons the walk grew with each number in
// the graph while scanned(seq) remained constant. Sixteen runs were
// conducted, thirteen at the prompt and three of this case: fs_cache misses
// ranged from 388178 to 388234, the climb from 1678159 to 1678163, the walk
// from 2876857 to 2876859, the candidates scanned spread by 1, and all else
// remained bit-identical.

#include "directory_rewrite.hpp"
#include "test_helpers.hpp"
#include "wikidata_dump.hpp"

#include "io/output.hpp"
#include "network/reasoning.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

using namespace zelph::test;

namespace
{
    // .prof emits its whole block as ONE event and on the DIAGNOSTIC channel,
    // not on Out -- which is why test_profiler.cpp reaches it with
    // any_event_contains rather than any_output_contains. Every channel is
    // collected here for the same reason.
    std::string all_event_text(const zelph::io::OutputCollector& collector)
    {
        std::string all;
        for (const auto& event : collector.events())
        {
            all += event.text;
            all += '\n';
        }
        return all;
    }

    // One counter out of the .prof dump. `section` disambiguates the keys that
    // occur more than once -- `hits=` belongs to both fs_cache and genuine.
    uint64_t prof_counter(const std::string& text, const std::string& section, const std::string& key)
    {
        const size_t start = text.find(section);
        if (start == std::string::npos)
            throw std::runtime_error("performance guard: .prof section '" + section + "' not found");

        const size_t at = text.find(key + "=", start);
        if (at == std::string::npos)
            throw std::runtime_error("performance guard: .prof counter '" + key + "' not found");

        return std::stoull(text.substr(at + key.size() + 1));
    }

    // One block from the .prof dump: the counters appearing on the line
    // that `label` opens, as the dump prints them. When examined as a
    // whole, the text shows a counter added to the block, which a lookup by
    // name cannot detect.
    std::string prof_block(const std::string& text, const std::string& label)
    {
        const std::string opening = "\n  " + label + " ";
        const size_t      start   = text.find(opening);
        if (start == std::string::npos)
            throw std::runtime_error("performance guard: .prof block '" + label + "' not found");

        const size_t from = start + opening.size();
        return text.substr(from, text.find('\n', from) - from);
    }

    // Stefan's choice of 3 August 2026: ten percent of headroom. Tight enough
    // that every cliff trips it, loose enough that ordinary work on the rules
    // does not.
    constexpr uint64_t ceiling(const uint64_t baseline)
    {
        return baseline + baseline / 10;
    }
} // namespace

TEST_CASE("performance guard: the Jacobian workload does the expected amount of work")
{
    // A plain session in the DEFAULT mode, which this binary is required
    // to request: every engine it initializes starts in
    // `.semi-naive check`, and check mode appends classic verification
    // passes after the delta fixpoint. These about double the count of
    // deduce_calls and increase the scan volume by half -- sufficient to
    // break the ceilings of both -- while facts_created, the fs_cache
    // counters, and the genuine walks remain unchanged. The baselines
    // originate from a single ordinary parallel run.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".semi-naive on");
    interactive.process(".log -1");         // counter-only mode: no per-deduction output
    interactive.process(".deductions off"); // the echo is thousands of lines and costs time
    interactive.process(".import examples/math/jacobian");

    collector.clear();
    interactive.process(".prof");
    const std::string prof = all_event_text(collector);

    const uint64_t deduce_calls  = prof_counter(prof, "rules_applied", "deduce_calls");
    const uint64_t facts_created = prof_counter(prof, "rules_applied", "facts_created");
    const uint64_t scanned_seq   = prof_counter(prof, "unification:", "scanned(seq)");
    const uint64_t scanned_par   = prof_counter(prof, "unification:", "scanned(par)");
    const uint64_t misses        = prof_counter(prof, "fs_cache:", "misses");
    const uint64_t full_clears   = prof_counter(prof, "fs_cache:", "full_clears");
    const uint64_t walks         = prof_counter(prof, "genuine:", "walks");
    const uint64_t unmarked      = prof_counter(prof, "rule_patterns:", "unmarked");
    const uint64_t walked        = prof_counter(prof, "anchors:", "walked");
    const uint64_t climbed       = prof_counter(prof, "anchors:", "climbed");

    // Non-vacuity: a failed import would leave every ceiling below satisfied.
    // Deliberately far under the baseline -- this is not a second ceiling.
    CHECK(deduce_calls > 30000);

    // THE one that mattered. Every fact the math stack creates goes through
    // triple-level construction, so the genuine store answers every
    // reconstruction request and the walk never runs. A single removal on the
    // wholesale invalidation path disarms the store and this becomes six
    // figures. It is exact rather than a ceiling because it is a property, not
    // a quantity: either the store is authoritative or it is not.
    CHECK(walks == 0);

    // The marks a claim revokes. A revocation removes the marking fact, and
    // eliminating it along the general path is what disarmed the store
    // above, so each one presents an opportunity for the cliff to reappear.
    // Exact rather than a ceiling: the import claims the same stdlib
    // patterns on every run, and a change to what a statement claims in a
    // nested position becomes visible here before becoming visible
    // elsewhere. Measured at 15 on the tree before opaque predicates were
    // introduced, in both sequential and parallel runs.
    CHECK(unmarked == 15);

    // Requests for reconstruction that could not be fulfilled by the
    // fs_cache. Rose from 455644, the August baseline, to 678673 when the
    // store went, so this is the second net under the same failure -- and it
    // also catches an invalidation storm that leaves the store armed.
    CHECK(misses <= ceiling(388183));
    CHECK(full_clears <= ceiling(1105));

    // Volume of derivation and of candidate scanning: a different regression
    // class from the above (doing more work rather than paying more for it),
    // and the one a rule change shows up in first.
    CHECK(deduce_calls <= ceiling(39869));
    CHECK(facts_created <= ceiling(15406));
    CHECK(scanned_seq + scanned_par <= ceiling(650147 + 72709));

    // The cost of reading the candidates before any was tried, which the
    // scan volume above does not account for: a condition anchored at a
    // node reads its whole adjacency, and the climb from a partial anchor
    // reads the adjacencies encountered along its ascent. A condition bound
    // throughout that is read from its subject once more instead of via the
    // exact probe raises the first to approximately 8.1M.
    CHECK(walked <= ceiling(2876857));
    CHECK(climbed <= ceiling(1678159));
}

// The blind spot of the prior scenario formed a case of its own. Its workload
// drops no cluster, and a cluster drop disarmed the fact-path stores
// permanently: a dry run within a cluster, dropped before a maths request,
// caused `genuine walks` to rise from 0 to 2618 on a single identity over a
// polynomial ring, and nothing above went red. A drop of a cluster with no
// external references now removes its nodes without disarming them.
TEST_CASE("performance guard: a dropped cluster leaves the math stack's stores armed" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".semi-naive on"); // as previously stated: the classic passes in check mode do not constitute the workload
    interactive.process(".log -1");
    interactive.process(".deductions off");
    interactive.process(".import math");

    interactive.process(".cluster dry");
    interactive.process("c likes d");
    interactive.process(".cluster default");
    interactive.process(".cluster-drop dry");

    interactive.process("<x y> ~ polyring");
    interactive.process("(x + y) ≡ (y + x)");

    collector.clear();
    interactive.process(".prof");
    const std::string prof = all_event_text(collector);

    // Non-vacuity: the request managed to arrive at the stores.
    CHECK(prof_counter(prof, "genuine:", "hits") > 0);
    CHECK(prof_counter(prof, "genuine:", "walks") == 0);
}

// The first case holds the volume of the Jacobian workload to ceilings, with a
// tenth of headroom. The counters named in
// `mkdocs/docs/internals/measurement.md` as hard semantic invariants say what
// the example derives and which decisions the engine takes on the way, and the
// page requires them to stay bit-identical across any change that does not
// alter semantics. Among these, only `facts_created` and `deduce_calls` have a
// ceiling in the first case, and such a ceiling permits a downward move to
// pass, as well as any upward move by less than a tenth. When a statement that
// writes a rule stopped being evaluated as a query too, `evaluate calls` and
// `leaf_conditions` each dropped by 426, one for each rule with a variable
// among the 430 that the import types, and no case went red: the page went on
// naming the old value. This case holds every counter in the set to the value
// from the standalone sequential run described by the page, ensuring that any
// such move fails the suite. With `.parallel` off, the counters repeat to the
// digit across runs, and they do not depend on the directory in which the tree
// is built. The default configuration does not spread a scan of this example
// either, but a spread scan moves `template_rejects`, and the sequential run
// keeps the values independent of the launch threshold. When the case goes
// red, a change intended to alter what is derived or decided brings the new
// values into this case and into the page together, accompanied by the
// mechanism explaining them; for any other change, the red case serves as a
// finding. The page names the values within a dedicated block, and the ctest
// check `measurement_page_names_the_values_the_guard_holds` fails when that
// block and this case diverge. Taken on 24 November 2026: three runs at the
// prompt, this case alone and within the suite, and a tree built at a
// different path yielded identical values.
TEST_CASE("performance guard: the sequential Jacobian workload keeps every hard semantic invariant at its value" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".parallel"); // a switch, activated by default
    REQUIRE_FALSE(interactive.graph()->use_parallel());
    interactive.process(".semi-naive on"); // as in the first case: the classic passes in check mode do not constitute the workload
    interactive.process(".log -1");
    interactive.process(".deductions off");
    interactive.process(".import examples/math/jacobian");

    collector.clear();
    interactive.process(".prof");
    const std::string prof = all_event_text(collector);

    CHECK(prof_counter(prof, "rules_applied", "facts_created") == 15406);
    CHECK(prof_counter(prof, "rules_applied", "deduce_calls") == 39869);
    CHECK(prof_counter(prof, "rules_applied", "seminaive_seeds") == 352931);
    CHECK(prof_counter(prof, "rules_applied", "seminaive_safety_extra") == 0);
    CHECK(prof_counter(prof, "extract:", "ok") == 165298);
    CHECK(prof_counter(prof, "extract:", "template_rejects") == 8532);

    // The page takes these four blocks into the set whole, so any counter added
    // to one of them becomes part of the set. Each block is held as the text
    // the dump prints, and a new counter fails the case until the case also
    // holds its value.
    CHECK(prof_block(prof, "evaluate:") == "calls=135611 leaf_conditions=123425 conjunction_sets=520 optimize_order=58291");
    CHECK(prof_block(prof, "negation:") == "cond=1379 ok=460 fail=919");
    CHECK(prof_block(prof, "check_fact:") == "known=24463 new=15406 wrong=0");
    CHECK(prof_block(prof, "termination_guard:") == "checks=0 skips=0 fresh_vars=0 fresh_nodes=0");

    // The exact identities the page checks on each protocol, using the same
    // dump. Every fs_cache miss is answered by either a genuine hit or a walk,
    // and the workload performs no reads on any node that was written by a
    // load, a trusted import, or a name merge, thus keeping the three walk
    // counters at zero: a walk in this context indicates the disarm funnel
    // fired during the run, and the page stops the measurement until the
    // trigger is understood. Among these, the first case holds `genuine walks`
    // alone. None of them constitutes a value within the set, and they are
    // compared as named counters, since the ctest check reads every comparison
    // between a counter or a block and a literal as one.
    const uint64_t misses         = prof_counter(prof, "fs_cache:", "misses");
    const uint64_t genuine_hits   = prof_counter(prof, "genuine:", "hits");
    const uint64_t genuine_walks  = prof_counter(prof, "genuine:", "walks");
    const uint64_t walk_fallbacks = prof_counter(prof, "var_closure:", "walk_fallbacks");
    const uint64_t template_walks = prof_counter(prof, "template_vars:", "walks");
    CHECK(misses > 0); // non-vacuity: without a miss, the identity would provide no information regarding what responds to one
    CHECK(misses == genuine_hits + genuine_walks);
    CHECK(genuine_walks == 0);
    CHECK(walk_fallbacks == 0);
    CHECK(template_walks == 0);
}

// The first case in this file runs a session that holds no fact with several
// objects, and it cannot observe the effect of any single such fact. While
// the possibility of one existing was represented by a single bit for the
// whole session, `zz p a b`, whose predicate is not held by any term of the
// math stack, caused every statement within every condition to shift to a
// reading based on its atoms, leaving no condition for the exact probe:
// before the workload, it made the parent of the record scan 14498533 +
// 5857845 candidates and hit the fs_cache 116.8M times, and a sequential run
// of the example execute 124 G instructions instead of 10.5 G. The record
// marks p exclusively, and deduce_calls, the candidates scanned, the
// adjacency walked, and the climb performed are those associated with
// `zz p a` and `zz p b`, which move the first case's through the same four
// atoms. The fact incurs the cost of the walk that asks whether a statement
// holds a marked predicate (may_match_wider_statement): its structure reads
// cause fs_cache hits to rise from 2.75M to 4.73M and misses from 390k to
// 489k, held by the last two ceilings; what each read costs, they cannot
// discern. Recorded on 5 November 2026 across six runs: walked, scanned, and
// the climb spread by 1, fs_cache misses and hits by 40, everything else
// bit-identical.
TEST_CASE("performance guard: one fact with two objects leaves the Jacobian workload's reading of its conditions as it was" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".semi-naive on"); // as in the first case: the classic passes in check mode do not constitute the workload
    interactive.process(".log -1");
    interactive.process(".deductions off");
    interactive.process("zz p a b");
    REQUIRE(interactive.graph()->wider_statements().predicates == 1);
    interactive.process(".import examples/math/jacobian");

    collector.clear();
    interactive.process(".prof");
    const std::string prof = all_event_text(collector);

    const uint64_t deduce_calls = prof_counter(prof, "rules_applied", "deduce_calls");
    CHECK(deduce_calls > 30000); // non-vacuity, as in the first case
    CHECK(prof_counter(prof, "genuine:", "walks") == 0);
    CHECK(deduce_calls <= ceiling(39879));
    CHECK(prof_counter(prof, "rules_applied", "facts_created") <= ceiling(15406));
    CHECK(prof_counter(prof, "unification:", "scanned(seq)") + prof_counter(prof, "unification:", "scanned(par)") <= ceiling(649573 + 72728));
    CHECK(prof_counter(prof, "anchors:", "walked") <= ceiling(2857172));
    CHECK(prof_counter(prof, "anchors:", "climbed") <= ceiling(1674922));
    CHECK(prof_counter(prof, "fs_cache:", "hits") <= ceiling(4727086));
    CHECK(prof_counter(prof, "fs_cache:", "misses") <= ceiling(489217));
}

// The scenarios above begin with an empty session, and thus cannot see what
// a session does BEFORE the workload. Every .load, every trusted import, and
// every name merge disarmed the fact-path stores for the rest of the
// session: the Jacobian workload following a .load of a two-fact network ran
// 27.2 G instead of 7.4 G user instructions sequentially (genuine walks 0 to
// 571887, template-variable walks 0 to 1796411), and after a merge of two
// unrelated atoms, 31.7 G instead of 9.0 G. The stores remain armed
// throughout these paths, and what the workload writes is answered by them;
// only the nodes those paths wrote are read by the walks, and the workload
// touches none. A file lacking the record of the variable store leaves the
// loaded nodes to a per-node test, which is where the walk would show if
// that test failed for the nodes the workload writes. That file holds a
// rule, as a network lacking a variable loads with the store complete
// regardless of the file's records; the walk reads the loaded rule's own
// nodes then, a handful of times, however long the workload is (7 genuine
// walks and 3 for variables at the time this case was written, on
// 2026-11-21).
TEST_CASE("performance guard: a load, an import or a name merge before the Jacobian workload leaves its stores answering" * doctest::test_suite("slow"))
{
    namespace fs     = std::filesystem;
    const auto path  = fs::temp_directory_path() / "zelph_guard_preloaded.bin";
    const auto ruled = fs::temp_directory_path() / "zelph_guard_preloaded_ruled.bin";
    const auto older = fs::temp_directory_path() / "zelph_guard_preloaded_older.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("zzx1 zzp zzy1");
        interactive.process("zzx2 zzq zzy2");
        interactive.process(".save \"" + path.string() + "\"");
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("zzx1 zzp zzy1");
        interactive.process("zzx2 zzq zzy2");
        interactive.process("(A zzp B) => (A zzr B)");
        interactive.process(".save \"" + ruled.string() + "\"");
    }
    write_without_directory(ruled, older, true);
    const auto dump = zelph::test::wikidata_dump::write_lines(
        "zelph_guard_preloaded",
        {zelph::test::wikidata_dump::entity("Q1", zelph::test::wikidata_dump::text("en", "one"), zelph::test::wikidata_dump::claim("P279", "Q2")),
         zelph::test::wikidata_dump::entity("Q2", zelph::test::wikidata_dump::text("en", "two"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".semi-naive on");
    interactive.process(".log -1");
    interactive.process(".deductions off");
    bool untracked = false;
    SUBCASE("a .load of a file with the record of the variable store")
    {
        interactive.process(".load \"" + path.string() + "\"");
    }
    SUBCASE("a .load of a file without it")
    {
        interactive.process(".load \"" + older.string() + "\"");
        untracked = true;
    }
    SUBCASE("a .load of a dump, through the trusted import")
    {
        interactive.process(".load \"" + dump.string() + "\"");
    }
    SUBCASE("a name merge")
    {
        interactive.process("zzx1 zzp zzy1");
        interactive.process("zzx2 zzq zzy2");
        interactive.process(".name zzx2 zelph zzx1");
    }
    fs::remove(path);
    fs::remove(ruled);
    fs::remove(older);
    zelph::test::wikidata_dump::remove_dump(dump);
    REQUIRE(interactive.graph()->fact_stores_enabled());
    REQUIRE(interactive.graph()->may_hold_untracked_nodes() == untracked);

    interactive.process(".import examples/math/jacobian");

    collector.clear();
    interactive.process(".prof");
    const std::string prof = all_event_text(collector);

    const uint64_t of_the_loaded_rule = untracked ? 16 : 0;
    CHECK(prof_counter(prof, "rules_applied", "deduce_calls") > 15000); // non-vacuity
    CHECK(prof_counter(prof, "genuine:", "walks") <= of_the_loaded_rule);
    CHECK(prof_counter(prof, "template_vars:", "walks") <= of_the_loaded_rule);
    CHECK(prof_counter(prof, "var_closure:", "walk_fallbacks") <= of_the_loaded_rule);
}
