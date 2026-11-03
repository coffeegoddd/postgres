#!/bin/bash
# A merge on the history store of two branches of a table of ROWS rows,
# CHANGED rows set on each side, spread over the table (every ROWS /
# CHANGED rows, the two sides interleaved, none set by both): its time,
# the chunks it asked the store for and put, and the history store's
# pages read in (not found in shared buffers) and found.  Run with shared
# buffers as large as the store (the cache at full size) and at a quarter
# of that, each after a restart (cold).
#   usage: vc_merge_rows.sh BIN_DIR NEW_DATA_DIR [ROWS [CHANGED]]
# BIN_DIR is an installed build's bin, with test_prolly installed;
# NEW_DATA_DIR must not exist (a cluster is made there, on port 55497).
set -eu
B=$1
D=$2
ROWS=${3:-1000000}
CHANGED=${4:-10000}
P=55497
test -e "$D" && { echo "$D exists"; exit 1; }
"$B/initdb" -D "$D" -A trust >/dev/null 2>&1
printf "port = $P\nautovacuum = off\nchunk_free_in_background = off\nmax_wal_size = 16GB\nshared_buffers = 4GB\n" >> "$D/postgresql.conf"
"$B/pg_ctl" -D "$D" -l "$D/log" -w start >/dev/null
trap '"$B/pg_ctl" -D "$D" -m fast stop >/dev/null 2>&1' EXIT
q() { "$B/psql" -X -q -At -p $P -d postgres "$@"; }
restart() { "$B/pg_ctl" -D "$D" -l "$D/log" -m fast -w restart >/dev/null; }
ms() { printf "%.0f" "$(q -c "SELECT ($1) / 1000.0")"; }
# (the history store's pages read in, and found, so far)
io() { q -F' ' -c "SELECT sum(heap_blks_read), sum(heap_blks_hit) FROM pg_statio_all_tables WHERE schemaname = 'pg_history'"; }

q -c "CREATE EXTENSION test_prolly" >/dev/null
q -c "SELECT test_vc_open()" >/dev/null
q -c "SELECT test_vc_commit('main', 'base')" >/dev/null
load=$(q -c "SELECT test_vc_bench_load('main', $ROWS, 100000)")
store=$(q -c "SELECT pg_relation_size('pg_history.chunks') + pg_relation_size('pg_history.chunk_index')")
echo "$ROWS rows loaded in $(ms $load) ms; the history store $(( store / 1048576 )) MB"
echo "cache      merge ms   chunks asked   put   pages read   found"

# (each merge a new pair of branches off main)
n=0
for share in full quarter; do
	n=$((n + 1))
	if [ $share = full ]; then sb=$(( store / 8192 * 5 / 4 + 16384 )); else sb=$(( store / 8192 / 4 )); fi
	q -c "ALTER SYSTEM SET shared_buffers = $sb" >/dev/null
	q -c "SELECT test_vc_branch('x$n', 'main'), test_vc_branch('y$n', 'main')" >/dev/null
	q -c "SELECT test_vc_bench_edit('x$n', $n, $CHANGED, $ROWS / $CHANGED, $n * 10 + 1)" >/dev/null
	q -c "SELECT test_vc_bench_edit('y$n', $ROWS / $CHANGED / 2 + $n, $CHANGED, $ROWS / $CHANGED, $n * 10 + 2)" >/dev/null
	restart
	read r0 h0 <<< "$(io)"
	read status us gets puts <<< "$(q -F' ' -c "SELECT replace(status, ' ', '_'), us, gets, puts FROM test_vc_bench_merge('x$n', 'y$n')")"
	read r1 h1 <<< "$(io)"
	printf "%-8s %10s   %12s   %3s   %10s   %5s   (%s, shared_buffers %d MB)\n" $share "$(ms $us)" $gets $puts \
		$(( r1 - r0 )) $(( h1 - h0 )) "$status" $(( sb * 8 / 1024 ))
done
