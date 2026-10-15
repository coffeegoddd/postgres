#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/../../../build-bench/vc"
mkdir -p "$out"
cd "$here/slt"
go build -o "$out/engine-vc" ./cmd/engine-vc
