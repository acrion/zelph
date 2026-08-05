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

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Raw weighted edges (synapse substrate)
// ---------------------------------------------------------------------------

TEST_CASE("neural: raw weighted edges are readable and writable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
%(zelph/nn-connect "a" "b" 0.5)
)");
        collector.clear();
        interactive.process(R"(%(zelph/weight "a" "b"))");
        CHECK(any_output_contains(collector, "0.5"));

        interactive.process(R"(%(zelph/set-weight "a" "b" -2.25))");
        collector.clear();
        interactive.process(R"(%(zelph/weight "a" "b"))");
        CHECK(any_output_contains(collector, "-2.25"));

        // Non-existing edge must yield nil.
        collector.clear();
        interactive.process(R"(%(if (nil? (zelph/weight "a" "nowhere")) "no-edge" "edge"))");
        CHECK(any_output_contains(collector, "no-edge")); });
}

TEST_CASE("neural: raw edges are invisible to the reasoning engine")
{
    // A synapse between two nodes must not be picked up as a fact.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
n1 ~ neuron
n2 ~ neuron
%(zelph/nn-connect "n1" "n2" 0.7)
n1 _P _O
)");
        // The only answer for n1 must be the IsA fact, never the raw edge.
        CHECK(answers_contain(collector, "n1 ~ neuron"));
        CHECK_FALSE(any_output_contains(collector, "n1 ?? n2")); });
}

// ---------------------------------------------------------------------------
// Forward pass on a hand-crafted XOR network
// ---------------------------------------------------------------------------

TEST_CASE("neural: forward pass on hand-crafted XOR network")
{
    // Bias-free XOR solution with ReLU:
    //   h1 = relu(x1 - x2), h2 = relu(x2 - x1), y = h1 + h2
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x1 in Lin
x2 in Lin
h1 in Lhid
h2 in Lhid
y in Lout
%(zelph/nn-connect "x1" "h1" 1)
%(zelph/nn-connect "x2" "h1" -1)
%(zelph/nn-connect "x1" "h2" -1)
%(zelph/nn-connect "x2" "h2" 1)
%(zelph/nn-connect "h1" "y" 1)
%(zelph/nn-connect "h2" "y" 1)
%(def net (zelph/nn-compile [(zelph/resolve "Lin") (zelph/resolve "Lhid") (zelph/resolve "Lout")]))
)");
        // Input order is deterministic (ascending node id = creation order).
        collector.clear();
        interactive.process(R"(%(string/join (map (fn [n] (zelph/name n)) (zelph/nn-nodes net 0)) ","))");
        CHECK(any_output_contains(collector, "x1,x2"));

        collector.clear();
        interactive.process(R"(%(string (get (zelph/nn-eval net [0 0]) 0) ","
        (get (zelph/nn-eval net [0 1]) 0) ","
        (get (zelph/nn-eval net [1 0]) 0) ","
        (get (zelph/nn-eval net [1 1]) 0)))");
        CHECK(any_output_contains(collector, "0,1,1,0")); });
}

// ---------------------------------------------------------------------------
// SGD training, write-back into the graph, and reuse after recompilation
// ---------------------------------------------------------------------------

TEST_CASE("neural: SGD training converges, writes back, and is reusable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
u in NetIn
v in NetOut
%(zelph/nn-connect "u" "v" 0)
%(def net (zelph/nn-compile [(zelph/resolve "NetIn") (zelph/resolve "NetOut")]))
)");
        // Learning rate 0 measures the loss without changing weights:
        // w = 0, sample (x=1, t=2) => loss = 0.5 * (0-2)^2 = 2.
        collector.clear();
        interactive.process(R"(%(zelph/nn-train net [1] [2] 0))");
        CHECK(any_output_contains(collector, "2"));

        // Train: w converges to 2 (convex problem, deterministic).
        process_lines(interactive, R"(
%(for i 0 200 (zelph/nn-train net [1] [2] 0.1))
%(zelph/nn-write-back net)
)");
        // The trained weight must be back in the graph's weight store.
        collector.clear();
        interactive.process(R"(%(if (< (math/abs (- (zelph/weight "u" "v") 2)) 1e-6) "weight-ok" "weight-bad"))");
        CHECK(any_output_contains(collector, "weight-ok"));

        // A freshly compiled net must pick up the trained weight from the
        // graph and predict correctly (subsequent use of the trained NN).
        collector.clear();
        interactive.process(R"(%(let [net2 (zelph/nn-compile [(zelph/resolve "NetIn") (zelph/resolve "NetOut")])]
        (if (< (math/abs (- (get (zelph/nn-eval net2 [3]) 0) 6)) 1e-6) "predict-ok" "predict-bad")))");
        CHECK(any_output_contains(collector, "predict-ok")); });
}

