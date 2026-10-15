#!/usr/bin/env bash
#
# Build the sqllogictest runner (slt/) into build-bench/slt/slt-runner and
# record a hash of its source in build-bench/slt/SOURCE_HASH, so a run can
# tell a stale build.  The corpus itself is read in place from a checkout of
# Dolthub's sqllogictest (default ~/cursor_src/sqllogictest); its commit is
# recorded with each result set.
#
# Usage: build_slt.sh

set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
out="$repo/build-bench/slt"

command -v go >/dev/null || { echo "build_slt.sh: needs Go (https://go.dev/dl/)" >&2; exit 1; }
mkdir -p "$out"
(cd "$here/slt" && go build -o "$out/slt-runner" .)
# The hash covers go.mod, go.sum and *.go, in byte order of their names.
(cd "$here/slt" && cat $(LC_ALL=C ls go.mod go.sum *.go | LC_ALL=C sort) \
	| sha256sum | cut -d' ' -f1) > "$out/SOURCE_HASH"
echo "built $out/slt-runner"
