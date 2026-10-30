# Contributing to the fork

This fork of PostgreSQL adds Dolt-style version control: branches, commits,
diffs and merges of a database's tables and schema, with Dolt's semantics.
It carries its changes as patches and extensions on top of
postgres/postgres and merges each new upstream release into them.  These
are the rules every change follows.  [`.github/README.md`](.github/README.md)
covers building and testing.

## Conventions

- **Interface, then implementation.**  Every component sits behind an
  interface: a struct of callbacks in the style of `TableAmRoutine`, so an
  implementation can be swapped later.  Callers include only the interface
  header, never an implementation's.  Each implementation registers its
  routine struct by name (`prolly/registry.h`), and each interface has a
  conformance suite in `src/test/modules/test_prolly` that every registered
  implementation runs.
- **Every bug gets a test.**  Each bug found or hit ships with a test that
  fails without the fix.  Confirm it by breaking the fix on purpose and
  watching the test fail, then restore the fix (and `touch` the restored
  file, so `make` rebuilds it).  Requirement-level tests go in
  `src/test/prolly`; tests of one interface or one implementation go in
  `src/test/modules/test_prolly`.
- **Code never refers to specs.**  Code and comments do not cite the
  design documents, issue numbers, roadmap layers or steps; a comment
  explains its reason itself, so it stays true when the documents move on.
  Issues, commit messages and pull request descriptions may cite them.
- **Stacked pull requests use `gh stack`.**  A piece of work split into
  several pull requests is one stack (see below).
- **Targeted tests locally, full suites in CI.**  Run only the tests a
  change affects, plus its break test, and push without waiting on local
  runs.  CI runs everything; it gates only the final merge.
- **Local builds use `--enable-depend`**, or a header change leaves stale
  objects behind.
- **Core patches are small, isolated and inventoried** (below).  The engine
  stays below Postgres's extension seams (table and index access methods,
  WAL resource managers, hooks) as far as it can, and patches the core
  only where a feature cannot be correct otherwise.

Write as the surrounding Postgres code does: its formatting (pgindent), its
error-reporting style, complete sentences in comments.

## Where code lives

| What | Where |
|---|---|
| Core patches | In place, in the upstream file they change, each listed in the patch inventory |
| The engine's code compiled into the server | `src/backend/prolly/`, headers in `src/include/prolly/` |
| Extensions | `contrib/prolly/<name>/`, listed in `contrib/prolly/Makefile` and `contrib/prolly/meson.build` |
| Requirement-level tests | `src/test/prolly/` |
| Interface and implementation tests | `src/test/modules/test_prolly/` |
| Benchmarks | `src/tools/prolly_bench/` |
| The fork's tooling (sync, inventory) | `src/tools/prolly/` |
| CI | `.github/workflows/prolly*.yml` |

Every directory above already has its build rules (Makefile and
meson.build), so new code adds files there and touches no upstream build
file.  A new subdirectory of `src/backend/prolly` is listed in that
directory's own Makefile and meson.build.

## The patch inventory

[`src/tools/prolly/patch_inventory`](src/tools/prolly/patch_inventory)
lists the fork's own paths (`own`) and every core file it changes
(`patch`), each with the roadmap layer that needs the change and a
one-line reason.  Merging an upstream release means re-examining each
listed file, which is why the list must be complete.

CI runs `src/tools/prolly/check_patch_inventory.sh`, which diffs the tree
against its merge base with postgres/master and fails when a changed core
file is not listed, or a listed one no longer differs.  Run it locally
before pushing a change to a core file:

    git fetch postgres master
    src/tools/prolly/check_patch_inventory.sh

A new top-level file or directory of the fork's own gets an `own` line.

## Merging upstream

The fork takes upstream's changes by merging postgres/postgres master,
never by rebasing, so fork commits keep their identity and each merge
shows what upstream brought.

- The remote for postgres/postgres is named **`postgres`**.  Never name it
  `upstream`: `gh` treats a remote named `upstream` as the repository to
  act on, and would open pull requests on postgres/postgres.
- Each sync is a branch named `upstream-sync-YYYY-MM-DD` and an ordinary
  pull request against `master`.
- A sync pull request is merged as a **merge commit**, never squashed or
  rebased: either would rewrite upstream's commits and break the next
  sync's merge base.

`src/tools/prolly/upstream_sync.sh` does all of this: it fetches, creates
the branch from `origin/master`, merges `postgres/master`, and resolves the
one conflict that recurs on its own, `CATALOG_VERSION_NO` in
`src/include/catalog/catversion.h` changed on both sides, by bumping past
both.  With `--push` it pushes the branch and opens the pull request.  Any
other conflict stops it with the list of files; resolve them by hand on a
branch of the same name, commit the merge, push, and open the pull request
with `gh pr create -R coffeegoddd/postgres`.

The `prolly upstream sync` workflow runs the script every Monday and opens
the pull request, or files an issue when the merge needs a person.

## Stacked pull requests

A piece of work too large for one review is a stack of pull requests, one
concern per layer, made with [`gh stack`](https://github.com/github/gh-stack):

    gh stack init layer0/first          # the bottom branch, based on master
    git add <paths> && git commit
    gh stack add layer0/second          # the next layer, based on the one below
    git add <paths> && git commit
    gh stack submit --auto --open       # push every branch and open its pull request
    gh stack view --json                # the stack and its pull requests

To change a lower layer, check it out (`gh stack down` or
`gh stack checkout <branch>`), commit, then `gh stack rebase --upstack` and
`gh stack push`.  Never delete a stacked branch by hand; `gh stack merge`
and `gh stack sync --prune` retire merged layers.

`gh stack` bases a stack on the local `master` branch, so keep it equal to
`origin/master` (`git fetch origin && git branch -f master origin/master`,
or `git pull` on master) before starting a stack.

Use `git add` with explicit paths.  Test directories hold regression
outputs (`results/`, `output_iso/`, `regression.diffs`) that a wholesale
`git add` would commit.
