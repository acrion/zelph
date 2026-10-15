// Copyright (c) 2025, 2026 acrion innovations GmbH
// Authors: Stefan Zipproth, s.zipproth@acrion.ch
//
// This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org
//
// zelph is offered under a commercial and under the AGPL license.
// For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.
//
// AGPL licensing:
//
// zelph is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// zelph is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with zelph. If not, see <https://www.gnu.org/licenses/>.

//! The safe API, exercised from outside the crate.
//!
//! Cargo runs the tests of one binary on several threads, and there can only
//! be one engine in a process - so every test here takes the same lock and
//! holds it for its whole body. That is a property of the thing under test,
//! not a workaround: an engine is a process-wide resource.

use std::sync::{Arc, Mutex, MutexGuard, OnceLock};

use zelph::{Channel, Engine, ErrorKind, Node};

fn engine_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    // A failing test poisons the lock; the next one still needs to run, and
    // the engine it creates is a fresh one either way.
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// An engine that says nothing. `.load` and `.save` report progress, and a
/// test run that prints it is unreadable.
fn silent_engine() -> Engine {
    Engine::with_output(|_, _, _| {}).expect("engine")
}

/// The neurons of a layer, declared the way a script would: `(name in layer)`.
fn layer(engine: &Engine, name: &str, neurons: &[&str]) -> (Node, Vec<Node>) {
    let layer = engine.resolve(name).unwrap();
    let part_of = engine.resolve("in").unwrap();

    let nodes = neurons
        .iter()
        .map(|neuron| {
            let node = engine.resolve(neuron).unwrap();
            engine.fact(node, part_of, &[layer]).unwrap();
            node
        })
        .collect();

    (layer, nodes)
}

#[test]
fn a_name_resolves_to_one_stable_node_and_back() {
    let _guard = engine_lock();
    let z = silent_engine();

    let socrates = z.resolve("Socrates").unwrap();
    assert_eq!(socrates, z.resolve("Socrates").unwrap());
    assert_ne!(socrates, z.resolve("Plato").unwrap());
    assert!(!socrates.is_null());

    assert_eq!(z.name(socrates).unwrap().as_deref(), Some("Socrates"));

    // A fact has no name, and that is an answer rather than a failure.
    let is_a = z.resolve("~").unwrap();
    let mortal = z.resolve("mortal").unwrap();
    let fact = z.fact(socrates, is_a, &[mortal]).unwrap();
    assert!(!fact.is_null());
    assert_eq!(z.name(fact).unwrap(), None);
}

#[test]
fn structurally_identical_lists_are_one_node() {
    let _guard = engine_lock();
    let z = silent_engine();

    let x = z.resolve("x").unwrap();
    let y = z.resolve("y").unwrap();

    let list = z.list(&[x, y]).unwrap();
    assert_eq!(list, z.list(&[x, y]).unwrap());
    assert_ne!(list, z.list(&[y, x]).unwrap());

    // The empty list IS the nil node - "no elements", not "no node".
    let empty = z.list(&[]).unwrap();
    assert!(!empty.is_null());
    assert_eq!(z.name(empty).unwrap().as_deref(), Some("nil"));
}

#[test]
fn a_fact_reads_back_as_its_parts() {
    let _guard = engine_lock();
    let z = silent_engine();

    let socrates = z.resolve("Socrates").unwrap();
    let is = z.resolve("is").unwrap();
    let mortal = z.resolve("mortal").unwrap();

    let fact = z.fact(socrates, is, &[mortal]).unwrap();
    let (subject, predicate, objects) = z.fact_parts(fact).unwrap();
    assert_eq!(subject, socrates);
    assert_eq!(predicate, is);
    assert_eq!(objects, vec![mortal]);

    // A cons cell is the fact (car cons cdr), so the two readers meet here.
    let list = z.list(&[socrates, mortal]).unwrap();
    let (car, _, tail) = z.fact_parts(list).unwrap();
    assert_eq!(car, socrates);
    assert_eq!(z.elements(tail[0]).unwrap(), vec![mortal]);

    // A name is not a statement.
    assert!(z.fact_parts(socrates).is_err());
}

