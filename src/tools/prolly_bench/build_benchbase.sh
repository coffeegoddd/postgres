#!/usr/bin/env bash
#
# Build BenchBase's Postgres distribution from a BenchBase checkout into
# build-bench/benchbase/, without touching the checkout: the source is
# copied (without .git and target/) and built there.  Records the source
# commit in build-bench/benchbase/BUILD_COMMIT.
#
# Usage: build_benchbase.sh [BENCHBASE_CHECKOUT]   (default ~/cursor_src/benchbase)

set -euo pipefail
src="${1:-$HOME/cursor_src/benchbase}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="$repo/build-bench/benchbase"

commit="$(git -C "$src" rev-parse HEAD)"
dirty="$(git -C "$src" status --porcelain | wc -l)"
echo "BenchBase $src at $commit ($(git -C "$src" rev-parse --abbrev-ref HEAD), $dirty uncommitted files)"

mkdir -p "$out"
rsync -a --delete --exclude .git --exclude target "$src/" "$out/src/"
(cd "$out/src" && ./mvnw -q -B package -P postgres -DskipTests)
rm -rf "$out/benchbase-postgres"
tar xzf "$out/src/target/benchbase-postgres.tgz" -C "$out"
echo "$commit" > "$out/BUILD_COMMIT"
echo "built $out/benchbase-postgres"
