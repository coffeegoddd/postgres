#!/bin/bash
# The custom resource manager under crashes: roots move exactly with the
# SQL commit, chunks survive checkpoints, rolled-back work leaves no root.
# Makes its own cluster (waldb, port 5495) with prolly_wal preloaded.
set -u
INST=/home/dustin/cursor_src/pg-v2-build/inst/bin
DATA=/home/dustin/pgv2-bench/waldb
PORT=5495
fail=0
expect() { if [ "$2" == "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: got [$2] want [$3]"; fail=1; fi; }
q() { "$INST/psql" -h /tmp -p $PORT -U postgres -d postgres -X -q -At "$@" 2>&1; }
up() {
	for i in $(seq 1 100); do q -c "SELECT 1" >/dev/null 2>&1 && return; sleep 0.1; done
	echo "server did not come back"; exit 1
}
crash() { "$INST/pg_ctl" -D "$DATA" -m immediate stop >/dev/null 2>&1; "$INST/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null; up; }
root() { q -c "SELECT coalesce(prolly_wal.get_root('main'), '-')"; }
chunk() { q -c "SELECT prolly_wal.put_chunk('$1'::bytea)" | tail -1; }

if [ ! -d "$DATA" ]; then
	"$INST/initdb" -D "$DATA" -U postgres --no-sync >/dev/null 2>&1
	printf "port = %s\nshared_preload_libraries = 'prolly_wal'\nwal_level = replica\n" $PORT >> "$DATA/postgresql.conf"
fi
"$INST/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null
q -c "DROP EXTENSION IF EXISTS prolly_wal" -c "CREATE EXTENSION prolly_wal" -c "DROP TABLE IF EXISTS t" -c "CREATE TABLE t (note text)" >/dev/null
q -c "SELECT pg_switch_wal()" >/dev/null
# chunk hashes, made by the server itself (put_chunk only logs and writes)
HA=$(chunk 'state A'); HB=$(chunk 'state B'); HC=$(chunk 'state C'); HD=$(chunk 'state D'); HE=$(chunk 'state E')

echo "== W1. a root set in a committed transaction, then a crash"
q -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HA')" -c "INSERT INTO t VALUES ('w1')" -c "COMMIT" >/dev/null
expect "root after commit" "$(root)" "$HA"
crash
expect "root after the crash" "$(root)" "$HA"
expect "its chunk after the crash" "$(q -c "SELECT convert_from(prolly_wal.get_chunk('$HA'), 'UTF8')")" "state A"

echo "== W2. a root set in a transaction rolled back"
q -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HB')" -c "ROLLBACK" >/dev/null
expect "root unchanged" "$(root)" "$HA"
crash
expect "root unchanged after a crash" "$(root)" "$HA"

echo "== W3. a crash after the commit record, before the root's file"
q -c "SET prolly_wal.crash_after_commit = on" -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HC')" -c "INSERT INTO t VALUES ('w3')" -c "COMMIT" >/dev/null
up
expect "root moved with the commit" "$(root)" "$HC"
expect "and the row it committed with" "$(q -c "SELECT count(*) FROM t WHERE note = 'w3'")" 1
expect "recovery says so" "$(grep -c 'applied the roots of committed transaction' "$DATA/server.log" | tr -d ' ' | sed 's/^0$/none/;s/^[1-9][0-9]*$/yes/')" yes

echo "== W4. a crash with the transaction still open"
( q -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HD')" -c "INSERT INTO t VALUES ('w4')" -c "SELECT pg_sleep(30)" -c "COMMIT" >/dev/null ) &
sleep 1.5
crash
wait 2>/dev/null
expect "root as before" "$(root)" "$HC"
expect "no row either" "$(q -c "SELECT count(*) FROM t WHERE note = 'w4'")" 0
# it never reached commit, so its root record was never flushed and no
# pending file was written: there is nothing left to drop
expect "no pending roots left" "$(ls "$DATA/pg_prolly/pending" | wc -l)" 0

echo "== W5. a checkpoint between set_root and the commit, then a crash after the commit record"
q -c "SET prolly_wal.crash_after_commit = on" -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HE')" \
	-c "\\! $INST/psql -h /tmp -p $PORT -U postgres -d postgres -X -q -c CHECKPOINT" -c "COMMIT" >/dev/null
up
expect "root moved with the commit" "$(root)" "$HE"

echo "== W6. chunks committed, then a checkpoint, then a crash"
HF=$(q -c "BEGIN" -c "SELECT prolly_wal.put_chunk(convert_to(repeat('f', 100000), 'UTF8'))" -c "COMMIT" | head -1)
q -c "CHECKPOINT" >/dev/null
crash
expect "the chunk survives" "$(q -c "SELECT length(prolly_wal.get_chunk('$HF'))")" 100000

echo "== W7. a subtransaction rolled back takes back its root"
q -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HA')" -c "SAVEPOINT s" -c "SELECT prolly_wal.set_root('other', '$HB')" -c "ROLLBACK TO s" -c "COMMIT" >/dev/null
expect "main set" "$(root)" "$HA"
expect "other not" "$(q -c "SELECT coalesce(prolly_wal.get_root('other'), '-')")" "-"

echo "== W8. two transactions on one root move it in commit order"
( q -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HB')" -c "SELECT pg_sleep(1)" -c "COMMIT" >/dev/null ) &
sleep 0.3
t0=$(date +%s%N)
q -c "BEGIN" -c "SELECT prolly_wal.set_root('main', '$HC')" -c "COMMIT" >/dev/null
waited=$(( ($(date +%s%N) - t0) / 1000000 ))
wait
expect "the second waited for the first" "$([ $waited -gt 400 ] && echo yes || echo "no ($waited ms)")" yes
expect "the last to commit wins" "$(root)" "$HC"

echo "== W9. the records in the WAL"
# pg_waldump cannot load an extension's description code: the records show
# as custom128, UNKNOWN
for seg in $(ls "$DATA/pg_wal" | grep -E '^[0-9A-F]{24}$'); do
	"$INST/pg_waldump" -p "$DATA/pg_wal" "$seg" 2>/dev/null | grep -m1 custom128
done | head -1 | sed 's/len.*desc:/... desc:/; s/^/  /'
"$INST/pg_ctl" -D "$DATA" -w stop -m fast >/dev/null
exit $fail
