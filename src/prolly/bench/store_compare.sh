#!/bin/bash
# The history store, three ways, on one workload: N random chunks of 4 KB
# committed BATCH at a time; every chunk read back in random order (warm,
# then after a server restart for the Postgres stores, after reopening for
# dirstore; every byte fetched); a random half
# discarded and its space taken back.
#   rows:   chunks as rows of a table (hash key, STORAGE EXTERNAL)
#   pages:  chunks in an append-only log of our own page layout, through the
#           buffer manager with generic WAL, and a hash index table
#   dirstore: the file store with its own journal (bench/storebench)
# Against the running test server (port 5499).
set -u
N=${1:-20000}
BATCH=${2:-100}
B=/home/dustin/cursor_src/pg-v2-build/inst/bin
DATA=/home/dustin/pgv2-data
DIR=/home/dustin/pgv2-bench/storecmp
HERE=$(cd "$(dirname "$0")" && pwd)
q() { "$B/psql" -h /tmp -p 5499 -U postgres -d storecmp -X -q -At "$@"; }
ms() { echo $(( $(date +%s%N) / 1000000 )); }
lsn() { q -c "SELECT pg_current_wal_lsn()"; }
walmb() { q -c "SELECT round(pg_wal_lsn_diff('$2', '$1') / 1048576.0, 1)"; }
mb() { q -c "SELECT round(($1) / 1048576.0, 1)"; }
restart() { "$B/pg_ctl" -D "$DATA" -m fast -w restart >/dev/null; }
row() { printf "  %-9s %-9s %8s ms   WAL %7s MB   disk %7s MB\n" "$1" "$2" "$3" "$4" "$5"; }

"$B/psql" -h /tmp -p 5499 -U postgres -d postgres -X -q -c "DROP DATABASE IF EXISTS storecmp" -c "CREATE DATABASE storecmp" 2>&1 | grep -v NOTICE
q <<SQL >/dev/null
CREATE EXTENSION prolly_chunkpages;
SELECT cp.setup();
CREATE SCHEMA e;
CREATE TABLE e.chunks (hash bytea PRIMARY KEY, data bytea NOT NULL);
ALTER TABLE e.chunks ALTER data SET STORAGE EXTERNAL;
CREATE FUNCTION e.put(d bytea) RETURNS bytea LANGUAGE sql AS \$\$
	INSERT INTO e.chunks VALUES (sha256(d), d) ON CONFLICT DO NOTHING;
	SELECT sha256(d)
\$\$;
CREATE FUNCTION e.get(h bytea) RETURNS bytea LANGUAGE sql STABLE AS \$\$
	SELECT data FROM e.chunks WHERE hash = h
\$\$;
-- the chunks, made once, so making them is not timed
CREATE UNLOGGED TABLE stage (n int PRIMARY KEY, d bytea);
INSERT INTO stage SELECT g, (SELECT decode(string_agg(md5(random()::text || g || i), ''), 'hex')
							  FROM generate_series(1, 256) i) FROM generate_series(1, $N) g;
CREATE UNLOGGED TABLE hashes AS SELECT n, sha256(d) AS hash FROM stage ORDER BY random();
ALTER TABLE hashes ADD COLUMN pos serial;
VACUUM ANALYZE stage;
VACUUM ANALYZE hashes;
SQL
echo "# $N random chunks of $(q -c "SELECT length(d) FROM stage LIMIT 1") bytes, $BATCH to a commit"

for store in rows pages; do
	if [ $store = rows ]; then put=e.put; get=e.get; else put=cp.put; get=cp.get; fi
	q -c "CHECKPOINT" >/dev/null
	w0=$(lsn); t0=$(ms)
	# one session, a transaction per batch
	for k in $(seq 0 $(( N / BATCH - 1 ))); do
		echo "BEGIN; SELECT count($put(d)) FROM stage WHERE n > $(( k * BATCH )) AND n <= $(( (k + 1) * BATCH )); COMMIT;"
	done | q >/dev/null
	t1=$(ms); w1=$(lsn)
	if [ $store = rows ]; then disk="pg_total_relation_size('e.chunks')"; else disk="pg_relation_size('cp.pages') + pg_total_relation_size('cp.idx')"; fi
	row $store write $((t1 - t0)) "$(walmb $w0 $w1)" "$(mb "$disk")"
	t0=$(ms); q -c "SELECT sum(length($get(hash) || '\\x00'::bytea)) FROM hashes" >/dev/null; t1=$(ms)
	row $store read $((t1 - t0)) "-" "-"
	restart
	t0=$(ms); q -c "SELECT sum(length($get(hash) || '\\x00'::bytea)) FROM hashes" >/dev/null; t1=$(ms)
	row $store "read cold" $((t1 - t0)) "-" "-"
	w0=$(lsn); t0=$(ms)
	if [ $store = rows ]; then
		q -c "DELETE FROM e.chunks WHERE hash IN (SELECT hash FROM hashes WHERE pos <= $(( N / 2 )))" -c "VACUUM e.chunks" >/dev/null
	else
		q -c "DELETE FROM cp.idx WHERE hash IN (SELECT hash FROM hashes WHERE pos <= $(( N / 2 )))" >/dev/null
		q -c "SELECT cp.compact()" -c "VACUUM cp.idx" >/dev/null
	fi
	t1=$(ms); w1=$(lsn)
	row $store discard $((t1 - t0)) "$(walmb $w0 $w1)" "$(mb "$disk")"
	if [ $store = rows ]; then
		# VACUUM leaves the space for reuse; only a rewrite gives it back
		w0=$(lsn); t0=$(ms); q -c "VACUUM FULL e.chunks" >/dev/null; t1=$(ms); w1=$(lsn)
		row rows "vac full" $((t1 - t0)) "$(walmb $w0 $w1)" "$(mb "$disk")"
	fi
done

echo "  dirstore (its own journal, no WAL; chunks reached through a node per batch):"
"$HERE/storebench" "$DIR/dirstore-$$" "$N" "$BATCH" | sed 's/^/    /'
