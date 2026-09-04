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

The packages run these tests when a user installs zelph, and not everything in the suite belongs there. A case that takes more than roughly 0.2 seconds is marked:

```cpp
TEST_CASE("jacobian: det J_G is the constant -512 as a polynomial identity" * doctest::test_suite("slow"))
```

The packages then run `zelph_tests --test-suite-exclude=slow`, which today is 673 of 762 cases in about 20 seconds instead of nearly three minutes. One case of every test file always stays unmarked, even where it is expensive, because the selection has to keep every area represented rather than only the cheap ones.

Forgetting the marker is caught rather than shipped: `ctest` refuses a selection that exceeds its declared budget, that lets a single case take more than a quarter of it, or that has shrunk below four fifths of all cases.

## Documentation

New or changed functionality must be reflected in the mkdocs documentation
under `mkdocs/docs` — new pages are welcome. For a new `.` command the
minimum is an entry in the command list in the [Quick Start
Guide](https://acrion.github.io/zelph/quickstart/) _and_ the built-in help
text in `command_executor.cpp`; the two must not drift apart.

Every documentation link in this file targets the build that follows `main`,
because that is the tree you are working against. The release site is
[zelph.org](https://zelph.org/).

Build the documentation before you open a pull request. CI runs the same
command with `--strict`, so a dead cross-reference or a broken anchor fails the
build rather than reaching the site.

```bash
pip install -r mkdocs/requirements.txt
cd mkdocs
mkdocs serve --livereload      # http://127.0.0.1:8000
mkdocs build --strict          # what CI does
```

Pass `--livereload` explicitly. With mkdocs 1.6.1 the flag defaults to off, the
server then builds once and serves that build for the rest of the session, and no
amount of reloading in the browser helps because it is the server that is stale.
The line `Watching paths for changes` in the startup output is what tells you the
watcher is running.

The playground under `/play` is not part of `docs/`; CI grafts it onto the
built site afterwards. A local build therefore has no `/play`, and the links to
it on the home page and the Playground page do not resolve. That is expected.

## Code Organization and Style

If a change adds a substantial amount of code, consider a separate source file instead of
growing an existing one — small, single-purpose files are preferred.

Every C++ change is formatted using the repository’s `.clang-format` before it
is committed. The style is settled and is not something that requires review to
spend time on.

```bash
dev_scripts/clang-format.sh     # or clang-format.nu, for nushell
```

Both format every tracked `.cpp`, `.hpp` and `.h` in place. There is no
formatting check in CI, because the output of clang-format differs between major
versions and a gate would fail on the version rather than on the change. Running
the script is therefore your responsibility.

## Editor setup

`compile_commands.json` at the repository root is a symlink into
`build-release/`. That file is what `clangd` reads, and `clangd` is what gives an
editor working completion, jump-to-definition and diagnostics across the project
– Zed, VS Code, Neovim and the other language-server clients all go through it.
The symlink dangles until you have configured a release build once, because the
compilation database is written by CMake into the build directory:

```bash
cmake -D CMAKE_BUILD_TYPE=Release -B build-release .
```

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
- `dev_scripts/clang-format.sh` has been run.
- Documentation is updated (mkdocs; for new commands: quickstart list and
  built-in help).
- The change is focused; unrelated improvements are split out.
- For performance-relevant changes: before/after measurement per the
  measurement page, hard invariants bit-identical.
- Code, comments, and output are in English.
