#!/bin/bash
# Experiment E: the chunk store and roots as rows of tables, through the
# buffer manager, against experiment D's files and custom WAL.  Roots move
# with the commit like any row; chunks are checkpointed and replicated like
# any page.  Uses waldb (from crash_test.sh) and makes a hot standby of it.
set -u
INST=/home/dustin/cursor_src/pg-v2-build/inst/bin
DATA=/home/dustin/pgv2-bench/waldb
STBY=/home/dustin/pgv2-bench/waldb_standby
PORT=5495
SPORT=5492
fail=0
expect() { if [ "$2" == "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: got [$2] want [$3]"; fail=1; fi; }
q() { "$INST/psql" -h /tmp -p $PORT -U postgres -d postgres -X -q -At "$@" 2>&1; }
s() { "$INST/psql" -h /tmp -p $SPORT -U postgres -d postgres -X -q -At "$@" 2>&1; }
up() { for i in $(seq 1 100); do q -c "SELECT 1" >/dev/null 2>&1 && return; sleep 0.1; done; echo "no server"; exit 1; }
crash() { "$INST/pg_ctl" -D "$DATA" -m immediate stop >/dev/null 2>&1; "$INST/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null; up; }

"$INST/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null 2>&1; up
q <<'SQL' >/dev/null
DROP SCHEMA IF EXISTS e CASCADE;
CREATE SCHEMA e;
CREATE TABLE e.chunks (hash bytea PRIMARY KEY, data bytea NOT NULL);
ALTER TABLE e.chunks ALTER data SET STORAGE EXTERNAL;	-- chunks arrive compressed or not; no second compression
CREATE TABLE e.roots (name text PRIMARY KEY, hash bytea NOT NULL);
CREATE TABLE e.t (note text);
CREATE FUNCTION e.put_chunk(d bytea) RETURNS bytea LANGUAGE sql AS $$
	INSERT INTO e.chunks VALUES (sha256(d), d) ON CONFLICT DO NOTHING;
	SELECT sha256(d)
$$;
-- the row lock serializes a root's writers in commit order
CREATE FUNCTION e.set_root(n text, h bytea) RETURNS void LANGUAGE sql AS $$
	INSERT INTO e.roots VALUES (n, h) ON CONFLICT (name) DO UPDATE SET hash = EXCLUDED.hash
$$;
CREATE FUNCTION e.root(n text) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce((SELECT encode(hash, 'hex') FROM e.roots WHERE name = n), '-')
$$;
CREATE FUNCTION e.random_chunk(n int) RETURNS bytea LANGUAGE sql AS $$
	SELECT decode(string_agg(md5(random()::text || g), ''), 'hex') FROM generate_series(1, n / 16) g
$$;
SQL
H() { echo -n "$1" | sha256sum | cut -c1-64; }

echo "== E1. roots move with the commit, through crashes"
q -c "BEGIN" -c "SELECT e.set_root('main', e.put_chunk('state A'))" -c "INSERT INTO e.t VALUES ('e1')" -c "COMMIT" >/dev/null
crash
expect "committed: root and row survive" "$(q -c "SELECT e.root('main')") $(q -c "SELECT count(*) FROM e.t WHERE note = 'e1'")" "$(H 'state A') 1"
q -c "BEGIN" -c "SELECT e.set_root('main', e.put_chunk('state B'))" -c "ROLLBACK" >/dev/null
expect "rolled back: root unchanged, its chunk gone" "$(q -c "SELECT e.root('main')") $(q -c "SELECT count(*) FROM e.chunks WHERE data = 'state B'")" "$(H 'state A') 0"
( q -c "BEGIN" -c "SELECT e.set_root('main', e.put_chunk('state D'))" -c "INSERT INTO e.t VALUES ('e4')" -c "SELECT pg_sleep(30)" -c "COMMIT" >/dev/null ) &
sleep 1.5; crash; wait 2>/dev/null
expect "open at the crash: nothing" "$(q -c "SELECT e.root('main')") $(q -c "SELECT count(*) FROM e.t WHERE note = 'e4'")" "$(H 'state A') 0"
q -c "BEGIN" -c "SELECT e.set_root('main', e.put_chunk('state E'))" -c "\\! $INST/psql -h /tmp -p $PORT -U postgres -d postgres -X -q -c CHECKPOINT" -c "COMMIT" >/dev/null
crash
expect "a checkpoint inside the transaction, then a crash" "$(q -c "SELECT e.root('main')")" "$(H 'state E')"

echo "== E2. a hot standby moves roots as commits replay"
"$INST/pg_ctl" -D "$STBY" -w stop -m fast >/dev/null 2>&1
if [ ! -d "$STBY" ]; then
	# a slot keeps the WAL the standby needs while it is stopped between runs
	q -c "SELECT pg_drop_replication_slot('e_standby') FROM pg_replication_slots WHERE slot_name = 'e_standby'" >/dev/null
	"$INST/pg_basebackup" -h /tmp -p $PORT -U postgres -D "$STBY" -R -X stream -C -S e_standby >/dev/null 2>&1
	printf "port = %s\n" $SPORT >> "$STBY/postgresql.conf"
fi
"$INST/pg_ctl" -D "$STBY" -l "$STBY/server.log" -w start >/dev/null
for i in $(seq 1 300); do [ "$(s -c "SELECT e.root('main')")" == "$(H 'state E')" ] && break; sleep 0.1; done
q -c "BEGIN" -c "SELECT e.set_root('main', e.put_chunk('state F'))" -c "INSERT INTO e.t VALUES ('e2')" -c "COMMIT" >/dev/null
LSN=$(q -c "SELECT pg_current_wal_lsn()")
for i in $(seq 1 300); do [ "$(s -c "SELECT pg_last_wal_replay_lsn() >= '$LSN'")" == t ] && break; sleep 0.1; done
expect "the standby's root, as soon as the commit replays" "$(s -c "SELECT e.root('main')")" "$(H 'state F')"
expect "its chunk on the standby" "$(s -c "SELECT convert_from(data, 'UTF8') FROM e.chunks WHERE hash = sha256('state F')")" "state F"
expect "and the row" "$(s -c "SELECT count(*) FROM e.t WHERE note = 'e2'")" 1
# experiment D's root, for comparison: its redo cannot see the commit, so
# the standby holds the root pending until it is promoted
HX=$(q -c "SELECT prolly_wal.put_chunk(convert_to('state X ' || clock_timestamp(), 'UTF8'))")
q -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HX')" -c "COMMIT" >/dev/null
LSN=$(q -c "SELECT pg_current_wal_lsn()")
for i in $(seq 1 300); do [ "$(s -c "SELECT pg_last_wal_replay_lsn() >= '$LSN'")" == t ] && break; sleep 0.1; done
expect "prolly_wal's root on the standby has not moved" "$([ "$(s -c "SELECT prolly_wal.get_root('main')")" != "$HX" ] && echo held)" held
expect "it is pending there" "$(grep -l "$HX" "$STBY"/pg_prolly/pending/* 2>/dev/null | wc -l | tr -d ' ')" 1

echo "== E3. logical decoding sees roots and chunks"
q -c "SELECT 1 FROM pg_create_logical_replication_slot('e3', 'test_decoding')" >/dev/null 2>&1 || echo "  (needs wal_level = logical; skipped)"
if q -c "SELECT 1 FROM pg_replication_slots WHERE slot_name = 'e3'" | grep -q 1; then
	q -c "BEGIN" -c "SELECT e.set_root('main', e.put_chunk('state G'))" -c "COMMIT" >/dev/null
	q -c "SELECT data FROM pg_logical_slot_get_changes('e3', NULL, NULL) WHERE data LIKE 'table e.%'" | cut -c1-70
	q -c "SELECT pg_drop_replication_slot('e3')" >/dev/null
fi

echo "== E4. cost: 20,000 random 4 KB chunks, 100 to a transaction"
measure() {	# label sql-producing-a-chunk-store-call
	local w0 t0 t1 w1
	q -c "CHECKPOINT" >/dev/null
	w0=$(q -c "SELECT pg_current_wal_lsn()"); t0=$(date +%s%N)
	for b in $(seq 1 200); do q -c "BEGIN" -c "SELECT count($2) FROM generate_series(1, 100)" -c "COMMIT" >/dev/null; done
	t1=$(date +%s%N); w1=$(q -c "SELECT pg_current_wal_lsn()")
	printf "  %-34s %6d ms   WAL %6s MB\n" "$1" $(( (t1 - t0) / 1000000 )) "$(q -c "SELECT round(pg_wal_lsn_diff('$w1', '$w0') / 1048576.0, 1)")"
}
measure "table rows (heap, TOAST, index)" "e.put_chunk(e.random_chunk(4096))"
FILES0=$(du -sk "$DATA/pg_prolly/chunks" | cut -f1)
measure "files + custom WAL (prolly_wal)" "prolly_wal.put_chunk(e.random_chunk(4096))"
echo "  on disk: table $(q -c "SELECT pg_size_pretty(pg_total_relation_size('e.chunks'))"), files $(( ($(du -sk "$DATA/pg_prolly/chunks" | cut -f1) - FILES0) / 1024 )) MB"

"$INST/pg_ctl" -D "$STBY" -w stop -m fast >/dev/null
"$INST/pg_ctl" -D "$DATA" -w stop -m fast >/dev/null
exit $fail
