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

#include "test_helpers.hpp"

#include "io/output.hpp"

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
    CHECK(misses <= ceiling(424622));
    CHECK(full_clears <= ceiling(1105));

    // Volume of derivation and of candidate scanning: a different regression
    // class from the above (doing more work rather than paying more for it),
    // and the one a rule change shows up in first.
    CHECK(deduce_calls <= ceiling(39869));
    CHECK(facts_created <= ceiling(15406));
    CHECK(scanned_seq + scanned_par <= ceiling(727256 + 94054));
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
