# Contributing to zelph

Thank you for considering a contribution. zelph is a compact codebase with an
unusually strict correctness contract — an inference engine must not drift
semantically under change — so this document is less about process and more
about the handful of expectations that keep the engine trustworthy.

## Licensing of contributions

zelph is dual-licensed: it is available under the AGPL-3.0-or-later,
and acrion innovations GmbH additionally offers it under a commercial
license (see https://acrion.ch/sales).

By submitting a contribution to this repository, you agree that your
contribution is licensed under the AGPL-3.0-or-later, and you grant
acrion innovations GmbH the right to also distribute it under its
commercial license. You retain the copyright to your contribution —
this is a license grant, not a copyright transfer. Attribution is
preserved through the git history and, where appropriate, in the
documentation.

## Before You Start

If your change touches `src/lib/network`, read the
contributor documentation first: [Performance
Architecture](https://acrion.github.io/zelph/internals/performance/) explains
the engine's invariants and their soundness arguments, and [Measurement
Methodology](https://acrion.github.io/zelph/internals/measurement/) explains
how changes there are validated.

## Tests

Every piece of new or changed functionality comes with tests — either as new
cases in the existing `src/test` files or, for a distinct area, in a new test
file. Follow the house convention of commenting _why_ a test pins what
it pins — the test comments are treated as primary sources. The full suite
must be green; note that it permanently runs the reasoner in `.semi-naive
check` mode, so completeness regressions in the evaluation machinery fail
loudly rather than silently.

## Documentation

New or changed functionality must be reflected in the mkdocs documentation
under `mkdocs/docs` — new pages are welcome. For a new `.` command the
minimum is an entry in the command list in the [Quick Start
Guide](https://acrion.github.io/zelph/quickstart/) _and_ the built-in help
text in `command_executor.cpp`; the two must not drift apart.

## Code Organization and Style

If a change adds a substantial amount of code, consider a separate source file instead of
growing an existing one — small, single-purpose files are preferred.

Two hard rules from the engine internals break the build or deadlock the
process when violated: `zelph_impl.hpp` is included only by `zelph.cpp`, and
code running under a live `Network::ReadScope` must never write to the
network, take another network lock, or call the locking API (`get_right`,
`check_fact`, `format`, `log`, output streams, …). Details and rationale:
[ReadScope](https://acrion.github.io/zelph/internals/performance/#readscope-one-lock-pair-per-read-region).

## Dependencies

If a helper already exists as an established external library, use the
library rather than reimplementing it. `src/lib/CMakeLists.txt` contains
examples of how dependencies are integrated into the build.

## Performance-Relevant Changes

If a change can affect performance — anything touching `src/lib/network`
qualifies by default — measure before and after following the [Measurement
Methodology](https://acrion.github.io/zelph/internals/measurement/): the
fresh-REPL reference protocol, the `.prof` counter comparison, and timing
bands rather than single runs. The hard semantic invariants listed there must
stay bit-identical. Stating your prediction and the measured result in the
pull-request description is appreciated — it is how the engine itself was
developed.

## Pull Request Checklist

- The test suite is green, including the tests added for this change.
- Documentation is updated (mkdocs; for new commands: quickstart list and
  built-in help).
- The change is focused; unrelated improvements are split out.
- For performance-relevant changes: before/after measurement per the
  measurement page, hard invariants bit-identical.
- Code, comments, and output are in English.
