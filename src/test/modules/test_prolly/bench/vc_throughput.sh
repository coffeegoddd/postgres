#!/bin/bash
# Commit throughput on the history store with 1, 16 and 64 backends, each
# committing on a branch of its own (a row of its own set, its table the
# same size throughout): commits a second, and a commit's latency (mean,
# p50, p99).  A run first makes the chunk-log lanes the backends write
# into, so none is made while a run is timed.
#   usage: vc_throughput.sh BIN_DIR NEW_DATA_DIR [SECONDS [CLIENTS...]]
# BIN_DIR is an installed build's bin, with test_prolly installed;
# NEW_DATA_DIR must not exist (a cluster is made there, on port 55496).
set -eu
B=$1
D=$2
SECS=${3:-20}
shift 3 2>/dev/null || shift $#
CLIENTS=${*:-1 16 64}
P=55496
test -e "$D" && { echo "$D exists"; exit 1; }
"$B/initdb" -D "$D" -A trust >/dev/null 2>&1
printf "port = $P\nmax_connections = 100\nautovacuum = off\nchunk_free_in_background = off\nmax_wal_size = 8GB\nshared_buffers = 1GB\n" >> "$D/postgresql.conf"
"$B/pg_ctl" -D "$D" -l "$D/log" -w start >/dev/null
# (stopped however the script ends)
trap '"$B/pg_ctl" -D "$D" -m fast stop >/dev/null 2>&1' EXIT
q() { "$B/psql" -X -q -At -p $P -d postgres "$@"; }
max=0
for c in $CLIENTS; do [ "$c" -gt $max ] && max=$c; done

q -c "CREATE EXTENSION test_prolly" >/dev/null
q -c "SELECT test_vc_open()" >/dev/null
q -c "SELECT test_vc_commit('main', 'base')" >/dev/null
q -c "SELECT test_vc_branch('b' || i, 'main') FROM generate_series(1, $max) i" >/dev/null
echo "SELECT test_vc_bench_commits('b' || (:client_id + 1), 1, 100);" > "$D/commit.sql"
# (the lanes made: as many backends at once as the largest run)
"$B/pgbench" -n -p $P -c $max -j $(( max < 8 ? max : 8 )) -t 5 -f "$D/commit.sql" postgres >/dev/null

echo "backends   commits/s   mean ms   p50 ms   p99 ms"
for c in $CLIENTS; do
	"$B/pgbench" -n -p $P -c $c -j $(( c < 8 ? c : 8 )) -T "$SECS" -l --log-prefix="$D/lat$c" \
		-f "$D/commit.sql" postgres > "$D/pgbench$c.out" 2>&1
	tps=$(sed -n 's/^tps = \([0-9.]*\).*/\1/p' "$D/pgbench$c.out")
	# (each line of a log: client, transaction, latency in microseconds, ...)
	cat "$D"/lat$c.* | awk '{print $3}' | sort -n > "$D/lat$c.sorted"
	n=$(wc -l < "$D/lat$c.sorted")
	mean=$(awk '{s += $1} END {printf "%.2f", s / NR / 1000}' "$D/lat$c.sorted")
	p50=$(awk -v k=$(( (n + 1) / 2 )) 'NR == k {printf "%.2f", $1 / 1000}' "$D/lat$c.sorted")
	p99=$(awk -v k=$(( (n * 99 + 99) / 100 )) 'NR == k {printf "%.2f", $1 / 1000}' "$D/lat$c.sorted")
	printf "%8d   %9.0f   %7s   %6s   %6s\n" $c "$tps" "$mean" "$p50" "$p99"
done

