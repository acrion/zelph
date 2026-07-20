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

// Demo button definitions for the zelph playground. Buttons type visible
// commands into the terminal - visitors learn the syntax by watching.
// `requires` is advisory only: buttons are never disabled (the visitor can
// always type prerequisites manually); unmet requirements just show a marker.
// Note: sparql blocks end with an empty line - the trailing '' is deliberate.

export const DEMO_GROUPS = [
  {
    title: "Arithmetic as Inference",
    buttons: [
      {
        id: "1.1",
        label: "Load arithmetic",
        command: ".import binary-arithmetic",
        info: `Loads the arithmetic rule module. There is no arithmetic code in zelph's engine: digits are ordinary graph nodes, numbers are cons-lists of digits, and addition, subtraction, multiplication and division are defined entirely by inference rules &mdash; here on top of a 16-fact full-adder truth table, computing internally in base 2 while reading and writing decimal. Full documentation: <a href="https://acrion.github.io/zelph/arithmetic/" target="_blank">zelph/arithmetic</a>`,
      },
      {
        id: "1.2",
        label: "Multiply big numbers",
        requires: ["1.1"],
        command: "(&34953489 * &923879384) = X",
        info: `Multiplies two large numbers &mdash; as pure inference. The engine derives the result through rule-driven graph rewriting, digit by digit, carry by carry. Since numbers are lists, there is no word size: this works for arbitrarily large numbers, and every result arrives with its derivation (⇐).`,
      },
      {
        id: "1.3",
        label: "Peek inside: additions",
        requires: ["1.2"],
        command: "(X + Y) = Z",
        info: `A query with variables lists all <code>+</code> facts currently in the graph. In the context of these examples, that means the intermediate additions the multiplication above spawned internally &mdash; multiplication delegates its accumulation steps to the addition module as ordinary facts. Derived knowledge stays in the graph and is reused by later computations.`,
      },
      {
        id: "1.4",
        label: "Divide",
        requires: ["1.1"],
        command: "(&2345 / &53) = X",
        info: `Euclidean division, again as pure inference &mdash; and the deepest cascade in the arithmetic system: candidate products come from the multiplication rules, candidate differences from subtraction, and quotient-digit selection from comparison. The modules know nothing about each other; they communicate exclusively through shared facts.`,
      },
      {
        id: "1.5",
        label: "Peek inside: subtractions",
        requires: ["1.4"],
        command: "(X - Y) = Z",
        info: `Lists all <code>-</code> facts &mdash; in this context, the candidate differences the division above asserted while selecting quotient digits. Division by a number that doesn't fit simply produces no difference fact: undefinedness is encoded as absence, no error machinery involved.`,
      },
      {
        id: "1.6",
        label: "Remainder",
        requires: ["1.1"],
        command: "(&2345 mod &53) = X",
        info: `The remainder of the division above. Since all intermediate states are already in the graph, this answers almost instantly &mdash; memoization is a property of the representation, not a feature.`,
      },
    ],
  },
  {
    title: "Number Theory and Meta-Rules",
    buttons: [
      {
        id: "2.1",
        label: "Load primality test",
        requires: ["1.1"],
        command: ".import primes-naf",
        info: `Loads the primality module (requires the arithmetic button above). It contributes no arithmetic of its own &mdash; trial division up to the square bound is expressed by asserting ordinary <code>+</code>, <code>*</code>, <code>mod</code> and <code>cmp</code> facts, which the arithmetic modules answer. The textbook rule "N is prime if it has no divisor" is written literally, using negation-as-failure. (This module also works on top of <code>.import arithmetic</code>, which computes in base 10 &mdash; the recursion rules are byte-identical, only the digit tables differ. Try typing it!)`,
      },
      {
        id: "2.2",
        label: "Test a prime",
        requires: ["2.1"],
        command: "(:testprime &53) = X",
        info: `Runs the full primality test for 53. Watch the cascade: division inside multiplication inside subtraction inside comparison &mdash; and at the end, a plain relational fact: 53 is prime.`,
      },
      {
        id: "2.3",
        label: "Test a composite",
        requires: ["2.1"],
        command: "(:testprime &42) = X",
        info: `Tests 42. Note: the verdict may appear a little further up in the output, among the derivation trace, rather than at the very end. Press the button again and the answer appears immediately &mdash; zelph remembers everything it ever derived (unless you sandbox work in clusters: <a href="https://acrion.github.io/zelph/#node-clusters-transactional-workspaces" target="_blank">node clusters</a>).`,
      },
      {
        id: "2.4",
        label: "Batch: test 2–20",
        requires: ["2.1"],
        command:
          '%(loop [n :range [2 21]]  (def num (zelph/number (string n)))  (zelph/fact num "testprime" num))',
        info: `Runs the full primality test for every number from 2 to 20 in a single reasoning run &mdash; each test cascading through division, multiplication, subtraction and comparison. The derived facts (<code>isprime</code>, <code>hasdivisor</code>, comparisons, …) become the shared dataset for the query, SPARQL and neural-network groups below. This is also a taste of the Janet scripting layer: a three-line loop generating facts programmatically.`,
      },
      {
        id: "2.5",
        label: "List primes",
        requires: ["2.4"],
        command: "(:testprime X) = prime",
        info: `All numbers the scan proved prime &mdash; as a plain query against derived facts.`,
      },
      {
        id: "2.6",
        label: "List composites",
        requires: ["2.4"],
        command: "(:testprime X) = composite",
        info: `All numbers the scan disproved. Note that 0 and 1 appear in neither list: they receive no verdict at all &mdash; partiality by absence, the same principle that makes division by zero simply derive nothing.`,
      },
      {
        id: "2.7",
        label: "Peek inside: divisors",
        requires: ["2.4"],
        command: "X hasdivisor Y",
        info: `The <code>hasdivisor</code> relation was introduced by the primality module; this query lists every divisor (up to the square bound) the scan recorded. These facts will shortly become neural-network training data.`,
      },
      {
        id: "2.8",
        label: "Peek inside: products",
        requires: ["2.4"],
        command: "(X * Y) = Z",
        info: `The multiplications the primality tests spawned internally &mdash; candidate products for trial division.`,
      },
      {
        id: "2.9",
        label: "Peek inside: greater-than",
        requires: ["2.4"],
        command: "X > Y",
        info: `Comparison results are ordinary relational facts, not opaque boolean returns. This matters for the next buttons.`,
      },
      {
        id: "2.10",
        label: "Teach transitivity",
        command: "(A R B, B R C, R is transitive) => (A R C)",
        info: `This rule teaches zelph the <em>concept</em> of transitivity &mdash; for any relation R. Note that <code>is</code> and <code>transitive</code> are nodes zelph knows nothing about; their meaning emerges purely from their use in facts and rules. The variable R ranges over predicates themselves, which is possible because predicates are first-class graph nodes &mdash; a class of rule that standard Datalog cannot express. More: <a href="https://acrion.github.io/zelph/logic/" target="_blank">zelph/logic</a>`,
      },
      {
        id: "2.11",
        label: "Declare > transitive",
        command: "> is transitive",
        info: `One fact &mdash; and the rule above finds matches across all the comparison facts the primality scan derived, deducing new greater-than relations that were never computed digit-wise. Computed facts and declared knowledge feed the same inference engine.`,
      },
      {
        id: "2.12",
        label: "Declare < transitive",
        command: "< is transitive",
        info: `Same for less-than. Computation and reasoning are literally the same operation.`,
      },
      {
        id: "2.13",
        label: "Consistency rule",
        command: "(:isprime N, N hasdivisor D) => !",
        info: `A constraint rule: a number that is prime and has a divisor is a contradiction (consequence <code>!</code>). On the current data it stays silent &mdash; the derived facts are consistent. This is the exact mechanism behind zelph's Wikidata work, where such rules flag thousands of inconsistencies in a 100-million-fact import.`,
      },
      {
        id: "2.14",
        label: "Provoke a contradiction",
        requires: ["2.4", "2.13"],
        command: ".cluster demo\n:isprime &9\n.cluster-drop demo",
        info: `Asserts a falsehood. The constraint rule fires, the contradictory fact is detected. To prevent that this contradiction is shown after each future step, we activate a cluster for this demo fact and drop it afterwards (alternatively, it is also possible to use <code>.prune-facts X isprime X</code>). Detection rather than prevention is the design: it is what makes auditing huge, inconsistent real-world datasets feasible.`,
      },
    ],
  },
  {
    title: "SPARQL over Derived Facts",
    buttons: [
      {
        id: "3.1",
        label: "Load SPARQL",
        command: ".import sparql",
        info: `Loads a SPARQL query engine &mdash; implemented entirely as a Janet script on zelph's public API, not in the C++ core. It was built for the Wikidata use case but is fully general purpose: it queries whatever facts exist in the network, including everything the reasoner just derived. Documentation: <a href="https://acrion.github.io/zelph/sparql/" target="_blank">zelph/sparql</a>`,
      },
      {
        id: "3.2",
        label: "SPARQL: primes",
        requires: ["3.1", "2.4"],
        command: "sparql\nSELECT ?n WHERE { ?n isprime ?n . }\n",
        info: `A SPARQL query over facts the reasoner derived minutes ago &mdash; no triple store, no endpoint: the semantic network itself is the queried graph. Requires the batch button from the group above.`,
      },
      {
        id: "3.3",
        label: "SPARQL: composites via MINUS",
        requires: ["3.1", "2.4"],
        command:
          "sparql\nSELECT DISTINCT ?n WHERE { ?n testprime ?n . MINUS { ?n isprime ?n . } }\n",
        info: `Tested but never proven prime &mdash; the composites. Note the symmetry: the rule engine defined primality via negation-as-failure, and SPARQL expresses the complement via MINUS. The same closed-world idea, in two query languages.`,
      },
      {
        id: "3.4",
        label: "SPARQL: divisor counts",
        requires: ["3.1", "2.4"],
        command:
          "sparql\nSELECT ?n (COUNT(?d) AS ?c) WHERE { ?n hasdivisor ?d . } GROUP BY ?n\n",
        info: `Aggregation over derived arithmetic: how many divisors (up to the square bound) did the prime scan record per number?`,
      },
    ],
  },
  {
    title: "Neural Networks in the Graph",
    buttons: [
      {
        id: "4.1",
        label: "Load neural helpers",
        command: ".import nn",
        info: `zelph's graph nodes can serve not only as ontology entities but directly as neurons: synapses are weighted connections between nodes, stored in the same weight store that holds fact probabilities. No parallel tensor world, no identifier mapping &mdash; the number 12 and "input neuron 12" are the same node. Documentation: <a href="https://acrion.github.io/zelph/neural/" target="_blank">zelph/neural</a>`,
      },
      {
        id: "4.2",
        label: "Train a divisor predictor",
        requires: ["4.1", "2.4"],
        command: `%
(def hd (zelph/resolve "hasdivisor"))
(def tested @[])
(each row (zelph/query (zelph/fact '_N "testprime" '_N))
  (array/push tested (get row '_N)))
(def divnet (nn/link-predictor "divnet" tested [hd] :epochs 200 :lr 0.2))
%`,
        info: `Trains a tiny link predictor on the <code>hasdivisor</code> facts the prime scan derived &mdash; symbolic reasoning generated the training data. The neurons are the tested numbers themselves; training data is gathered by a reasoning query.`,
      },
      {
        id: "4.3",
        label: "Peek inside the net",
        requires: ["4.2"],
        command:
          '%(each score (nn/predict-names divnet [(zelph/number "12") (zelph/resolve "hasdivisor")] 3) (pp score))',
        info: `The net's raw scores for the number 12. With one-hot targets, the outputs approximate P(divisor | number): 12 has two recorded divisors (2 and 3), so the mass splits roughly 50/50 &mdash; hovering right at the 0.5 decision threshold. This explains what the rules below will and won't accept.`,
      },
      {
        id: "4.4",
        label: "Neural rule conditions",
        requires: ["4.2"],
        command:
          "(:testprime A, A hasdivisor X, ≈divnet(A hasdivisor X)) => (A verifiedDivisor X)\n(:testprime A, ≈divnet(A hasdivisor X), ¬(A hasdivisor X)) => (A candidateDivisor X)",
        info: `Two rules consult the net via the ≈ operator. The guard rule re-verifies known divisor facts (numbers whose probability mass splits between two divisors clear the threshold for only one of them). The generator rule lets the net propose divisors where the scan found none &mdash; note the interplay of ≈ and negation. Spoiler: with identity-based inputs, the net has learned the dataset's prior &mdash; "an unknown number is probably even" &mdash; and suspects every prime of being divisible by 2. Its guess for the number 2 itself is accidentally correct. Each deduced fact carries the net's confidence as its fact probability.`,
      },
      {
        id: "4.5",
        label: "SPARQL over the net's ideas",
        requires: ["4.4", "3.1"],
        command: "sparql\nSELECT ?n ?d WHERE { ?n candidateDivisor ?d . }\n",
        info: `Full circle: symbolic arithmetic derived the training data, the neural net proposed new candidate facts with confidences, and SPARQL queries the result &mdash; all within one graph, processed by one engine.`,
      },
    ],
  },
  {
    // Publish together with the paper release: this group imports stdlib
    // modules (binary-nand-arithmetic, symbolic-core, diff, eml) that are
    // pushed only with the paper. If the WASM build embeds the stdlib via
    // an explicit file list rather than a glob, add these four modules
    // there as well -- otherwise the buttons fail in the playground while
    // working locally.
    title: "From NAND to EML",
    buttons: [
      {
        id: "5.1",
        label: "Arithmetic from one NAND gate",
        command: ".import binary-nand-arithmetic",
        info: `Loads a base-2 arithmetic module whose only gate datum is the single axiom <code>(1 nand 1) out 0</code>. One negation-as-failure rule completes the truth table, five rules build the classical gate library (NOT, AND, OR, XOR, majority) from NAND alone, and nine synthesis rules derive all 52 digit-table facts that the module from button 1.1 states by hand &mdash; on the same predicates, so the shared recursion layer sees no difference. Self-contained: group 1 is not required. For the full provenance show below, best start from a freshly loaded playground &mdash; if the hand-written tables are already present, the derived facts coincide with existing nodes.`,
      },
      {
        id: "5.2",
        label: "Peek inside: the gate table",
        requires: ["5.1"],
        command: "(A nand B) out X",
        info: `The complete NAND truth table: one asserted axiom and three rows derived by negation-as-failure &mdash; every digit pair that is not the zero row outputs 1. Note the derivation markers (⇐) on the derived rows.`,
      },
      {
        id: "5.3",
        label: "Multiply on the NAND substrate",
        requires: ["5.1"],
        command: "(&12 * &34) = X",
        info: `The same digit-by-digit inference as in group 1 &mdash; but every gate-table lookup in the cascade now carries its own derivation, bottoming out, transitively, in the single NAND axiom. A provenance chain from 12&times;34&nbsp;=&nbsp;408 down to one gate fact.`,
      },
      {
        id: "5.4",
        label: "Load symbolic layer",
        requires: ["5.1"],
        command: ".import symbolic-core",
        info: `Loads the symbolic layer: simplification as forward chaining. Symbolic terms use the <em>same</em> <code>+</code>/<code>*</code> predicates as the numeric modules; atoms declare a sort (<code>~ symvar</code>, <code>~ symconst</code>), and a marker-gated rule cascade computes normal forms bottom-up in a single pass. Works on top of any of the three arithmetic modules &mdash; here, on the NAND substrate.`,
      },
      {
        id: "5.5",
        label: "Constant folding for free",
        requires: ["5.4"],
        command:
          ":simplify ((&2 + &3) * (&4 + &6))\n(:simplify ((&2 + &3) * (&4 + &6))) = X",
        info: `The simplifier has no folding code. Its reduced forms are ordinary arithmetic facts, so the arithmetic module computes their <code>=</code> results, and one bridge rule <code>(T red C, C = R) =&gt; (T rw R)</code> adopts them. Watch <code>(&5 * &10)</code> being materialized and computed mid-simplification &mdash; and on this substrate, even the folded 50 traces back to the NAND axiom.`,
      },
      {
        id: "5.6",
        label: "Declared knowledge simplifies",
        requires: ["5.4"],
        command:
          "a ~ symconst\nb ~ symconst\nc ~ symconst\n(a + b) = c\n:simplify (a + b)\n(:simplify (a + b)) = X",
        info: `The same bridge rule consumes <em>declared</em> equations: stating <code>(a + b) = c</code> over opaque constants makes simplification answer <code>c</code>. An equation imported from a knowledge graph drives rewriting exactly like a computed one &mdash; computation and knowledge share one fact space.`,
      },
      {
        id: "5.7",
        label: "Load differentiation",
        requires: ["5.4"],
        command: ".import diff",
        info: `Symbolic differentiation in the same architecture. Derivatives of function symbols are facts (<code>exp hasderivative exp</code>) consumed by one generic chain rule, and constancy is the textbook definition made executable: T is constant w.r.t. x if T does not contain x &mdash; a containment recursion plus negation-as-failure.`,
      },
      {
        id: "5.8",
        label: "Differentiate",
        requires: ["5.7"],
        command:
          "x ~ symvar\nc ~ symconst\n(x + c) diffby x\n(x * x) diffby x\n((x + c) diffby x) = D\n((x * x) diffby x) = D",
        info: `d(x+c)/dx answers 1: the constant vanishes via negation-as-failure, and the raw 1&nbsp;+&nbsp;0 is folded away because every derivative is pushed through the simplifier. The product rule assembles d(x&middot;x)/dx, whose neutral factors simplify to (x&nbsp;+&nbsp;x). Every step is an ordinary deduction with provenance.`,
      },
      {
        id: "5.9",
        label: "Load EML",
        requires: ["5.4"],
        command: ".import eml",
        info: `Odrzywołek (2026) showed that a single operator eml(x,&nbsp;y)&nbsp;=&nbsp;exp(x)&nbsp;&minus;&nbsp;ln(y), together with the constant 1, generates all elementary functions &mdash; a Sheffer stroke for continuous mathematics (<a href="https://arxiv.org/abs/2603.21852" target="_blank">arXiv:2603.21852</a>). This module wires eml into the simplifier as an ordinary binary operator with a three-rule identity table.`,
      },
      {
        id: "5.10",
        label: "Derive Eq. (5)",
        requires: ["5.9"],
        command:
          "x ~ symvar\n:simplify (&1 eml ((&1 eml x) eml &1))\n(:simplify (&1 eml ((&1 eml x) eml &1))) = X",
        info: `Submits the tree of the paper's key identity ln&nbsp;z&nbsp;=&nbsp;eml(1,&nbsp;eml(eml(1,&nbsp;z),&nbsp;1)) to the simplifier. The engine derives <code>(ln of x)</code> as a chain of ordinary deductions &mdash; across two strata of the negation schedule &mdash; with the full provenance attached. The identity is not checked numerically and not assumed: it is derived.`,
      },
    ],
  },
];
