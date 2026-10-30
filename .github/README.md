# PostgreSQL with version control

This is a fork of [PostgreSQL](https://github.com/postgres/postgres) that
adds version control with [Dolt](https://github.com/dolthub/dolt)'s
semantics: branches, commits, diffs and merges of a database's tables and
schema, inside Postgres itself.  The engine lives below Postgres's
extension seams (table access methods, WAL resource managers, hooks), with
small core patches where correctness needs them, and the fork merges each
new upstream release.

- **Design:** [`SPEC.md`](https://github.com/coffeegoddd/postgres2/blob/master/src/prolly/SPEC.md)
  and [`ARCHITECTURE.md`](https://github.com/coffeegoddd/postgres2/blob/master/src/prolly/ARCHITECTURE.md)
  on coffeegoddd/postgres2, the prototype.
- **Roadmap:** the [`roadmap` issues](https://github.com/coffeegoddd/postgres/issues?q=label%3Aroadmap),
  one per layer, built bottom up.
- **Rules for changes:** [`CONTRIBUTING-prolly.md`](../CONTRIBUTING-prolly.md):
  conventions, where code lives, the patch inventory, upstream merges and
  stacked pull requests.

Upstream's own README is [`README.md`](../README.md).

## Setting up a checkout

    git clone https://github.com/coffeegoddd/postgres.git
    cd postgres
    git remote add postgres https://github.com/postgres/postgres.git
    gh repo set-default coffeegoddd/postgres
    git config remote.pushDefault origin

The remote for postgres/postgres is named `postgres`, never `upstream`
(`gh` would then act on postgres/postgres).  `gh repo set-default` makes
`gh` open pull requests and issues here; still pass
`-R coffeegoddd/postgres` to `gh pr` and `gh issue` in scripts.
`remote.pushDefault` tells `gh stack` which remote to push to.

## Building

Build outside the source tree, with the options CI uses.
`--enable-depend` is required: without it, a changed header leaves stale
objects behind.

    mkdir ../build && cd ../build
    ../postgres/configure --prefix="$PWD/inst" \
        --enable-depend --enable-cassert --enable-debug \
        --enable-tap-tests --enable-injection-points \
        --with-openssl --with-lz4
    make -s -j"$(nproc)"

On Debian or Ubuntu the dependencies are `bison flex libicu-dev
libipc-run-perl liblz4-dev libreadline-dev libssl-dev pkg-config
zlib1g-dev`.

The tree also builds with meson (`meson setup build && ninja -C build`);
CI builds both.

## Running the tests

From the build directory:

    make -C src/test/modules/test_prolly check  # engine unit tests and interface conformance
    make -C src/test/prolly check               # the requirements suite
    make -C src/test/prolly check-heap          # its Postgres-semantics tests on heap
    make check                                  # Postgres's own regression tests

Run the tests a change affects, not whole suites: CI runs everything on
every pull request, and nightly adds sanitizer builds and `check-world`.

- `src/test/modules/test_prolly` runs C tests inside a backend:
  `test_prolly_unit_tests()` runs the unit tests, and
  `test_prolly_conformance()` runs each interface's conformance suite
  against every implementation registered under it.  Add a unit-test file
  to its `Makefile`, `meson.build` and the suite list in `test_prolly.c`.
- `src/test/prolly` runs regression, isolation and TAP tests against a
  server on the table access method named by `PROLLY_AM`; see its
  [`README`](../src/test/prolly/README).

The patch inventory check and the sync script's tests run without a build:

    git fetch postgres master
    src/tools/prolly/check_patch_inventory.sh
    src/tools/prolly/test_check_patch_inventory.sh
    src/tools/prolly/test_upstream_sync.sh /tmp/sync-test   # any directory that does not exist yet

## Opening a pull request

Branch from an up-to-date `master`, commit with explicit paths
(`git add <files>`, never a whole test directory), push, and open the
pull request against `master`.  Work split into several pull requests is
one `gh stack`:

    git fetch origin && git branch -f master origin/master
    gh stack init topic/first
    git add <files> && git commit
    gh stack add topic/second
    git add <files> && git commit
    gh stack submit --auto --open

The `prolly` workflow runs on every pull request, including each layer of
a stack, and must pass before merging.  See
[`CONTRIBUTING-prolly.md`](../CONTRIBUTING-prolly.md) for changing a lower
layer and for upstream sync pull requests, which are merged as merge
commits.
