#!/bin/bash
# Dolt's sysbench OLTP tests on stock Postgres, then on the prototype: the
# same server, the tables in a versioned schema, with the capture daemon
# (rebuild --capture-daemon) decoding every change and making a Dolt commit
# every few seconds into the history store inside Postgres.
#   sysbench_compare.sh [SECONDS_PER_TEST] [COMMIT_INTERVAL]
set -u
T=${1:-20}
EVERY=${2:-5}
B=/home/dustin/cursor_src/pg-v2-build/inst/bin
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/claude-1000/sysbench_compare}
mkdir -p "$OUT"
TESTS="oltp_point_select oltp_read_only select_random_points select_random_ranges oltp_read_write oltp_write_only oltp_insert oltp_update_index oltp_update_non_index oltp_delete"
SB="sysbench --db-driver=pgsql --pgsql-host=/tmp --pgsql-port=5499 --pgsql-user=postgres --tables=1 --table-size=10000 --rand-type=uniform --db-ps-mode=disable --percentile=50"

run_all() {	# label db
	for threads in 1 16; do
		for t in $TESTS; do
			res=$($SB --pgsql-db=$2 --threads=$threads --time=$T $t run 2>&1)
			tps=$(echo "$res" | sed -n 's/.*transactions: *[0-9]* *(\([0-9.]*\) per sec.*/\1/p')
			lat=$(echo "$res" | sed -n 's/.*50th percentile: *\([0-9.]*\).*/\1/p')
			printf "%-8s %2d  %-24s %10s tps  %8s ms\n" "$1" "$threads" "$t" "$tps" "$lat"
		done
	done
}

# stock Postgres
"$B/psql" -h /tmp -p 5499 -U postgres -d postgres -X -q -c "DROP DATABASE IF EXISTS sb_stock" -c "CREATE DATABASE sb_stock" 2>&1 | grep -v NOTICE
$SB --pgsql-db=sb_stock oltp_common prepare >/dev/null
"$B/psql" -h /tmp -p 5499 -U postgres -d sb_stock -X -q -c "VACUUM ANALYZE"
run_all stock sb_stock | tee "$OUT/stock.txt"

# the prototype: capture running beside the same workload
"$HERE/rebuild" --capture-daemon "$EVERY" > "$OUT/daemon.log" 2>&1 &
DAEMON=$!
until grep -q ready "$OUT/daemon.log"; do sleep 0.5; done
$SB --pgsql-db=rt_src oltp_common prepare >/dev/null
"$B/psql" -h /tmp -p 5499 -U postgres -d rt_src -X -q -c "VACUUM ANALYZE"
run_all capture rt_src | tee "$OUT/capture.txt"
kill -TERM $DAEMON
wait $DAEMON
echo "# daemon: $(grep -c '^commit' "$OUT/daemon.log") Dolt commits"
grep '^# verified\|^# MISMATCH' "$OUT/daemon.log"
grep '^commit' "$OUT/daemon.log" | awk '{c+=$4; m+=$7; f+=$10; n++} END {printf "# average per commit: capture %.0f ms, commit %.0f ms, flush %.0f ms\n", c/n, m/n, f/n}'
