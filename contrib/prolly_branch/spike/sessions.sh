#!/bin/bash
# Experiment A beyond one session: readers and plans across a checkout,
# logical decoding, the freeze horizon, dropped tables, crash recovery.
# Run after branch_test.sql, against the database it left (spike_a).
set -u
B=/home/dustin/cursor_src/pg-v2-build/inst/bin
DATA=/home/dustin/pgv2-data
P=(-h /tmp -p 5499 -U postgres -d spike_a -X -q -At)
q() { "$B/psql" "${P[@]}" -c "$1"; }
ms() { echo $(( $(date +%s%N) / 1000000 )); }

q "SELECT prolly_branch.checkout('bt', 'main')" >/dev/null
MAIN=$(q "SELECT count(*) FROM bt.item")
q "SELECT prolly_branch.checkout('bt', 'b')" >/dev/null
BR=$(q "SELECT count(*) FROM bt.item")
q "SELECT prolly_branch.checkout('bt', 'main')" >/dev/null
echo "rows: main $MAIN, b $BR"

echo "== 1. a reader waits for a checkout, then sees the branch"
( "$B/psql" "${P[@]}" -c "BEGIN; SELECT prolly_branch.checkout('bt','b'); SELECT pg_sleep(2); COMMIT;" >/dev/null ) &
sleep 0.5
t0=$(ms); r=$(q "SELECT count(*) FROM bt.item"); t1=$(ms)
wait
echo "reader waited $((t1 - t0)) ms and read $r (b has $BR)"

echo "== 2. a session's prepared plan and a cursor across checkouts"
FIFO=$(mktemp -u); mkfifo "$FIFO"; OUT=$(mktemp)
"$B/psql" "${P[@]}" < "$FIFO" > "$OUT" 2>&1 &
S1=$!
exec 3> "$FIFO"
echo "PREPARE q AS SELECT count(*) FROM bt.item; EXECUTE q;" >&3
sleep 0.5
q "SELECT prolly_branch.checkout('bt','main')" >/dev/null
echo "EXECUTE q;" >&3
sleep 0.5
echo "BEGIN; DECLARE c CURSOR FOR SELECT id FROM bt.item ORDER BY id; FETCH 1 FROM c;" >&3
sleep 0.5
( t0=$(ms); q "SELECT prolly_branch.checkout('bt','b')" >/dev/null; echo "checkout waited $(( $(ms) - t0 )) ms for the open cursor" ) &
sleep 1
echo "FETCH 1 FROM c; COMMIT; EXECUTE q;" >&3
exec 3>&-
wait $S1
wait
echo "session saw: $(tr '\n' ' ' < "$OUT")"
rm -f "$FIFO" "$OUT"

echo "== 3. logical decoding across a checkout"
q "SELECT prolly_branch.checkout('bt','main')" >/dev/null
q "SELECT pg_drop_replication_slot('spike') FROM pg_replication_slots WHERE slot_name = 'spike'" >/dev/null
q "SELECT 1 FROM pg_create_logical_replication_slot('spike', 'test_decoding')" >/dev/null
q "INSERT INTO bt.item (v, tags, pt, word, big) VALUES (42, '{1}', '(1,1)', 'on main', 'x')" >/dev/null
q "SELECT prolly_branch.checkout('bt','b')" >/dev/null
q "INSERT INTO bt.item (v, tags, pt, word, big) VALUES (43, '{1}', '(1,1)', 'on b', 'x')" >/dev/null
q "SELECT data FROM pg_logical_slot_get_changes('spike', NULL, NULL) WHERE data LIKE 'table bt.item%' OR data LIKE '%pg_class%'" | cut -c1-90
q "SELECT pg_drop_replication_slot('spike')" >/dev/null

echo "== 4. the freeze horizon does not see a parked branch"
for i in $(seq 1 200); do q "SELECT txid_current()" >/dev/null; done
q "VACUUM (FREEZE) bt.item" >/dev/null
q "SELECT 'parked main: relfrozenxid ' || relfrozenxid || ', age ' || age(relfrozenxid) FROM prolly_branch.files WHERE branch = 'main' AND relid = 'bt.item'::regclass"
q "SELECT 'checked out b: relfrozenxid ' || relfrozenxid || ', age ' || age(relfrozenxid) FROM pg_class WHERE oid = 'bt.item'::regclass"
q "SELECT 'oldest the database will keep status for (min relfrozenxid in pg_class): age ' || max(age(relfrozenxid)) FROM pg_class WHERE relkind IN ('r','m','t') AND relfrozenxid <> '0'"

echo "== 5. a table dropped leaves its parked files"
q "CREATE TABLE bt.gone (x int); INSERT INTO bt.gone SELECT generate_series(1, 1000)" >/dev/null
q "DELETE FROM prolly_branch.files WHERE branch = 'b'" >/dev/null
q "SELECT prolly_branch.remember('bt'::regnamespace)" >/dev/null
q "SELECT prolly_branch.create_branch('bt', 'd')" >/dev/null
DB=$(q "SELECT oid FROM pg_database WHERE datname = 'spike_a'")
F=$(q "SELECT filenode FROM prolly_branch.files WHERE branch = 'd' AND relid = 'bt.gone'::regclass")
q "DROP TABLE bt.gone" >/dev/null
q "CHECKPOINT" >/dev/null
if [ -e "$DATA/base/$DB/$F" ]; then echo "branch d's file for bt.gone is still on disk after DROP TABLE"; else echo "dropped with the table"; fi

echo "== 6. crash recovery: writes on a branch, an immediate stop"
q "SELECT prolly_branch.checkout('bt','b')" >/dev/null
q "UPDATE bt.item SET v = v + 1 WHERE id % 4 = 0; INSERT INTO bt.item (v, tags, pt, word, big) SELECT 9, '{9}', '(9,9)', 'crash', repeat('y', 5000) FROM generate_series(1, 100)" >/dev/null
BEFORE_B=$(q "SELECT bt.fingerprint(false)")
q "SELECT prolly_branch.checkout('bt','main')" >/dev/null
BEFORE_M=$(q "SELECT bt.fingerprint(false)")
"$B/pg_ctl" -D "$DATA" -m immediate stop >/dev/null
"$B/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null
AFTER_M=$(q "SELECT bt.fingerprint(false)")
q "SELECT bt.check('main after crash', '$BEFORE_M')" >/dev/null && echo "main survives the crash"
q "SELECT prolly_branch.checkout('bt','b')" >/dev/null
q "SELECT bt.check('b after crash', '$BEFORE_B')" >/dev/null && echo "b survives the crash"