// A mention is a statement a program writes down without asserting it,
// something the C ABI was unable to express: `fact` claims. It has to
// remain absent from every answer until a claim of the identical
// statement appears, and that claim must transform it into data.
#[test]
fn a_mention_is_data_only_once_it_is_claimed() {
    let _guard = engine_lock();
    let z = silent_engine();

    let mary = z.resolve("mary").unwrap();
    let waits = z.resolve("waits").unwrap();
    let nil = z.resolve("nil").unwrap();

    let mentioned = z.mention(mary, waits, &[nil]).unwrap();
    assert!(!mentioned.is_null());
    assert!(!z.exists(mary, waits, &[nil]).unwrap());

    assert_eq!(z.fact(mary, waits, &[nil]).unwrap(), mentioned);
    assert!(z.exists(mary, waits, &[nil]).unwrap());

    // A mention of what is asserted keeps it asserted.
    assert_eq!(z.mention(mary, waits, &[nil]).unwrap(), mentioned);
    assert!(z.exists(mary, waits, &[nil]).unwrap());
}

// A rule expressed through a mention is a mention just like any other
// statement written in this way, but it was in force the moment `mention`
// returned it, since nothing held it. A claim of the same rule via `fact`
// puts it in force.
#[test]
fn a_rule_written_as_a_mention_is_in_force_only_once_it_is_claimed() {
    let _guard = engine_lock();
    let z = silent_engine();

    let [a, p, b, c, q, d, causes] =
        ["a", "p", "b", "c", "q", "d", "=>"].map(|name| z.resolve(name).unwrap());
    let condition = z.mention(a, p, &[b]).unwrap();
    let consequence = z.mention(c, q, &[d]).unwrap();
    let rule = z.mention(condition, causes, &[consequence]).unwrap();

    z.fact(a, p, &[b]).unwrap();
    z.run().unwrap();
    assert!(!z.exists(c, q, &[d]).unwrap());

    assert_eq!(z.fact(condition, causes, &[consequence]).unwrap(), rule);
    z.run().unwrap();
    assert!(z.exists(c, q, &[d]).unwrap());
}

#[test]
fn a_list_reads_back_as_what_it_was_built_from() {
    let _guard = engine_lock();
    let z = silent_engine();

    let x = z.resolve("x").unwrap();
    let y = z.resolve("y").unwrap();
    let w = z.resolve("w").unwrap();

    let inner = z.list(&[x, y]).unwrap();
    assert_eq!(z.elements(inner).unwrap(), vec![x, y]);

    // Order, not membership: it is the distinction between a list and a
    // set, and the reason a list can designate something.
    assert_ne!(z.elements(z.list(&[y, x]).unwrap()).unwrap(), vec![x, y]);

    // An element is a node like any other, so nesting results in nesting.
    let outer = z.list(&[inner, w]).unwrap();
    let read = z.elements(outer).unwrap();
    assert_eq!(read, vec![inner, w]);
    assert_eq!(z.elements(read[0]).unwrap(), vec![x, y]);

    assert!(z.elements(z.list(&[]).unwrap()).unwrap().is_empty());

    // A name isn’t a list of one. Reading a non-list results in an error,
    // so a caller can tell a structure it composed from a node that
    // simply exists.
    assert!(z.elements(x).is_err());
}

#[test]
fn sources_are_directional_and_may_be_empty() {
    let _guard = engine_lock();
    let z = silent_engine();

    let (l, members) = layer(&z, "L", &["a1", "a2", "a3"]);
    let part_of = z.resolve("in").unwrap();

    let mut found = z.sources(part_of, l).unwrap();
    found.sort();
    let mut expected = members.clone();
    expected.sort();
    assert_eq!(found, expected);

    // (L in M) makes L a subject, not a source of itself.
    let m = z.resolve("M").unwrap();
    z.fact(l, part_of, &[m]).unwrap();
    assert_eq!(z.sources(part_of, l).unwrap().len(), 3);

    // Nothing to report is Ok(empty), never an error.
    let unrelated = z.resolve("Unrelated").unwrap();
    assert!(z.sources(part_of, unrelated).unwrap().is_empty());
}