// ---------------------------------------------------------------------------
// Sparsity preservation: absent synapses stay absent through training
// ---------------------------------------------------------------------------

TEST_CASE("neural: training never creates synapses that are absent in the graph")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
i1 in SpIn
i2 in SpIn
o1 in SpOut
%(zelph/nn-connect "i1" "o1" 0.5)
%(def net (zelph/nn-compile [(zelph/resolve "SpIn") (zelph/resolve "SpOut")]))
%(for i 0 50 (zelph/nn-train net [1 1] [3] 0.1))
%(zelph/nn-write-back net)
)");
        // i2 -> o1 was never connected and must still not exist.
        collector.clear();
        interactive.process(R"(%(if (nil? (zelph/weight "i2" "o1")) "still-sparse" "leaked"))");
        CHECK(any_output_contains(collector, "still-sparse")); });
}

// ---------------------------------------------------------------------------
// Layer wiring helper
// ---------------------------------------------------------------------------

TEST_CASE("neural: nn-connect-layers wires layers fully and preserves existing synapses")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a1 in LA
a2 in LA
b1 in LB
b2 in LB
%(zelph/nn-connect "a1" "b1" 7)
)");
        // a1 -> b1 already exists, so only 3 of 4 edges are created.
        collector.clear();
        interactive.process(R"(%(string "edges:" (zelph/nn-connect-layers "LA" "LB" 0)))");
        CHECK(any_output_contains(collector, "edges:3"));

        // The pre-existing synapse keeps its weight; new ones are 0 (scale 0).
        collector.clear();
        interactive.process(R"(%(string "w:" (zelph/weight "a1" "b1") "," (zelph/weight "a2" "b2")))");
        CHECK(any_output_contains(collector, "w:7,0"));

        // Re-running (even with a different scale) creates nothing and
        // overwrites nothing.
        collector.clear();
        interactive.process(R"(%(string "edges:" (zelph/nn-connect-layers "LA" "LB" 5)))");
        CHECK(any_output_contains(collector, "edges:0"));

        collector.clear();
        interactive.process(R"(%(string "w:" (zelph/weight "a1" "b1") "," (zelph/weight "a2" "b2")))");
        CHECK(any_output_contains(collector, "w:7,0")); });
}

// ---------------------------------------------------------------------------
// Node-addressed training and evaluation (multi-hot API)
// ---------------------------------------------------------------------------

TEST_CASE("neural: node-addressed training, evaluation, and graded activations")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
i1 in MIn
i2 in MIn
o1 in MOut
o2 in MOut
%(zelph/nn-connect-layers "MIn" "MOut" 0)
%(def net (zelph/nn-compile [(zelph/resolve "MIn") (zelph/resolve "MOut")]))
%
(for e 0 60
  (zelph/nn-train-nodes net ["i1"] ["o1"] 0.5)
  (zelph/nn-train-nodes net ["i2"] ["o2"] 0.5))
