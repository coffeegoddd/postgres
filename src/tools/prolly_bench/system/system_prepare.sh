#!/usr/bin/env bash
#
# Put the machine into benchmark mode, saving the current settings first so
# system_restore.sh can undo it.  Needs sudo.
#
#   perf_event_paranoid = -1   perf can sample system-wide as a normal user
#   kptr_restrict = 0          perf can resolve kernel symbols
#   CPU governor = performance (on amd-pstate-epp this also sets EPP)
#   CPU boost = off            steadier clock speeds between repetitions;
#                              pass --keep-boost to leave it as it is
#
# The harness records all of these with every run.  Settings are not
# persistent: a reboot also restores the defaults.
#
# Usage: system_prepare.sh [--keep-boost] [--dry-run]

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

keep_boost=false
dry_run=false
for arg in "$@"; do
    case "$arg" in
        --keep-boost) keep_boost=true ;;
        --dry-run) dry_run=true ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ -e $STATE_FILE ]]; then
    echo "Benchmark settings are already applied (saved values in $STATE_FILE)."
    echo "Run system_restore.sh first if you want to re-apply them."
    show_summary
    exit 0
fi

echo "Current settings:"
show_summary

if $dry_run; then
    echo "Dry run: would save the settings above to $STATE_FILE and apply benchmark settings."
    exit 0
fi

mkdir -p "$STATE_DIR"
current_settings > "$STATE_FILE.tmp"
mv "$STATE_FILE.tmp" "$STATE_FILE"

write_setting /proc/sys/kernel/perf_event_paranoid -1
write_setting /proc/sys/kernel/kptr_restrict 0
for f in $CPU_GLOB/scaling_governor; do
    [[ -e $f ]] && write_setting "$f" performance
done
if ! $keep_boost && [[ -e $BOOST ]]; then
    write_setting "$BOOST" 0
fi

echo "Benchmark settings applied (previous values saved in $STATE_FILE):"
show_summary