#[test]
fn a_network_is_wired_compiled_trained_and_read_back() {
    let _guard = engine_lock();
    let z = silent_engine();

    let (input, inputs) = layer(&z, "In", &["i1", "i2"]);
    let (output, outputs) = layer(&z, "Out", &["o1", "o2"]);

    assert_eq!(z.connect_layers(input, output, 0.0, 1).unwrap(), 4);
    // Idempotent: re-wiring never overwrites what training produced.
    assert_eq!(z.connect_layers(input, output, 0.5, 1).unwrap(), 0);

    let net = z.compile(&[input, output]).unwrap();

    for _ in 0..60 {
        net.train(&inputs[0..1], &outputs[0..1], 0.5).unwrap();
        net.train(&inputs[1..2], &outputs[1..2], 0.5).unwrap();
    }

    let (best, score) = net.best(&inputs[0..1]).unwrap().expect("an output");
    assert_eq!(best, outputs[0]);
    assert!(score > 0.5);

    // The whole output layer, sorted by descending score.
    let all = net.eval(&inputs[1..2], None).unwrap();
    assert_eq!(all.len(), 2);
    assert_eq!(all[0].0, outputs[1]);
    assert!(all[0].1 >= all[1].1);

    // top_k caps the result.
    assert_eq!(net.eval(&inputs[1..2], Some(1)).unwrap().len(), 1);

    // A graded input scales the (linear) response.
    let full = net.best(&inputs[1..2]).unwrap().unwrap().1;
    let half = net
        .eval_graded(&inputs[1..2], &[0.5], Some(1))
        .unwrap()
        .remove(0)
        .1;
    assert!((2.0 * half - full).abs() < 1e-9);

    // A mismatch between nodes and activations is caught in Rust, before the
    // boundary.
    let err = net
        .eval_graded(&inputs[0..2], &[1.0], Some(1))
        .expect_err("length mismatch");
    assert_eq!(err.kind(), ErrorKind::InvalidArgument);
}

#[test]
fn a_snapshot_restores_the_weights_it_was_taken_from() {
    let _guard = engine_lock();
    let z = silent_engine();

    let (input, inputs) = layer(&z, "In", &["i1"]);
    let (output, outputs) = layer(&z, "Out", &["o1"]);
    z.connect_layers(input, output, 0.1, 7).unwrap();

    let net = z.compile(&[input, output]).unwrap();

    let before = net.snapshot().unwrap();
    assert_eq!(before.shape().len(), 1); // one matrix between two layers
    assert_eq!(before.weights().len(), before.shape().iter().sum::<usize>());

    let score_before = net.best(&inputs).unwrap().unwrap().1;

    for _ in 0..20 {
        net.train(&inputs, &outputs, 0.5).unwrap();
    }
    let trained = net.snapshot().unwrap();
    assert_ne!(trained, before);

    net.restore(&before).unwrap();
    let score_restored = net.best(&inputs).unwrap().unwrap().1;
    assert!((score_restored - score_before).abs() < 1e-12);
    assert_eq!(net.snapshot().unwrap(), before);
}

#[test]
fn weights_survive_save_and_load_and_nodes_keep_their_identity() {
    let _guard = engine_lock();

    let path = std::env::temp_dir().join("zelph_rust_roundtrip.bin");
    let _ = std::fs::remove_file(&path);

    let (saved_output, saved_score) = {
        let z = silent_engine();
        let (input, inputs) = layer(&z, "In", &["i1"]);
        let (output, outputs) = layer(&z, "Out", &["o1"]);
        z.connect_layers(input, output, 0.0, 1).unwrap();

        let net = z.compile(&[input, output]).unwrap();
        for _ in 0..40 {
            net.train(&inputs, &outputs, 0.5).unwrap();
        }

        // Without this the graph knows nothing of the training, and what is
        // saved is the untrained net.
        net.write_back().unwrap();
        let (node, score) = net.best(&inputs).unwrap().unwrap();

        z.save(&path).unwrap();
        (node, score)
    };

    assert!(path.exists());

    {
        let z = silent_engine();
        z.load(&path).unwrap();

        // A node is its hash, so a name resolved in a fresh engine IS the node
        // the file talks about. Nothing has to be mapped or remembered.
        assert_eq!(z.resolve("o1").unwrap(), saved_output);

        let input = z.resolve("In").unwrap();
        let output = z.resolve("Out").unwrap();
        let i1 = z.resolve("i1").unwrap();

        let net = z.compile(&[input, output]).unwrap();
        let (node, score) = net.best(&[i1]).unwrap().unwrap();
        assert_eq!(node, saved_output);
        assert!((score - saved_score).abs() < 1e-12);
    }

    let _ = std::fs::remove_file(&path);
}