%
)");
        collector.clear();
        interactive.process(R"(%(string "top:" (zelph/name (get (get (zelph/nn-eval-nodes net ["i1"] 1) 0) 0)) "," (zelph/name (get (get (zelph/nn-eval-nodes net ["i2"] 1) 0) 0))))");
        CHECK(any_output_contains(collector, "top:o1,o2"));

        // Graded input activation scales the (linear) response: half the
        // input activation must give half the score.
        collector.clear();
        interactive.process(R"(%(let [full (get (get (zelph/nn-eval-nodes net ["i1"] 1) 0) 1) half (get (get (zelph/nn-eval-nodes net [["i1" 0.5]] 1) 0) 1)] (if (< (math/abs (- (* 2 half) full)) 1e-9) "graded-ok" "graded-bad")))");
        CHECK(any_output_contains(collector, "graded-ok"));

        // Nodes outside the layer are rejected instead of silently ignored.
        collector.clear();
        interactive.process(R"(%(try (zelph/nn-train-nodes net [(zelph/resolve "stranger")] ["o1"] 0.1) ([err] "unknown-node-rejected")))");
        CHECK(any_output_contains(collector, "unknown-node-rejected")); });
}

// ---------------------------------------------------------------------------
// Sparse input handling: the node-addressed entry points know which inputs
// are non-zero and evaluate only those. That must be an optimisation and
// nothing else - the same numbers, the same weights afterwards.
// ---------------------------------------------------------------------------

TEST_CASE("neural: node-addressed evaluation equals the dense pass")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Four inputs, two hidden units, one output: wide enough that a
        // single active input leaves most of the layer at zero, and deep
        // enough to cover the ReLU on the way.
        process_lines(interactive, R"(
a1 in SIn
a2 in SIn
a3 in SIn
a4 in SIn
h1 in SHid
h2 in SHid
s1 in SOut
%(zelph/nn-connect-layers "SIn" "SHid" 0.5)
%(zelph/nn-connect-layers "SHid" "SOut" 0.5)
%(def net (zelph/nn-compile [(zelph/resolve "SIn") (zelph/resolve "SHid") (zelph/resolve "SOut")]))
%(for e 0 40 (zelph/nn-train-nodes net ["a1" "a3"] [["s1" 0.7]] 0.05) (zelph/nn-train-nodes net ["a2"] [["s1" -0.3]] 0.05))
)");

        // One active input, then two, then a graded one: in each case the
        // node-addressed result must equal the dense vector's exactly. The
        // skipped terms are multiplications by zero, so this is equality,
        // not approximate agreement.
        collector.clear();
        interactive.process(R"(%(let [d (get (zelph/nn-eval net [1 0 1 0]) 0)
                                      s (get (get (zelph/nn-eval-nodes net ["a1" "a3"] 1) 0) 1)]
                                  (if (= d s) "two-active-equal" (string "two-active-differ " d " " s))))");
        CHECK(any_output_contains(collector, "two-active-equal"));

        collector.clear();
        interactive.process(R"(%(let [d (get (zelph/nn-eval net [0 1 0 0]) 0)
                                      s (get (get (zelph/nn-eval-nodes net ["a2"] 1) 0) 1)]
                                  (if (= d s) "one-active-equal" (string "one-active-differ " d " " s))))");
        CHECK(any_output_contains(collector, "one-active-equal"));

        collector.clear();
        interactive.process(R"(%(let [d (get (zelph/nn-eval net [0 0 0.25 0]) 0)
                                      s (get (get (zelph/nn-eval-nodes net [["a3" 0.25]] 1) 0) 1)]
                                  (if (= d s) "graded-equal" (string "graded-differ " d " " s))))");
        CHECK(any_output_contains(collector, "graded-equal"));

        // An empty input activates nothing at all, which is the degenerate
        // case of the sparse loop.
        collector.clear();
        interactive.process(R"(%(let [d (get (zelph/nn-eval net [0 0 0 0]) 0)
                                      s (get (get (zelph/nn-eval-nodes net [] 1) 0) 1)]
                                  (if (= d s) "empty-equal" (string "empty-differ " d " " s))))");
        CHECK(any_output_contains(collector, "empty-equal")); });
}

