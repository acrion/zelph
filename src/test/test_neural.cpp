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
        interactive.process(".import " + std::string(ZELPH_SOURCE_DIR) + "/sample_scripts/nn.zph");
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