// A ground marking `(F ~ refuted)` constitutes a refutation, and `fact`
// refused to write it after having silently taken a claimed fact from
// every query; a caller must use `refute` to say it instead. Refused for a
// claimed fact, as `¬(F)` is.
#[test]
fn a_refutation_is_stated_with_refute_and_refused_for_a_claimed_fact() {
    let _guard = engine_lock();
    let z = silent_engine();

    let socrates = z.resolve("Socrates").unwrap();
    let is = z.resolve("is").unwrap();
    let mortal = z.resolve("mortal").unwrap();
    let immortal = z.resolve("immortal").unwrap();
    let is_a = z.resolve("~").unwrap();
    let refuted = z.resolve_in("refuted", "zelph").unwrap();

    let claimed = z.fact(socrates, is, &[mortal]).unwrap();
    let err = z
        .fact(claimed, is_a, &[refuted])
        .expect_err("a ground marking is a refutation");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err.message().contains("zelph_refute"));

    let err = z
        .refute(socrates, is, &[mortal])
        .expect_err("a claimed fact cannot be refuted");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err.message().contains("known to be true"));
    assert!(z.exists(socrates, is, &[mortal]).unwrap());
    let err = z
        .refute(claimed, is_a, &[refuted])
        .expect_err("a refutation marking cannot be refuted");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err.message().contains("cannot itself be refuted"));
    assert!(z.exists(socrates, is, &[mortal]).unwrap());

    let denied = z.refute(socrates, is, &[immortal]).unwrap();
    assert!(!denied.is_null());
    assert!(!z.exists(socrates, is, &[immortal]).unwrap());
    let err = z
        .fact(socrates, is, &[immortal])
        .expect_err("a refuted fact cannot be asserted");
    assert!(err.message().contains("known to be wrong"));
}

#[test]
fn failures_arrive_as_errors_with_a_kind_and_a_message() {
    let _guard = engine_lock();
    let z = silent_engine();

    let s = z.resolve("s").unwrap();
    let p = z.resolve("p").unwrap();

    let err = z.fact(s, p, &[]).expect_err("a fact needs an object");
    assert_eq!(err.kind(), ErrorKind::InvalidArgument);
    assert!(!err.message().is_empty());

    // A layer with no members: the C++ side throws, and the caller gets a
    // value rather than an unwind through a foreign frame.
    let empty_in = z.resolve("EmptyIn").unwrap();
    let empty_out = z.resolve("EmptyOut").unwrap();
    let err = z
        .compile(&[empty_in, empty_out])
        .expect_err("no members to compile");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err.message().contains("no members"));

    // .save validates its extension, and the refusal reaches the caller.
    assert!(z.save("not-a-binary.txt").is_err());

    // A NUL byte cannot cross into C, and is rejected before it tries.
    assert_eq!(
        z.resolve("a\0b").expect_err("NUL").kind(),
        ErrorKind::InvalidArgument
    );
}

#[test]
fn only_one_engine_exists_at_a_time() {
    let _guard = engine_lock();

    let first = silent_engine();
    let second = Engine::new().expect_err("a second engine");
    assert_eq!(second.kind(), ErrorKind::Runtime);

    drop(first);

    // The refusal is not permanent.
    drop(silent_engine());
}

#[test]
fn the_output_callback_receives_what_the_engine_emits() {
    let _guard = engine_lock();

    let lines: Arc<Mutex<Vec<(Channel, String)>>> = Arc::new(Mutex::new(Vec::new()));
    let sink = Arc::clone(&lines);

    let z = Engine::with_output(move |channel, text, _newline| {
        sink.lock().unwrap().push((channel, text.to_string()));
    })
    .unwrap();

    // A failing command reports on the error channel; the point is that the
    // report reaches the host instead of the process's stdout, which for a
    // host speaking its own protocol would be a protocol error.
    let _ = z.load("does-not-exist.bin");

    let seen = lines.lock().unwrap();
    assert!(
        !seen.is_empty(),
        "the engine said nothing at all, which means the callback is not wired"
    );
}

