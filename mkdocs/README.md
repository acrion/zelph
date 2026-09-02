# Building and publishing the zelph documentation

Two sites are built from this directory, and they are not the same site. Every
page looks alike on both, so the distinction is worth keeping in mind:
acrion.github.io follows `main`, zelph.org follows the latest release.

| site | content | how it is produced |
| --- | --- | --- |
| [acrion.github.io/zelph](https://acrion.github.io/zelph/) | current `main` | automatically, by the `docs` job in `.github/workflows/ci.yml` on every push to the default branch |
| [zelph.org](https://zelph.org/) | the latest release | by hand, from the release tag, with the steps below |

## Local preview

```bash
pip install -r requirements.txt
mkdocs serve --livereload   # http://127.0.0.1:8000
mkdocs build --strict       # what CI runs
```

Pass `--livereload` explicitly. With mkdocs 1.6.1 the flag defaults to off, the
server then builds once and serves that build for the rest of the session, and no
amount of reloading in the browser helps because it is the server that is stale.
The line `Watching paths for changes` in the startup output is what tells you the
watcher is running.

The versions in `requirements.txt` are pinned so that a local preview and a CI
run render the same site.

The playground is not included in the source tree. It is built by the `wasm` job
and grafted onto the finished site afterwards, which is why a local preview
returns 404 for every link to `/play`. That is expected, and it is the reason
`mkdocs.yml` leaves `unrecognized_links` at `info`.

## Publishing zelph.org

zelph.org is not built from this directory. It is assembled in a separate
maintainer-side repository that holds the same pages plus the published Wikidata
report trees — hundreds of megabytes of derivation output that do not belong in a
source repository. That repository synchronises `docs/`, `overrides/`,
`requirements.txt` and this directory's `mkdocs.yml` from a release checkout,
overrides only the navigation, builds, and uploads the result.

Two consequences matter here:

- **`mkdocs.yml` is copied verbatim.** A change to it reaches zelph.org
  unchanged, so it must not assume anything that only holds for GitHub Pages.
- **Image and asset references must stay relative.** `../assets/x.svg` resolves
  correctly under a subpath (`acrion.github.io/zelph/logic/`) and at a domain
  root (`zelph.org/logic/`). A root-absolute `/assets/x.svg` works only at the
  root and silently breaks the other build.

The order of the remaining steps is crucial in either case: `mkdocs build` wipes
`site/` and writes it again from scratch, so the playground must be fetched after
that step, never before.

## What reaches the published site

Anything remaining in `docs/` is copied verbatim into the published site,
whether or not git tracks it. `exclude_docs` keeps the generated report tree and
stray `.orig` files out; a merge leftover is otherwise invisible in `git status`
and still reaches the web.
