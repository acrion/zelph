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