#[test]
fn a_net_is_evaluated_from_several_threads_while_another_trains_it() {
    let _guard = engine_lock();
    let z = silent_engine();

    let (input, inputs) = layer(&z, "In", &["i1", "i2"]);
    let (output, outputs) = layer(&z, "Out", &["o1", "o2"]);
    z.connect_layers(input, output, 0.1, 3).unwrap();

    let net = z.compile(&[input, output]).unwrap();
    let stop = std::sync::atomic::AtomicBool::new(false);

    // That this compiles at all is half the test: `&Net` crosses a thread
    // boundary, which the type system permits only because Net is Sync -
    // the compiler checking a guarantee that used to be a comment in C++.
    std::thread::scope(|scope| {
        for _ in 0..3 {
            scope.spawn(|| {
                while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                    let (node, score) = net.best(&inputs[0..1]).unwrap().expect("an output");
                    assert!(!node.is_null());
                    assert!(score.is_finite());
                }
            });
        }

        for _ in 0..2000 {
            net.train(&inputs[0..1], &outputs[0..1], 0.05).unwrap();
        }
        stop.store(true, std::sync::atomic::Ordering::Relaxed);
    });

    // After training, the trained association is the one that wins.
    assert_eq!(net.best(&inputs[0..1]).unwrap().unwrap().0, outputs[0]);
}

// ---------------------------------------------------------------------------
// The reasoning surface: the half of zelph that is the reason it exists.
// ---------------------------------------------------------------------------

#[test]
fn a_rule_derives_what_forward_chaining_makes_of_it() {
    let _guard = engine_lock();
    let z = silent_engine();

    let is_a = z.resolve("~").unwrap();
    let human = z.resolve("human").unwrap();
    let mortal = z.resolve("mortal").unwrap();

    let socrates = z.resolve("socrates").unwrap();
    z.fact(socrates, is_a, &[human]).unwrap();

    // A variable is a node the caller holds, so an answer can name it without
    // a string crossing the boundary.
    let x = z.variable("X").unwrap();
    assert_eq!(x, z.variable("X").unwrap());

    let condition = z.fact(x, is_a, &[human]).unwrap();
    let consequence = z.fact(x, is_a, &[mortal]).unwrap();
    z.rule(&[condition], &[consequence]).unwrap();

    // Nothing is derived until the engine runs.
    assert!(!z.exists(socrates, is_a, &[mortal]).unwrap());
    z.run().unwrap();
    assert!(z.exists(socrates, is_a, &[mortal]).unwrap());

    // A query reports one set of bindings per match, by node.
    let who = z.variable("Who").unwrap();
    let pattern = z.fact(who, is_a, &[mortal]).unwrap();
    let answers = z.query(pattern).unwrap();
    assert_eq!(answers.len(), 1);
    assert_eq!(answers[0], vec![(who, socrates)]);

    // When dealing with a positive rule like this one, the expense of a
    // delta run lies in the addition rather than the graph, which
    // determines whether reasoning is possible within a loop.
    let plato = z.resolve("plato").unwrap();
    z.fact(plato, is_a, &[human]).unwrap();
    z.run_delta().unwrap();
    assert!(z.exists(plato, is_a, &[mortal]).unwrap());
}

#[test]
fn a_rules_condition_set_takes_no_further_condition() {
    let _guard = engine_lock();
    let z = silent_engine();

    let is_a = z.resolve("~").unwrap();
    let part_of = z.resolve("in").unwrap();
    let human = z.resolve("human").unwrap();
    let greek = z.resolve("greek").unwrap();
    let mortal = z.resolve("mortal").unwrap();

    let x = z.variable("X").unwrap();
    let condition = z.fact(x, is_a, &[human]).unwrap();
    let consequence = z.fact(x, is_a, &[mortal]).unwrap();
    let conditions = z.rule(&[condition], &[consequence]).unwrap();

    // The node `rule` returns is the rule's condition set. Introducing a
    // membership into it would impose an additional condition on a rule
    // that is already active, without a word, hence the engine refuses
    // it: a rule's text remains immutable once the rule is written.
    let further = z.fact(x, is_a, &[greek]).unwrap();
    let err = z
        .fact(further, part_of, &[conditions])
        .expect_err("a rule's condition set is its text");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err.message().contains("the condition set of a rule"));

    // The rule continues to carry its single condition: Socrates is
    // not a Greek in this instance.
    let socrates = z.resolve("socrates").unwrap();
    z.fact(socrates, is_a, &[human]).unwrap();
    z.run().unwrap();
    assert!(z.exists(socrates, is_a, &[mortal]).unwrap());
}