TEST_CASE("neural: node-addressed training leaves the same weights as the dense pass")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Two identical nets over disjoint layers, one trained through the
        // dense entry point and one through the node-addressed one. Every
        // weight must match afterwards, including those of the inputs that
        // were never active - they must not drift.
        process_lines(interactive, R"(
d1 in DIn
d2 in DIn
d3 in DIn
e1 in DOut
n1 in NIn
n2 in NIn
n3 in NIn
f1 in NOut
%(zelph/nn-connect-layers "DIn" "DOut" 0)
%(zelph/nn-connect-layers "NIn" "NOut" 0)
%(def dense (zelph/nn-compile [(zelph/resolve "DIn") (zelph/resolve "DOut")]))
%(def sparse (zelph/nn-compile [(zelph/resolve "NIn") (zelph/resolve "NOut")]))
%(for i 0 50 (zelph/nn-train dense [1 0 0.5] [0.8] 0.05) (zelph/nn-train dense [0 1 0] [-0.4] 0.05))
%(for i 0 50 (zelph/nn-train-nodes sparse [["n1" 1] ["n3" 0.5]] [["f1" 0.8]] 0.05) (zelph/nn-train-nodes sparse ["n2"] [["f1" -0.4]] 0.05))
%(zelph/nn-write-back dense)
%(zelph/nn-write-back sparse)
)");
        collector.clear();
        interactive.process(R"(%(if (and (= (zelph/weight "d1" "e1") (zelph/weight "n1" "f1"))
                                         (= (zelph/weight "d2" "e1") (zelph/weight "n2" "f1"))
                                         (= (zelph/weight "d3" "e1") (zelph/weight "n3" "f1")))
                                  "weights-equal"
                                  (string "weights-differ "
                                          (zelph/weight "d1" "e1") "/" (zelph/weight "n1" "f1") " "
                                          (zelph/weight "d2" "e1") "/" (zelph/weight "n2" "f1") " "
                                          (zelph/weight "d3" "e1") "/" (zelph/weight "n3" "f1"))))");
        CHECK(any_output_contains(collector, "weights-equal"));

        // The reported loss is the value before the update, so both paths
        // must also agree on it.
        collector.clear();
        interactive.process(R"(%(let [d (zelph/nn-train dense [1 0 0.5] [0.8] 0)
                                      s (zelph/nn-train-nodes sparse [["n1" 1] ["n3" 0.5]] [["f1" 0.8]] 0)]
                                  (if (= d s) "loss-equal" (string "loss-differ " d " " s))))");
        CHECK(any_output_contains(collector, "loss-equal")); });
}

// ---------------------------------------------------------------------------
// Weight snapshots: a training run can be put back where it was best
// ---------------------------------------------------------------------------

TEST_CASE("neural: a snapshot restores the exact weights it was taken from")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
p1 in PIn
p2 in PIn
q1 in POut
%(zelph/nn-connect-layers "PIn" "POut" 0)
%(def net (zelph/nn-compile [(zelph/resolve "PIn") (zelph/resolve "POut")]))
%(for i 0 30 (zelph/nn-train-nodes net ["p1"] [["q1" 1.0]] 0.1))
%(def good (zelph/nn-snapshot net))
%(def good-score (get (get (zelph/nn-eval-nodes net ["p1"] 1) 0) 1))
%(for i 0 30 (zelph/nn-train-nodes net ["p1"] [["q1" -5.0]] 0.1))
)");
        // The net has been trained away from where the snapshot was taken.
        collector.clear();
        interactive.process(R"(%(if (> (math/abs (- (get (get (zelph/nn-eval-nodes net ["p1"] 1) 0) 1) good-score)) 0.5) "moved-away" "did-not-move"))");
        CHECK(any_output_contains(collector, "moved-away"));

        // Restoring must reproduce the earlier score exactly, not closely.
        collector.clear();
        interactive.process(R"(%(do (zelph/nn-restore net good)
                                    (if (= (get (get (zelph/nn-eval-nodes net ["p1"] 1) 0) 1) good-score) "restored-exactly" "restored-differently")))");
        CHECK(any_output_contains(collector, "restored-exactly"));

        // And the restored weights are the ones write-back puts in the graph.
        collector.clear();
        interactive.process(R"(%(do (zelph/nn-write-back net)
                                    (if (= (zelph/weight "p1" "q1") good-score) "written-back" (string "written-back-differs " (zelph/weight "p1" "q1")))))");
        CHECK(any_output_contains(collector, "written-back"));

        // A snapshot of the wrong shape is rejected rather than truncated.
        collector.clear();
        interactive.process(R"(%(try (zelph/nn-restore net [[1 2 3 4 5]]) ([err] "shape-rejected")))");
        CHECK(any_output_contains(collector, "shape-rejected"));

        collector.clear();
        interactive.process(R"(%(try (zelph/nn-restore net [[0 0] [0 0]]) ([err] "count-rejected")))");
        CHECK(any_output_contains(collector, "count-rejected")); });
}

