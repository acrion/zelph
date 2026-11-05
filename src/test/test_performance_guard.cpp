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

#include "test_helpers.hpp"

#include "io/output.hpp"
#include "network/reasoning.hpp"

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