#[test]
fn a_rule_whose_consequence_predicate_no_condition_binds_is_refused() {
    // `rule` writes what zelph_rule writes, and a consequence whose predicate
    // remains unbound by any condition is rejected in that context, just as
    // every rule builder refuses it: a firing lacks a predicate to write. A
    // predicate that a condition identifies becomes bound by each firing.
    let _guard = engine_lock();
    let z = silent_engine();

    let waits = z.resolve("waits").unwrap();
    let nil = z.resolve("nil").unwrap();
    let ok = z.resolve("ok").unwrap();
    let x = z.variable("X").unwrap();
    let w = z.variable("W").unwrap();

    let condition = z.fact(x, waits, &[nil]).unwrap();
    let unbound = z.fact(x, w, &[nil]).unwrap();
    let err = z
        .rule(&[condition], &[unbound])
        .expect_err("no condition binds W");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err
        .message()
        .contains("is the variable W, which no condition binds"));

    let bound = z.fact(x, w, &[ok]).unwrap();
    z.rule(&[unbound], &[bound]).unwrap();
    let ann = z.resolve("ann").unwrap();
    z.fact(ann, waits, &[nil]).unwrap();
    z.run().unwrap();
    assert!(z.exists(ann, waits, &[ok]).unwrap());
}

#[test]
fn a_rule_written_with_fact_whose_consequence_predicate_no_condition_binds_is_refused_by_the_next_run() {
    // `fact` writes the `=>` fact that `rule` produces, and nothing verified it
    // there: `(X waits nil) => (W W nil)` derived `-> -> nil`. The call cannot
    // reject it, since the inner rule of a generator has identical structure
    // until the generator holds it, so the next run verifies it and fails; the
    // rule is gone then, and the following run derives nothing.
    let _guard = engine_lock();
    let z = silent_engine();

    let waits = z.resolve("waits").unwrap();
    let nil = z.resolve("nil").unwrap();
    let causes = z.resolve("=>").unwrap();
    let x = z.variable("X").unwrap();
    let w = z.variable("W").unwrap();

    let condition = z.fact(x, waits, &[nil]).unwrap();
    let consequence = z.fact(w, w, &[nil]).unwrap();
    z.fact(condition, causes, &[consequence]).unwrap();
    let ann = z.resolve("ann").unwrap();
    z.fact(ann, waits, &[nil]).unwrap();

    let err = z.run().expect_err("no condition binds W");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err
        .message()
        .contains("zelph_fact: The predicate of \"W W nil\" in the rule's consequence is the variable W"));
    assert!(!z.exists(condition, causes, &[consequence]).unwrap());

    z.run().unwrap();
    let s = z.variable("S").unwrap();
    let p = z.variable("P").unwrap();
    let pattern = z.fact(s, p, &[nil]).unwrap();
    // `ann waits nil` alone: no `-> -> nil` positioned
    // next to it.
    assert_eq!(z.query(pattern).unwrap().len(), 1);
}

#[test]
fn a_rule_written_with_fact_whose_consequence_predicate_no_condition_binds_is_refused_by_the_next_save() {
    // Before the next run, `save` wrote the rule to the file unchecked, and a
    // subsequent `load` retrieved it as a rule the graph already holds, which
    // no later run checks. The `save` performs its check first, just as a
    // `run` does, and fails due to refusal; the file remains unwritten, the
    // rule is gone, and the next `save` succeeds.
    let _guard = engine_lock();
    let z = silent_engine();

    let waits = z.resolve("waits").unwrap();
    let nil = z.resolve("nil").unwrap();
    let causes = z.resolve("=>").unwrap();
    let x = z.variable("X").unwrap();
    let w = z.variable("W").unwrap();

    let condition = z.fact(x, waits, &[nil]).unwrap();
    let consequence = z.fact(w, w, &[nil]).unwrap();
    z.fact(condition, causes, &[consequence]).unwrap();

    let path = std::env::temp_dir().join("zelph_rust_fact_rule_save.bin");
    let _ = std::fs::remove_file(&path);

    let err = z.save(&path).expect_err("no condition binds W");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err
        .message()
        .contains("zelph_fact: The predicate of \"W W nil\" in the rule's consequence is the variable W"));
    assert!(!path.exists());
    assert!(!z.exists(condition, causes, &[consequence]).unwrap());

    z.save(&path).unwrap();
    assert!(path.exists());
    let _ = std::fs::remove_file(&path);
}