TEST_CASE("neural: restoring never resurrects a synapse the graph does not have")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Only one of the two possible synapses exists in the graph.
        process_lines(interactive, R"(
r1 in RIn
r2 in RIn
s1 in ROut
%(zelph/nn-connect "r1" "s1" 0.25)
%(def net (zelph/nn-compile [(zelph/resolve "RIn") (zelph/resolve "ROut")]))
%(def snap (zelph/nn-snapshot net))
)");
        // Hand-write a weight into the absent slot and restore it. Training
        // has always refused to create that synapse; restoring must too.
        collector.clear();
        interactive.process(R"(%(do (put (get snap 0) 1 9.0)
                                    (zelph/nn-restore net snap)
                                    (zelph/nn-write-back net)
                                    (if (nil? (zelph/weight "r2" "s1")) "absent-stays-absent" (string "leaked " (zelph/weight "r2" "s1")))))");
        CHECK(any_output_contains(collector, "absent-stays-absent")); });
}

// ---------------------------------------------------------------------------
// Graph-driven training: the reasoning query defines the training data
// ---------------------------------------------------------------------------

TEST_CASE("neural: graph-driven link prediction trained via reasoning queries")
{
    // Mini knowledge graph in wikidata style. The training set is gathered
    // from the graph itself via zelph/query; the net then predicts O given
    // (S, P) as multi-hot input.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
earth haspart crust
earth color blue
mars haspart core
mars color red
earth in KIn
mars in KIn
haspart in KIn
color in KIn
crust in KOut
blue in KOut
core in KOut
red in KOut
%(zelph/nn-connect-layers "KIn" "KOut" 0)
%(def net (zelph/nn-compile [(zelph/resolve "KIn") (zelph/resolve "KOut")]))
%
# The reasoning engine's query mechanism defines what the net is trained
# on: each binding row becomes one (S P -> O) sample.
(def samples @[])
(each pred ["haspart" "color"]
  (each row (zelph/query (zelph/fact 'S pred 'O))
    (array/push samples [[(get row 'S) pred] [(get row 'O)]])))
(for epoch 0 200
  (each [ins tgts] samples
    (zelph/nn-train-nodes net ins tgts 0.2)))
%
)");
        collector.clear();
        interactive.process(R"(%(defn top1 [s p] (zelph/name (get (get (zelph/nn-eval-nodes net [s p] 1) 0) 0))))");
        interactive.process(R"(%(string "pred:" (top1 "earth" "haspart") "," (top1 "earth" "color") "," (top1 "mars" "haspart") "," (top1 "mars" "color")))");
        CHECK(any_output_contains(collector, "pred:crust,blue,core,red")); });
}

// ---------------------------------------------------------------------------
// Neural rule conditions (≈)
// ---------------------------------------------------------------------------

