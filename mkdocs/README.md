# Building and publishing the zelph documentation

Two sites are built from this directory. Both follow `main`, effectively
representing the most recent release (refer to
[CONTRIBUTING.md](../CONTRIBUTING.md#documentation)); they differ in their
production method and in the Wikidata reports that zelph.org carries in
addition.

| site | content | how it is produced |
| --- | --- | --- |
| [acrion.github.io/zelph](https://acrion.github.io/zelph/) | current `main` | automatically, by the `docs` job in `.github/workflows/ci.yml` on every push to the default branch |
| [zelph.org](https://zelph.org/) | current `main`, plus the published Wikidata reports | by `publish.sh main` in the site repository, see below |

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
`requirements.txt` and this directory's `mkdocs.yml` from a checkout of `main`,
overrides only the navigation, builds, and uploads the result.

Two consequences matter here:

- **`mkdocs.yml` is copied verbatim.** A change to it reaches zelph.org
  unchanged, so it must not assume anything that only holds for GitHub Pages.
- **Image and asset references must stay relative.** `../assets/x.svg` resolves
  correctly under a subpath (`acrion.github.io/zelph/logic/`) and at a domain
  root (`zelph.org/logic/`). A root-absolute `/assets/x.svg` works only at the
  root and silently breaks the other build.
- **Links must stay relative too.** A page here must not link to `zelph.org`.
  Since these pages are published on both sites, such a reference within one
  would redirect a reader from one site to the other without indicating the
  shift. Opt for a relative link, one that resolves correctly on both. Two
  things are exempt: the three video sources, as those files are hosted solely
  on zelph.org and cannot be accessed otherwise, and the *Versions* note found
  at the end of `playground.md`, which explicitly mentions both hosts.

The order of the remaining steps is crucial in either case: `mkdocs build` wipes
`site/` and writes it again from scratch, so the playground must be fetched after
that step, never before.

## What reaches the published site

Anything remaining in `docs/` is copied verbatim into the published site,
whether or not git tracks it. `exclude_docs` keeps the generated report tree and
stray `.orig` files out; a merge leftover is otherwise invisible in `git status`
and still reaches the web.