#[test]
fn a_mentioned_rule_that_fact_claims_into_force_is_refused_by_the_next_run_and_stays_a_mention() {
    // A rule authored by `mention` is out of force until `fact` claims it.
    // Without verification, that claim put a rule in force whose consequence
    // predicate is not constrained by any condition. The next run checks the
    // claim just as it checks a rule `fact` generates, yet the rule was
    // already present before the call: the refusal takes the claim back,
    // leaving the rule as a mere mention, which the next run does not fire and
    // which `mention` finds in place.
    let _guard = engine_lock();
    let z = silent_engine();

    let waits = z.resolve("waits").unwrap();
    let nil = z.resolve("nil").unwrap();
    let causes = z.resolve("=>").unwrap();
    let x = z.variable("X").unwrap();
    let w = z.variable("W").unwrap();

    let condition = z.mention(x, waits, &[nil]).unwrap();
    let consequence = z.mention(w, w, &[nil]).unwrap();
    let rule = z.mention(condition, causes, &[consequence]).unwrap();
    let ann = z.resolve("ann").unwrap();
    z.fact(ann, waits, &[nil]).unwrap();

    assert_eq!(z.fact(condition, causes, &[consequence]).unwrap(), rule);
    let err = z.run().expect_err("no condition binds W");
    assert_eq!(err.kind(), ErrorKind::Runtime);
    assert!(err
        .message()
        .contains("zelph_fact: The predicate of \"W W nil\" in the rule's consequence is the variable W"));

    z.run().unwrap();
    let s = z.variable("S").unwrap();
    let p = z.variable("P").unwrap();
    let pattern = z.fact(s, p, &[nil]).unwrap();
    assert_eq!(z.query(pattern).unwrap().len(), 1);
    assert_eq!(z.mention(condition, causes, &[consequence]).unwrap(), rule);
}

#[test]
fn a_rule_generator_built_from_facts_passes_the_run_and_fires() {
    // The predicate used by the inner rule of a generator originates in the
    // generator's binding, and by the time of the run, the generator holds it.
    let _guard = engine_lock();
    let z = silent_engine();

    let causes = z.resolve("=>").unwrap();
    let inverse = z.resolve("inverse").unwrap();
    let parent = z.resolve("parent").unwrap();
    let child = z.resolve("child").unwrap();
    let (r, s, a, b) = (
        z.variable("R").unwrap(),
        z.variable("S").unwrap(),
        z.variable("A").unwrap(),
        z.variable("B").unwrap(),
    );

    let inner_condition = z.fact(a, r, &[b]).unwrap();
    let inner_consequence = z.fact(b, s, &[a]).unwrap();
    let inner = z.fact(inner_condition, causes, &[inner_consequence]).unwrap();
    let condition = z.fact(r, inverse, &[s]).unwrap();
    z.fact(condition, causes, &[inner]).unwrap();

    let ann = z.resolve("ann").unwrap();
    let bob = z.resolve("bob").unwrap();
    z.fact(parent, inverse, &[child]).unwrap();
    z.fact(ann, parent, &[bob]).unwrap();
    z.run().unwrap();
    assert!(z.exists(bob, child, &[ann]).unwrap());
}

#[test]
fn negate_returns_the_negated_condition_and_the_pattern_stays_positive() {
    // `negate` wrote `~ negation` about the pattern and returned the
    // pattern, thus a rule built with it named the pattern itself - and
    // every other rule over the same node did the same, subsequently reading
    // it as negated. It now returns the fact `((pattern) ~ negation)`, which
    // a rule takes in place of the pattern.
    let _guard = engine_lock();
    let z = silent_engine();

    let go = z.resolve("go").unwrap();
    let ok = z.resolve("ok").unwrap();
    let bad = z.resolve("bad").unwrap();
    let t = z.resolve("t").unwrap();
    let k = z.resolve("k").unwrap();
    let b = z.resolve("b").unwrap();
    z.fact(t, go, &[k]).unwrap();

    let x = z.variable("X").unwrap();
    let y = z.variable("Y").unwrap();
    let pattern = z.fact(x, z.resolve("p").unwrap(), &[b]).unwrap();

    let negated = z.negate(pattern).unwrap();
    assert_ne!(negated, pattern);
    let (subject, predicate, objects) = z.fact_parts(negated).unwrap();
    assert_eq!(subject, pattern);
    assert_eq!(predicate, z.resolve("~").unwrap());
    assert_eq!(objects.len(), 1);
    assert_eq!(z.name(objects[0]).unwrap().as_deref(), Some("negation"));

    // A single rule applies to the negated condition, another rule
    // applies to the pattern directly.
    let walks = z.fact(x, go, &[y]).unwrap();
    z.rule(&[walks, negated], &[z.fact(x, ok, &[y]).unwrap()]).unwrap();
    z.rule(&[walks, pattern], &[z.fact(x, bad, &[y]).unwrap()]).unwrap();
    z.run().unwrap();

    assert!(z.exists(t, ok, &[k]).unwrap());
    assert!(!z.exists(t, bad, &[k]).unwrap());
}

