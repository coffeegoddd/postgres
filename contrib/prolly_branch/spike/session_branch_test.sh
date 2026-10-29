#!/bin/bash
# Experiment B: a branch per session through relation_locator_hook.
# Sets up its own database (spike_b) with branch_test.sql, then branches
# main into g and works on both at once.
set -u
B=/home/dustin/cursor_src/pg-v2-build/inst/bin
DATA=/home/dustin/pgv2-data
DIR=$(cd "$(dirname "$0")" && pwd)
P=(-h /tmp -p 5499 -U postgres -d spike_b -X -q -At)
q() { "$B/psql" "${P[@]}" -c "$1"; }
fail=0
expect() { if [ "$2" == "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: got [$2] want [$3]"; fail=1; fi; }

"$B/psql" -h /tmp -p 5499 -U postgres -d postgres -X -q -c "DROP DATABASE IF EXISTS spike_b" -c "CREATE DATABASE spike_b"
"$B/psql" "${P[@]}" -o /dev/null -f "$DIR/branch_test.sql" | tail -1
q "CREATE TABLE bt.cached (id int GENERATED ALWAYS AS IDENTITY (CACHE 20) PRIMARY KEY, v int)" >/dev/null
q "INSERT INTO bt.cached (v) SELECT generate_series(1, 5)" >/dev/null
q "SELECT prolly_branch.create_branch('bt', 'g')" >/dev/null
q "SELECT prolly_branch.create_branch('bt', 'h')" >/dev/null
FP_MAIN=$(q "SELECT bt.fingerprint(false)")

# Two long-lived sessions, one on each branch.
start() {	# name branch
	mkfifo "/tmp/claude-1000/sb_$1"
	"$B/psql" "${P[@]}" < "/tmp/claude-1000/sb_$1" > "/tmp/claude-1000/sb_$1.out" 2>&1 &
	eval "exec ${3}> /tmp/claude-1000/sb_$1"
	echo "SELECT prolly_branch.use_branch('bt', '$2');" >&"$3"
}
ask() {	# fd name sql: the session's answer
	: > "/tmp/claude-1000/sb_$2.out"
	echo "$3;" >&"$1"
	echo "SELECT '#done';" >&"$1"
	until grep -aq '#done' "/tmp/claude-1000/sb_$2.out"; do sleep 0.05; done
	if grep -aq 'ERROR' "/tmp/claude-1000/sb_$2.out"; then
		echo "  [$2: $(grep -a 'ERROR' "/tmp/claude-1000/sb_$2.out" | head -1)]" >&2
	fi
	grep -av '#done' "/tmp/claude-1000/sb_$2.out" | grep -av '^$' | tail -"${4:-1}"
}
rm -f /tmp/claude-1000/sb_m /tmp/claude-1000/sb_g /tmp/claude-1000/sb_m.out /tmp/claude-1000/sb_g.out
start m main 5
start g g 6
ask 5 m "SELECT 1" >/dev/null; ask 6 g "SELECT 1" >/dev/null

echo "== B1. two sessions, two branches, at once"
ask 6 g "SELECT bt.write(400, 13); DELETE FROM bt.item WHERE id % 13 = 0;" >/dev/null
ask 5 m "SELECT bt.write(100, 17);" >/dev/null
FP_G=$(ask 6 g "SELECT bt.fingerprint(false)")
FP_M2=$(ask 5 m "SELECT bt.fingerprint(false)")
expect "g changed apart from main" "$([ "$FP_G" != "$FP_M2" ] && echo differ)" differ
expect "g through every index, amcheck clean" "$(ask 6 g "SELECT bt.check('g', '$FP_G'); SELECT 'ok'")" ok
expect "main through every index, amcheck clean" "$(ask 5 m "SELECT bt.check('m', '$FP_M2'); SELECT 'ok'")" ok
expect "a new session sees the checked-out branch (main)" "$(q "SELECT bt.fingerprint(false)")" "$FP_M2"

echo "== B2. parallel workers read the session's branch"
expect "g, parallel scan" "$(ask 6 g "SET debug_parallel_query = on; SET parallel_setup_cost = 0; SET min_parallel_table_scan_size = 0; SELECT count(*) || '/' || sum(v) FROM bt.item;")" \
	"$(ask 6 g "SET debug_parallel_query = off; SELECT count(*) || '/' || sum(v) FROM bt.item;")"
expect "a worker ran" "$(ask 6 g "SET debug_parallel_query = on; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM bt.item" 20 | grep -ac 'Workers Launched: [1-9]')" 1
ask 6 g "SET debug_parallel_query = off" >/dev/null

echo "== B3. a sequence's cached values when a session changes branch"
A=$(ask 6 g "SELECT nextval(pg_get_serial_sequence('bt.cached', 'id'))")
ask 6 g "SELECT prolly_branch.use_branch('bt', 'main');" >/dev/null
M=$(ask 6 g "SELECT nextval(pg_get_serial_sequence('bt.cached', 'id'))")
echo "  g handed out $A, then the same session on main got $M (main's own next: $(q "SELECT last_value FROM bt.cached_id_seq"))"
ask 6 g "SELECT prolly_branch.use_branch('bt', 'g');" >/dev/null

echo "== B4. logical decoding of a session's writes to its branch's files"
q "SELECT 1 FROM pg_create_logical_replication_slot('spike_b', 'test_decoding')" >/dev/null
ask 6 g "INSERT INTO bt.item (v, tags, pt, word, big) VALUES (77, '{1}', '(1,1)', 'decode me', 'x');" >/dev/null
ask 5 m "INSERT INTO bt.item (v, tags, pt, word, big) VALUES (78, '{1}', '(1,1)', 'and me', 'x');" >/dev/null
q "SELECT data FROM pg_logical_slot_get_changes('spike_b', NULL, NULL) WHERE data NOT LIKE 'BEGIN%' AND data NOT LIKE 'COMMIT%'" 2>&1 | cut -c1-110 | head -6
q "SELECT pg_drop_replication_slot('spike_b')" >/dev/null 2>&1

echo "== B5. VACUUM in a session on g writes g's numbers into the shared catalog"
BEFORE=$(q "SELECT relpages || '/' || reltuples::int FROM pg_class WHERE oid = 'bt.item'::regclass")
ask 6 g "VACUUM bt.item;" >/dev/null
AFTER=$(q "SELECT relpages || '/' || reltuples::int FROM pg_class WHERE oid = 'bt.item'::regclass")
echo "  pg_class relpages/reltuples for bt.item: $BEFORE before, $AFTER after (main has $(q "SELECT count(*) FROM bt.item") rows, g $(ask 6 g "SELECT count(*) FROM bt.item"))"

echo "== B6. a global checkout under a session that chose its branch"
G_BEFORE=$(ask 6 g "SELECT bt.fingerprint(false)")
q "SELECT prolly_branch.checkout('bt', 'h')" >/dev/null
expect "the database is on h" "$(q "SELECT branch FROM prolly_branch.checked_out")" h
expect "the g session still reads g" "$G_BEFORE" "$(ask 6 g "SELECT bt.fingerprint(false)")" "$(ask 6 g "SELECT bt.fingerprint(false)")"
G_NOW=$(ask 6 g "SELECT count(*) FROM bt.item")
expect "g's rows after the checkout" "$G_NOW" "$(ask 6 g "SELECT count(*) FROM bt.item")"
q "SELECT prolly_branch.checkout('bt', 'main')" >/dev/null

echo "== B7. DDL in one session lands in the shared catalog"
ask 6 g "CREATE INDEX item_word_btree ON bt.item (word);" >/dev/null
echo "  main's session through g's new index: $(ask 5 m "SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM bt.item WHERE word >= 'w';") vs scanning: $(ask 5 m "SET enable_seqscan = on; SET enable_indexscan = off; SELECT count(*) FROM bt.item WHERE word >= 'w';")"
ask 5 m "RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;" >/dev/null
ask 6 g "DROP INDEX bt.item_word_btree;" >/dev/null

echo "== B8. crash with both branches written"
FP_G=$(ask 6 g "SELECT bt.write(50, 19); SELECT bt.fingerprint(false);")
FP_M2=$(ask 5 m "SELECT bt.write(60, 23); SELECT bt.fingerprint(false);")
exec 5>&- 6>&-
wait 2>/dev/null
"$B/pg_ctl" -D "$DATA" -m immediate stop >/dev/null
"$B/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null
expect "main after the crash" "$(q "SELECT bt.fingerprint(false)")" "$FP_M2"
expect "g after the crash" "$(q "SELECT prolly_branch.use_branch('bt', 'g'); SELECT bt.fingerprint(false)" | tail -1)" "$FP_G"
rm -f /tmp/claude-1000/sb_m /tmp/claude-1000/sb_g /tmp/claude-1000/sb_m.out /tmp/claude-1000/sb_g.out
exit $fail
