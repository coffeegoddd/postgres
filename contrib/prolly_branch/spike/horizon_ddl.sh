#!/bin/bash
# Experiment A: the freeze horizon with the whole database frozen, and a
# definition that differs between branches.
set -u
B=/home/dustin/cursor_src/pg-v2-build/inst/bin
P=(-h /tmp -p 5499 -U postgres -d spike_a -X -q -At)
q() { "$B/psql" "${P[@]}" -c "$1"; }

echo "== 4b. every table frozen: the database's horizon passes the parked branch's"
q "SELECT prolly_branch.checkout('bt','main')" >/dev/null
q "UPDATE bt.item SET v = v WHERE id < 50" >/dev/null	# main now holds unfrozen xids
q "SELECT prolly_branch.checkout('bt','b')" >/dev/null	# main is parked
for i in $(seq 1 300); do q "SELECT txid_current()" >/dev/null; done
q "VACUUM (FREEZE)" >/dev/null
q "SELECT 'datfrozenxid ' || datfrozenxid FROM pg_database WHERE datname = 'spike_a'"
q "SELECT 'parked main relfrozenxid ' || relfrozenxid || ', its newest unfrozen xmin ' ||
		  (SELECT max(xmin::text::int8) FROM bt.item) FROM prolly_branch.files
	WHERE branch = 'main' AND relid = 'bt.item'::regclass" 2>&1 | sed 's/its newest unfrozen xmin.*/(xmins read from b, see below)/'
q "SELECT prolly_branch.checkout('bt','main')" >/dev/null
q "SELECT 'main checked out again: relfrozenxid ' || relfrozenxid || ', unfrozen xmins up to ' || (SELECT max(xmin::text::int8) FROM bt.item) || ', datfrozenxid ' || (SELECT datfrozenxid FROM pg_database WHERE datname = current_database()) FROM pg_class WHERE oid = 'bt.item'::regclass"

echo "== 7. a definition that differs: a rewrite on one branch"
q "CREATE TABLE bt.typed (id int PRIMARY KEY, n int NOT NULL); INSERT INTO bt.typed SELECT g, g * 1000 FROM generate_series(1, 5) g" >/dev/null
q "SELECT prolly_branch.remember('bt'::regnamespace)" >/dev/null
q "SELECT prolly_branch.create_branch('bt', 'e')" >/dev/null
q "ALTER TABLE bt.typed ALTER COLUMN n TYPE text USING 'n=' || n" >/dev/null
q "SELECT 'main after the rewrite: ' || string_agg(n, ',' ORDER BY id) FROM bt.typed"
q "SELECT prolly_branch.checkout('bt','e')" 2>&1 | head -2
q "SELECT 'e read under main''s definition: ' || string_agg(coalesce(n, 'NULL'), ',' ORDER BY id) FROM bt.typed" 2>&1 | head -2
q "SELECT prolly_branch.checkout('bt','main')" >/dev/null 2>&1
