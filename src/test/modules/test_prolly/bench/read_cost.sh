#!/bin/bash
# The production history store's per-chunk read cost from C, no SQL a
# chunk: N random chunks of 4 KB put, then read back in random order by a
# C function (test_chunk_read_bench) four ways: the index lookup alone,
# the log's read alone (the chunks' places found first, untimed),
# ChunkStoreGet a chunk at a time, and ChunkStoreGetMany a thousand at a
# time (read in the log's order); every byte read summed, as storebench
# sums dirstore's.  Each three times.  Warm: everything in shared buffers
# (set large enough to hold the store).  Cold: just after a restart,
# shared buffers empty, the kernel's page cache not dropped (that needs
# root), and warm from the writes just before.  Beside it, dirstore's
# memory-mapped reads (src/prolly/bench/storebench) if given: its "read"
# just after its writes, and its "read cold" after it reopens, the page
# cache warm both times.
# (A profile of a mode, as for the commit that added this: the same
# queries in a single-user backend, postgres --single, under
# perf record -e cycles:u; perf report --sort symbol.)
#   usage: read_cost.sh BIN_DIR NEW_DATA_DIR [N [STOREBENCH]]
# BIN_DIR is an installed build's bin, with test_prolly installed;
# NEW_DATA_DIR must not exist (a cluster is made there, on port 55495).
set -eu
B=$1
D=$2
N=${3:-100000}
SB=${4:-}
P=55495
test -e "$D" && { echo "$D exists"; exit 1; }
"$B/initdb" -D "$D" -A trust >/dev/null 2>&1
printf "port = $P\nautovacuum = off\nchunk_free_in_background = off\nmax_wal_size = 8GB\nshared_buffers = 1GB\n" >> "$D/postgresql.conf"
"$B/pg_ctl" -D "$D" -l "$D/log" -w start >/dev/null
trap '"$B/pg_ctl" -D "$D" -m fast stop >/dev/null 2>&1 || true' EXIT
q() { "$B/psql" -X -q -At -p $P -d postgres "$@"; }
restart() { "$B/pg_ctl" -D "$D" -l "$D/log" -m fast -w restart >/dev/null; }

q <<SQL >/dev/null
CREATE EXTENSION test_prolly;
CREATE UNLOGGED TABLE stage (n int PRIMARY KEY, d bytea);
INSERT INTO stage SELECT g, (SELECT decode(string_agg(md5(random()::text || g || i), ''), 'hex')
							  FROM generate_series(1, 256) i) FROM generate_series(1, $N) g;
SQL
for k in $(seq 0 $(( N / 100 - 1 ))); do
	echo "BEGIN; SELECT count((test_cs_put('pages', d)).hash) FROM stage WHERE n > $(( k * 100 )) AND n <= $(( (k + 1) * 100 )); COMMIT;"
done | q >/dev/null
q -c "CREATE UNLOGGED TABLE order_read AS SELECT array_agg(substr(sha256(d), 1, 20) ORDER BY random()) AS hashes FROM stage" >/dev/null
q -c "CHECKPOINT" >/dev/null
echo "# $N random chunks of 4096 bytes, read in random order from C (store $(q -c "SELECT pg_size_pretty(pg_total_relation_size('pg_history.chunks') + pg_total_relation_size('pg_history.chunk_index'))"))"

bench() {
	q -c "SELECT format('  %-10s %-5s %8s ms  %6s us a chunk  (%s of %s chunks, %s MB)', '$1', '$2', round(ms::numeric, 1),
		round((ms * 1000 / array_length(hashes, 1))::numeric, 2), chunks, array_length(hashes, 1), round(bytes / 1048576.0, 1))
		FROM order_read, test_chunk_read_bench('pg_history.chunks', 'pg_history.chunk_index', hashes, '$1')"
}
for how in lookup read get get_many; do
	for r in 1 2 3; do
		restart
		q -c "SELECT count(*) FROM order_read" >/dev/null
		bench $how cold
	done
	for r in 1 2 3; do
		bench $how warm
	done
done

if [ -n "$SB" ]; then
	test -e "$D-dirstore" && { echo "$D-dirstore exists"; exit 1; }
	echo "  dirstore (memory-mapped, its own process; every chunk in random order, the page cache warm: read after its writes, read cold after it reopens):"
	"$SB" "$D-dirstore" "$N" 100 | grep -E "read|reopen" | sed 's/^/    /'
fi