#[test]
fn a_cluster_is_a_rollback() {
    let _guard = engine_lock();
    let z = silent_engine();

    let is_a = z.resolve("~").unwrap();
    let thing = z.resolve("thing").unwrap();

    // Asserted before any cluster exists, so no drop can reach it.
    let permanent = z.resolve("permanent").unwrap();
    z.fact(permanent, is_a, &[thing]).unwrap();

    assert_eq!(z.active_cluster().unwrap(), None);
    z.cluster(Some("scratch")).unwrap();
    assert_eq!(z.active_cluster().unwrap().as_deref(), Some("scratch"));

    let ephemeral = z.resolve("ephemeral").unwrap();
    z.fact(ephemeral, is_a, &[thing]).unwrap();
    assert!(z.cluster_size("scratch").unwrap().unwrap() > 0);
    assert_eq!(z.cluster_size("no-such-cluster").unwrap(), None);

    z.cluster(None).unwrap();
    assert!(z.drop_cluster("scratch").unwrap() > 0);

    assert!(!z.exists(ephemeral, is_a, &[thing]).unwrap());
    assert!(z.exists(permanent, is_a, &[thing]).unwrap());
}

// A query posed inside a cluster, which the caller subsequently drops,
// results in no residual trace, assuming the drop takes back what the query
// wrote. The pattern was the first fact of `b` as a predicate and declared it
// a relation type, and the drop took the pattern while `b ~ ->` remained
// intact.
#[test]
fn a_question_asked_in_a_dropped_cluster_takes_back_the_declaration_it_wrote() {
    let _guard = engine_lock();
    let z = silent_engine();

    let is_a = z.resolve("~").unwrap();
    let arrow = z.resolve("->").unwrap();
    let b = z.resolve("b").unwrap();
    z.fact(z.resolve("a").unwrap(), z.resolve("likes").unwrap(), &[b]).unwrap();
    assert!(!z.exists(b, is_a, &[arrow]).unwrap());

    z.cluster(Some("question")).unwrap();
    let pattern = z
        .fact(z.variable("X").unwrap(), b, &[z.variable("Y").unwrap()])
        .unwrap();
    assert!(z.query(pattern).unwrap().is_empty());
    z.cluster(None).unwrap();
    // The drop takes X, Y, the pattern and the declaration of its
    // predicate.
    assert_eq!(z.drop_cluster("question").unwrap(), 4);

    assert!(!z.exists(b, is_a, &[arrow]).unwrap());

    // The subsequent initial fact regarding `b` as a predicate
    // declares it as usual.
    z.fact(z.resolve("c").unwrap(), b, &[z.resolve("d").unwrap()]).unwrap();
    assert!(z.exists(b, is_a, &[arrow]).unwrap());
}

#[test]
fn a_cluster_name_beginning_with_two_underscores_is_refused() {
    let _guard = engine_lock();
    let z = silent_engine();

    // The engine names its own scratch clusters in this way, and it either
    // discarded a caller's cluster bearing such a name or merged it with
    // its own.
    let err = z.cluster(Some("__rule")).unwrap_err();
    assert_eq!(err.kind(), ErrorKind::InvalidArgument);
    assert_eq!(z.active_cluster().unwrap(), None);
    assert_eq!(z.cluster_size("__rule").unwrap(), None);
}

#[test]
fn sets_collections_and_the_two_directions() {
    let _guard = engine_lock();
    let z = silent_engine();

    let a = z.resolve("a").unwrap();
    let b = z.resolve("b").unwrap();

    // A set constant is identified by its members; a collection has an
    // identity of its own.
    assert_eq!(z.set(&[a, b]).unwrap(), z.set(&[a, b]).unwrap());
    assert_ne!(z.collection(&[a, b]).unwrap(), z.collection(&[a, b]).unwrap());

    let hits = z.resolve("hits").unwrap();
    z.fact(a, hits, &[b]).unwrap();

    assert_eq!(z.targets(a, hits).unwrap(), vec![b]);
    assert_eq!(z.sources(hits, b).unwrap(), vec![a]);
    assert!(z.targets(b, hits).unwrap().is_empty());
}