TEST_CASE("neural: approx guard mode verifies facts and propagates confidence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
s1 in GIn
o1 in GOut
o2 in GOut
%(zelph/nn-connect "s1" "o1" 0.8)
%(zelph/nn-connect "s1" "o2" 0.2)
gnet nn-layers <GIn GOut>
s1 relG o1
s1 relG o2
(A relG B, ≈gnet(A relG B)) => (A verifiedG B)
)");
        // o1 scores 0.8 > 0.5 -> verified; o2 scores 0.2 -> condition fails.
        CHECK(any_output_starts_with(collector, "( s1 verifiedG o1 )"));
        CHECK_FALSE(any_output_starts_with(collector, "( s1 verifiedG o2 )"));

        // Confidence propagation: the deduced fact's probability (0.8)
        // lives in the shared weight store on the fact->predicate edge.
        collector.clear();
        interactive.process(R"(%(let [f (zelph/fact "s1" "verifiedG" "o1")] (if (< 0.79 (zelph/weight f "verifiedG") 0.81) "conf-ok" "conf-bad")))");
        CHECK(any_output_contains(collector, "conf-ok")); });
}

TEST_CASE("neural: approx generator mode proposes bindings above threshold")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
s2 in HIn
c1 in HOut
c2 in HOut
%(zelph/nn-connect "s2" "c1" 0.9)
%(zelph/nn-connect "s2" "c2" 0.1)
hnet nn-layers <HIn HOut>
s2 marked yes
(A marked yes, ≈hnet(A relH X)) => (A suggestedH X)
)");
        CHECK(any_output_starts_with(collector, "( s2 suggestedH c1 )"));
        CHECK_FALSE(any_output_starts_with(collector, "( s2 suggestedH c2 )")); });
}

TEST_CASE("neural: approx with missing net definition fails the condition gracefully")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
s3 relQ o3
(A relQ B, ≈ghostnet(A relQ B)) => (A verifiedQ B)
)");
        // No nn-layers definition for ghostnet: the condition simply
        // fails; no deduction, no crash.
        CHECK_FALSE(any_output_starts_with(collector, "( s3 verifiedQ")); });
}

// ---------------------------------------------------------------------------
// nn.zph end-to-end: train from graph facts, use via Janet and via ≈
// ---------------------------------------------------------------------------

TEST_CASE("neural: nn.zph link predictor end-to-end with approx rules")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import nn");
        process_lines(interactive, R"(
earth haspart crust
mars haspart core
%(def h (nn/link-predictor "wnet" [(zelph/resolve "earth") (zelph/resolve "mars")] ["haspart"] :epochs 200 :lr 0.2))
)");
        // Direct prediction through the Janet layer.
        collector.clear();
        interactive.process(R"(%(let [[n s] (first (nn/predict-names h ["earth" "haspart"] 1))] n))");
        CHECK(any_output_contains(collector, "crust"));

        // The declared net is addressable from ≈ rules via its graph name.
        collector.clear();
        process_lines(interactive, R"(
(A haspart B, ≈wnet(A haspart B)) => (A verifiedW B)
)");
        CHECK(any_output_starts_with(collector, "( earth verifiedW crust )"));
        CHECK(any_output_starts_with(collector, "( mars verifiedW core )"));
        CHECK_FALSE(any_output_starts_with(collector, "( earth verifiedW core )")); });
}

// ---------------------------------------------------------------------------
// Synapses create no adjacency: relation nodes are safe neurons
// ---------------------------------------------------------------------------

TEST_CASE("neural: synapses never corrupt the fact structure of relation-node neurons")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
s relF o
)");
        // Wire synapses into and out of the fact node itself. Before the
        // synapse-store fix, the edge into the fact node carried the
        // signature of an additional object and the edge out of it the
        // signature of a predicate link.
        process_lines(interactive, R"(
%(def f (zelph/fact "s" "relF" "o"))
%(zelph/nn-connect "probe" f 0.5)
%(zelph/nn-connect f "probe2" 0.25)
)");
        // The fact must still decompose exactly as before; probe/probe2
        // must not surface anywhere in its components.
        collector.clear();
        interactive.process("s _P _O");
        CHECK(answers_contain(collector, "s relF o"));
        CHECK_FALSE(any_output_contains(collector, "probe"));

        // Hash-consing identity: re-deriving the same fact must yield the
        // identical node (this used to trip the corrupt-database assert
        // in check_fact).
        collector.clear();
        interactive.process(R"(%(if (= (zelph/fact "s" "relF" "o") f) "identity-ok" "identity-broken"))");
        CHECK(any_output_contains(collector, "identity-ok")); });
}

