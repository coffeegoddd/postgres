# Shared by system_prepare.sh and system_restore.sh.  Not meant to be run.
#
# The system settings a benchmark run changes, and where their previous
# values are saved so system_restore.sh can put them back.

STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/prolly_bench"
STATE_FILE="$STATE_DIR/system_settings.saved"

CPU_GLOB=/sys/devices/system/cpu/cpu[0-9]*/cpufreq
BOOST=/sys/devices/system/cpu/cpufreq/boost

# Lines of "path<TAB>value" for every setting we manage, as they are now.
current_settings() {
    local f
    for f in /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/kptr_restrict; do
        printf '%s\t%s\n' "$f" "$(cat "$f")"
    done
    if [[ -w $BOOST || -e $BOOST ]]; then
        printf '%s\t%s\n' "$BOOST" "$(cat "$BOOST")"
    fi
    # Governor before EPP: on amd-pstate-epp the performance governor forces
    # EPP to "performance", so EPP must be restored after the governor.
    for f in $CPU_GLOB/scaling_governor; do
        [[ -e $f ]] && printf '%s\t%s\n' "$f" "$(cat "$f")"
    done
    for f in $CPU_GLOB/energy_performance_preference; do
        [[ -e $f ]] && printf '%s\t%s\n' "$f" "$(cat "$f")"
    done
}

# Write value to a sysfs/procfs file as root.
write_setting() {
    local path=$1 value=$2
    if [[ "$(cat "$path")" != "$value" ]]; then
        echo "$value" | sudo tee "$path" >/dev/null
    fi
}

show_summary() {
    echo "  perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
    echo "  kptr_restrict:       $(cat /proc/sys/kernel/kptr_restrict)"
    [[ -e $BOOST ]] && echo "  cpu boost:           $(cat "$BOOST")"
    echo "  governors:           $(cat $CPU_GLOB/scaling_governor 2>/dev/null | sort | uniq -c | xargs)"
    echo "  energy pref (EPP):   $(cat $CPU_GLOB/energy_performance_preference 2>/dev/null | sort | uniq -c | xargs)"
}
