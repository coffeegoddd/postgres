#!/usr/bin/env bash
#
# Undo system_prepare.sh: put back every setting it saved.  Needs sudo.
#
# Usage: system_restore.sh [--dry-run]

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

dry_run=false
for arg in "$@"; do
    case "$arg" in
        --dry-run) dry_run=true ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ ! -e $STATE_FILE ]]; then
    echo "Nothing to restore: no saved settings at $STATE_FILE."
    show_summary
    exit 0
fi

if $dry_run; then
    echo "Dry run: would restore:"
    sed 's/^/  /' "$STATE_FILE"
    exit 0
fi

failed=0
while IFS=$'\t' read -r path value; do
    if [[ -e $path ]]; then
        write_setting "$path" "$value" || { echo "could not restore $path" >&2; failed=1; }
    fi
done < "$STATE_FILE"

if (( failed )); then
    echo "Some settings were not restored; saved values kept in $STATE_FILE." >&2
    exit 1
fi
rm -f "$STATE_FILE"

echo "Settings restored:"
show_summary
