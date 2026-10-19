#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/../../../build-bench/differential"
mkdir -p "$out"
cd "$here/slt"
go build -o "$out/engine-diff" ./cmd/engine-diff
go build -o "$out/vc-fuzz" ./cmd/vc-fuzz
