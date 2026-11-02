#!/bin/bash
# Experiment F rerun on the production history store: N random chunks of
# 4 KB committed BATCH to a transaction; every chunk read back in random
# order (warm, then after a restart; every byte fetched); a random half
# freed and its space taken back.  Beside it, the same workload as rows of
# a table (hash key, STORAGE EXTERNAL), and dirstore (the prototype's file
# store, src/prolly/bench/storebench) if given.
# The half freed is a random one (FREED=random, as experiment F), or the
# oldest, the first put (FREED=oldest, as history's collection frees the
# commits before a cut).
#   usage: [FREED=random|oldest] store_compare.sh BIN_DIR NEW_DATA_DIR [N [BATCH [STOREBENCH]]]
# BIN_DIR is an installed build's bin, with test_prolly installed;
# NEW_DATA_DIR must not exist (a cluster is made there, on port 55494).
set -u
B=$1
D=$2
N=${3:-100000}
BATCH=${4:-100}
SB=${5:-}
P=55494
FREED=${FREED:-random}
if [ "$FREED" = oldest ]; then order="n"; else order="random()"; fi
test -e "$D" && { echo "$D exists"; exit 1; }
"$B/initdb" -D "$D" -A trust >/dev/null 2>&1
printf "port = $P\nautovacuum = off\nchunk_free_in_background = off\nmax_wal_size = 8GB\n" >> "$D/postgresql.conf"
"$B/pg_ctl" -D "$D" -l "$D/log" -w start >/dev/null
q() { "$B/psql" -X -q -At -p $P -d postgres "$@"; }
ms() { echo $(( $(date +%s%N) / 1000000 )); }
lsn() { q -c "SELECT pg_current_wal_insert_lsn()"; }
walmb() { q -c "SELECT round(pg_wal_lsn_diff('$2', '$1') / 1048576.0, 1)"; }
mb() { q -c "SELECT round(($1) / 1048576.0, 1)"; }
restart() { "$B/pg_ctl" -D "$D" -m fast -w restart >/dev/null; }
row() { printf "  %-9s %-9s %8s ms   WAL %7s MB   disk %7s MB\n" "$1" "$2" "$3" "$4" "$5"; }

q <<SQL >/dev/null
CREATE EXTENSION test_prolly;
CREATE SCHEMA e;
CREATE TABLE e.chunks (hash bytea PRIMARY KEY, data bytea NOT NULL);
ALTER TABLE e.chunks ALTER data SET STORAGE EXTERNAL;
CREATE FUNCTION e.put(d bytea) RETURNS bytea LANGUAGE sql AS \$\$
	INSERT INTO e.chunks VALUES (substr(sha256(d), 1, 20), d) ON CONFLICT DO NOTHING;
	SELECT substr(sha256(d), 1, 20)
\$\$;
CREATE FUNCTION e.get(h bytea) RETURNS bytea LANGUAGE sql STABLE AS \$\$
	SELECT data FROM e.chunks WHERE hash = h
\$\$;
CREATE FUNCTION e.pput(d bytea) RETURNS bytea LANGUAGE sql AS \$\$
	SELECT hash FROM test_cs_put('pages', d)
\$\$;
CREATE FUNCTION e.pget(h bytea) RETURNS bytea LANGUAGE sql STABLE AS \$\$
	SELECT test_cs_get('pages', h)
\$\$;
-- the chunks, made once, so making them is not timed
CREATE UNLOGGED TABLE stage (n int PRIMARY KEY, d bytea);
INSERT INTO stage SELECT g, (SELECT decode(string_agg(md5(random()::text || g || i), ''), 'hex')
							  FROM generate_series(1, 256) i) FROM generate_series(1, $N) g;
CREATE UNLOGGED TABLE hashes AS SELECT n, substr(sha256(d), 1, 20) AS hash FROM stage ORDER BY $order;
ALTER TABLE hashes ADD COLUMN pos serial;
VACUUM ANALYZE stage;
VACUUM ANALYZE hashes;
SQL
echo "# $N random chunks of $(q -c "SELECT length(d) FROM stage LIMIT 1") bytes, $BATCH to a commit; the $FREED half freed"

for store in rows pages; do
	if [ $store = rows ]; then put=e.put; get=e.get; disk="pg_total_relation_size('e.chunks')"
	else put=e.pput; get=e.pget; disk="pg_relation_size('pg_history.chunks') + pg_relation_size('pg_history.chunk_index')"; fi
	q -c "CHECKPOINT" >/dev/null
	w0=$(lsn); t0=$(ms)
	for k in $(seq 0 $(( N / BATCH - 1 ))); do
		echo "BEGIN; SELECT count($put(d)) FROM stage WHERE n > $(( k * BATCH )) AND n <= $(( (k + 1) * BATCH )); COMMIT;"
	done | q >/dev/null
	t1=$(ms); w1=$(lsn)
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
		q -c "SELECT test_cs_free('pages', ARRAY(SELECT hash FROM hashes WHERE pos <= $(( N / 2 ))))" \
		  -c "VACUUM pg_history.chunk_index" >/dev/null
	fi
	t1=$(ms); w1=$(lsn)
	row $store discard $((t1 - t0)) "$(walmb $w0 $w1)" "$(mb "$disk")"
	if [ $store = rows ]; then
		# VACUUM leaves the space for reuse; only a rewrite gives it back
		w0=$(lsn); t0=$(ms); q -c "VACUUM FULL e.chunks" >/dev/null; t1=$(ms); w1=$(lsn)
		row rows "vac full" $((t1 - t0)) "$(walmb $w0 $w1)" "$(mb "$disk")"
	else
		# the pages freed in the middle of the log used again: the other half
		# put again, the file no bigger
		half=$(( N / 2 ))
		t0=$(ms)
		for k in $(seq 0 $(( half / BATCH - 1 ))); do
			echo "BEGIN; SELECT count($put(s.d)) FROM stage s JOIN hashes h USING (n) WHERE h.pos > $(( k * BATCH )) AND h.pos <= $(( (k + 1) * BATCH )); COMMIT;"
		done | q >/dev/null
		t1=$(ms)
		row pages "put again" $((t1 - t0)) "-" "$(mb "$disk")"
		echo "  pages audit: $(q -c "SELECT ok || ' ' || chunks || ' chunks' FROM test_cs_check('pages')")"
	fi
done
"$B/pg_ctl" -D "$D" -m fast stop >/dev/null

if [ -n "$SB" ]; then
	echo "  dirstore (its own journal, no WAL; chunks reached through a node per batch):"
	"$SB" "$D-dirstore" "$N" "$BATCH" | sed 's/^/    /'
fi