TEST_CASE("neural: structural cons lists survive being wired as neurons")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // l2 is the cdr of l4 (suffix sharing), mirroring the number case
        // where &2 is the cdr of &4. A synapse l4 -> l2 runs opposite to
        // the real structural edge l2 -> l4 and used to mimic a predicate
        // link on l4 plus an extra object on l2.
        process_lines(interactive, R"(
%(def l2 (zelph/list "x"))
%(def l4 (zelph/list "y" "x"))
%(zelph/nn-connect "probe" l2 0.5)
%(zelph/nn-connect l4 l2 0.75)
)");
        collector.clear();
        interactive.process(R"(%(if (and (= (zelph/list "x") l2) (= (zelph/list "y" "x") l4)) "identity-ok" "identity-broken"))");
        CHECK(any_output_contains(collector, "identity-ok")); });
}

// ---------------------------------------------------------------------------
// Compile masks come from the synapse store, not from fact topology
// ---------------------------------------------------------------------------

TEST_CASE("neural: structural fact edges never enter compiled masks as phantom synapses")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The structural object edge l2 -> l4 exists in the adjacency maps.
        // With l2 as input and l4 as output neuron (the "&2 divides &4"
        // constellation), the old has_right_edge probing turned it into a
        // trainable weight-1 phantom synapse.
        process_lines(interactive, R"(
%(def l2 (zelph/list "x"))
%(def l4 (zelph/list "y" "x"))
%(zelph/fact l2 "in" (zelph/resolve "QIn"))
%(zelph/fact l4 "in" (zelph/resolve "QOut"))
%(def net (zelph/nn-compile [(zelph/resolve "QIn") (zelph/resolve "QOut")]))
)");
        collector.clear();
        interactive.process(R"(%(string "out:" (get (zelph/nn-eval net [1]) 0)))");
        CHECK(any_output_contains(collector, "out:0"));
        CHECK_FALSE(any_output_contains(collector, "out:0.5"));

        // A real synapse on the same pair must be visible after recompiling.
        process_lines(interactive, R"(
%(zelph/nn-connect l2 l4 0.5)
%(def net2 (zelph/nn-compile [(zelph/resolve "QIn") (zelph/resolve "QOut")]))
)");
        collector.clear();
        interactive.process(R"(%(string "out:" (get (zelph/nn-eval net2 [1]) 0)))");
        CHECK(any_output_contains(collector, "out:0.5")); });
}

// ---------------------------------------------------------------------------
// zelph/weight three-way semantics
// ---------------------------------------------------------------------------

TEST_CASE("neural: zelph/weight distinguishes synapse entries, plain edges, and absence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
s relW o
%(zelph/nn-connect "n1" "n2" 0.25)
)");
        // Synapse entry: stored value.
        collector.clear();
        interactive.process(R"(%(string "w:" (zelph/weight "n1" "n2")))");
        CHECK(any_output_contains(collector, "w:0.25"));

        // Real edge without a stored entry (fact asserted with probability
        // 1): canonical weight 1.
        collector.clear();
        interactive.process(R"(%(string "w:" (zelph/weight (zelph/fact "s" "relW" "o") "relW")))");
        CHECK(any_output_contains(collector, "w:1"));

        // Neither synapse nor edge: nil.
        collector.clear();
        interactive.process(R"(%(if (nil? (zelph/weight "n1" "nowhere")) "no-pair" "pair"))");
        CHECK(any_output_contains(collector, "no-pair")); });
}

