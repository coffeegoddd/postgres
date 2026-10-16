#!/usr/bin/env bash
# Build a fresh install with evidence binding the binaries to their source.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$here/build_postgres.py" "$@"