// ---------------------------------------------------------------------------
// Thread safety of a compiled network
//
// The engine that drives this feature evaluates a network from a search while
// a second thread trains it, so the guarantee has to be stated and pinned:
// any number of threads may evaluate concurrently, and a training step or
// set_weights excludes them for its duration. Before the lock existed,
// train_step wrote _w while forward read it -- a data race, i.e. undefined
// behaviour rather than merely a stale number.
//
// A test cannot prove the absence of a race; what it can do is exercise the
// path hard enough that a sanitizer build has something to find, and pin the
// two properties that must hold with or without concurrency: evaluation stays
// finite and in range, and the weights a reader observes are always a whole
// set, never a half-written one. Run the suite under -fsanitize=thread to
// turn this into an actual race check.
// ---------------------------------------------------------------------------

TEST_CASE("neural: a network can be evaluated while another thread trains it")
{
    run_parallel_mode([](auto& collector, auto& interactive)
                      {
        process_lines(interactive, R"(
a in TLin
b in TLin
h in TLhid
o in TLout
%(zelph/nn-connect "a" "h" 0.5)
%(zelph/nn-connect "b" "h" -0.25)
%(zelph/nn-connect "h" "o" 0.75)
%(def tnet (zelph/nn-compile [(zelph/resolve "TLin") (zelph/resolve "TLhid") (zelph/resolve "TLout")]))
)");
        collector.clear();

        // Two readers and one trainer on the same compiled net. The readers
        // report whether every value they saw was finite; the trainer just
        // hammers the weights.
        interactive.process(R"js(%(do
  (def ch (ev/thread-chan 8))
  (defn reader []
    (var ok true)
    (loop [_ :range [0 400]]
      (def r (zelph/nn-eval-nodes tnet [(zelph/resolve "a")] 1))
      (def v (get (first r) 1))
      (unless (and (number? v) (= v v) (< (math/abs v) 1e6)) (set ok false)))
    ok)
  (ev/spawn-thread (ev/give ch (string "reader1=" (reader))))
  (ev/spawn-thread (ev/give ch (string "reader2=" (reader))))
  (ev/spawn-thread
    (do
      (loop [_ :range [0 400]]
        (zelph/nn-train-nodes tnet [(zelph/resolve "a")] [[(zelph/resolve "o") 1]] 0.01))
      (ev/give ch "trainer=done")))
  (zelph/out (string (ev/take ch) " " (ev/take ch) " " (ev/take ch)))))js");

        CHECK(any_output_contains(collector, "reader1=true"));
        CHECK(any_output_contains(collector, "reader2=true"));
        CHECK(any_output_contains(collector, "trainer=done")); });
}

TEST_CASE("neural: a snapshot taken during training is a whole set of weights")
{
    run_parallel_mode([](auto& collector, auto& interactive)
                      {
        process_lines(interactive, R"(
p in SLin
q in SLhid
r in SLout
%(zelph/nn-connect "p" "q" 0.5)
%(zelph/nn-connect "q" "r" 0.5)
%(def snet (zelph/nn-compile [(zelph/resolve "SLin") (zelph/resolve "SLhid") (zelph/resolve "SLout")]))
)");
        collector.clear();

        // zelph/nn-snapshot used to hand out a reference into the live weight
        // store; it returns a copy taken under the lock now, so its shape is
        // always the compiled shape even while a trainer runs.
        interactive.process(R"js(%(do
  (def ch (ev/thread-chan 4))
  (ev/spawn-thread
    (do (loop [_ :range [0 300]]
          (zelph/nn-train-nodes snet [(zelph/resolve "p")] [[(zelph/resolve "r") 1]] 0.01))
        (ev/give ch :trained)))
  (var shapes-ok true)
  (loop [_ :range [0 300]]
    (def s (zelph/nn-snapshot snet))
    (unless (and (= 2 (length s)) (= 1 (length (get s 0))) (= 1 (length (get s 1))))
      (set shapes-ok false)))
  (ev/take ch)
  (zelph/out (string "shapes=" shapes-ok))))js");

        CHECK(any_output_contains(collector, "shapes=true")); });
}
