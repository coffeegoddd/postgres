#!/bin/bash
# Experiment C: catalogs branched by row.  A versioned schema has one
# pg_namespace row per branch (nspbranch), each branch's objects live in its
# own namespace row, and a session's catalog_branch picks which it sees.
# Needs a build with spike/catalog_branch.patch applied (and its own
# cluster, made here, since the patch changes the catalog).
set -u
INST=/home/dustin/cursor_src/pg-v2-cat/inst/bin
DATA=/home/dustin/pgv2-bench/catdb2	# made by initdb here, which runs the patched bootstrap
PORT=5496
fail=0
expect() { if [ "$2" == "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: got [$2] want [$3]"; fail=1; fi; }
on() {	# branch sql...: psql on that branch
	local b=$1; shift
	PGOPTIONS="-c catalog_branch=$b" "$INST/psql" -h /tmp -p $PORT -U postgres -d spike_c -X -q -At -v ON_ERROR_STOP=1 "$@" 2>&1
}

if [ ! -d "$DATA" ]; then
	"$INST/initdb" -D "$DATA" -U postgres --no-sync >/dev/null
	echo "port = $PORT" >> "$DATA/postgresql.conf"
	echo "wal_level = logical" >> "$DATA/postgresql.conf"
fi
"$INST/pg_ctl" -D "$DATA" -l "$DATA/server.log" -w start >/dev/null 2>&1
"$INST/psql" -h /tmp -p $PORT -U postgres -d postgres -X -q -c "DROP DATABASE IF EXISTS spike_c" -c "CREATE DATABASE spike_c"
"$INST/psql" -h /tmp -p $PORT -U postgres -d postgres -X -q -c "DROP ROLE IF EXISTS owner2" -c "CREATE ROLE owner2"

echo "== setup: a shared schema, and a versioned schema on branch 1"
on 1 <<'SQL'
CREATE SCHEMA pub;
CREATE TABLE pub.p (id int PRIMARY KEY, note text);
INSERT INTO pub.p VALUES (1, 'shared');
CREATE SCHEMA app;
SET allow_system_table_mods = on;
UPDATE pg_namespace SET nspbranch = 1 WHERE nspname = 'app';
RESET allow_system_table_mods;
CREATE TYPE app.mood AS ENUM ('ok', 'sad');
CREATE TABLE app.t (id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v int NOT NULL,
					m app.mood NOT NULL DEFAULT 'ok', p int REFERENCES pub.p);
CREATE INDEX t_v ON app.t (v);
CREATE VIEW app.tv AS SELECT id, v FROM app.t WHERE v > 10;
CREATE FUNCTION app.f(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';
INSERT INTO app.t (v, p) SELECT g, 1 FROM generate_series(1, 100) g;
SQL

echo "== branch 2: the versioned schema dumped from branch 1, restored on branch 2"
PGOPTIONS="-c catalog_branch=1" "$INST/pg_dump" -h /tmp -p $PORT -U postgres -n app spike_c > /tmp/claude-1000/spike_c_app.sql
{ cat /tmp/claude-1000/spike_c_app.sql
  echo "SET allow_system_table_mods = on;"
  echo "UPDATE pg_namespace SET nspbranch = 2 WHERE oid = 'app'::regnamespace;"
} | on 2 >/dev/null
expect "the restore went in" "$(on 2 -c "SELECT count(*) FROM app.t")" 100

echo "== C1. each branch sees its own definitions and rows"
on 2 -c "ALTER TABLE app.t ADD COLUMN extra int DEFAULT 9; INSERT INTO app.t (v, p) SELECT g, 1 FROM generate_series(1, 50) g; ALTER TYPE app.mood ADD VALUE 'glad'" >/dev/null
on 1 -c "DELETE FROM app.t WHERE v > 90" >/dev/null
expect "rows on 1" "$(on 1 -c "SELECT count(*) FROM app.t")" 90
expect "rows on 2" "$(on 2 -c "SELECT count(*) FROM app.t")" 150
expect "columns on 1 (information_schema)" "$(on 1 -c "SELECT string_agg(column_name, ',' ORDER BY ordinal_position) FROM information_schema.columns WHERE table_schema = 'app' AND table_name = 't'")" "id,v,m,p"
expect "columns on 2 (information_schema)" "$(on 2 -c "SELECT string_agg(column_name, ',' ORDER BY ordinal_position) FROM information_schema.columns WHERE table_schema = 'app' AND table_name = 't'")" "id,v,m,p,extra"
expect "enum on 1" "$(on 1 -c "SELECT enum_range(NULL::app.mood)")" "{ok,sad}"
expect "enum on 2" "$(on 2 -c "SELECT enum_range(NULL::app.mood)")" "{ok,sad,glad}"
expect "\\d on 2 shows extra" "$(on 2 -c '\d app.t' | grep -c extra)" 1
expect "\\d on 1 does not" "$(on 1 -c '\d app.t' | grep -c extra)" 0
expect "one app schema listed on 1" "$(on 1 -c "SELECT count(*) FROM pg_namespace WHERE nspname = 'app'")" 1
expect "pg_dump on 1: no extra" "$(PGOPTIONS='-c catalog_branch=1' "$INST/pg_dump" -h /tmp -p $PORT -U postgres -n app -s spike_c 2>&1 | grep -c 'extra\|error')" 0
expect "pg_dump on 2: extra" "$(PGOPTIONS='-c catalog_branch=2' "$INST/pg_dump" -h /tmp -p $PORT -U postgres -n app -s spike_c 2>&1 | grep -c 'extra integer')" 1

echo "== C2. DDL on both branches at once"
( on 1 -c "BEGIN; CREATE INDEX t_m1 ON app.t (m); SELECT pg_sleep(1); COMMIT;" >/dev/null ) &
( on 2 -c "BEGIN; CREATE INDEX t_m2 ON app.t (m, extra); SELECT pg_sleep(1); COMMIT;" >/dev/null ) &
wait
expect "1's indexes" "$(on 1 -c "SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes WHERE schemaname = 'app'")" "t_m1,t_pkey,t_v"
expect "2's indexes" "$(on 2 -c "SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes WHERE schemaname = 'app'")" "t_m2,t_pkey,t_v"
expect "the same name made on both branches (CREATE TABLE app.only)" "$(on 1 -c "CREATE TABLE app.only (x int)" && on 2 -c "CREATE TABLE app.only (y int)" && echo made)" made

echo "== C3. the shared schema is shared"
on 2 -c "INSERT INTO pub.p VALUES (2, 'from 2')" >/dev/null
expect "1 sees 2's write to pub" "$(on 1 -c "SELECT count(*) FROM pub.p")" 2
expect "a foreign key from 2 into pub holds" "$(on 2 -c "INSERT INTO app.t (v, p) VALUES (1, 99)" | grep -c 'violates foreign key')" 1

echo "== C4. a session on no branch sees no versioned schema"
expect "branch 0" "$(on 0 -c "SELECT count(*) FROM app.t" | grep -c 'does not exist')" 1

echo "== C5. switching branch inside a session"
expect "SET catalog_branch" "$(on 1 -c "SELECT count(*) FROM app.t" -c "SET catalog_branch = 2" -c "SELECT count(*) FROM app.t" | tr '\n' ' ')" "90 150 "
expect "a prepared statement across a switch" "$(on 1 -c "PREPARE q AS SELECT count(*) FROM app.t" -c "EXECUTE q" -c "SET catalog_branch = 2" -c "EXECUTE q" | tr '\n' ' ')" "90 150 "

echo "== C6. other catalogs: queries see one branch, lookups by id see all"
expect "pg_class rows named t, from 1" "$(on 1 -c "SELECT count(*) FROM pg_class WHERE relname = 't'")" 1
expect "pg_type rows named mood, from 2" "$(on 2 -c "SELECT count(*) FROM pg_type WHERE typname = 'mood'")" 1
expect "pg_proc rows named f, from 2" "$(on 2 -c "SELECT count(*) FROM pg_proc WHERE proname = 'f'")" 1
expect "pg_constraint rows on app tables, from 1" "$(on 1 -c "SELECT count(*) FROM pg_constraint WHERE conname = 't_pkey'")" 1
expect "a session on no branch sees none of them" "$(on 0 -c "SELECT count(*) FROM pg_class WHERE relname = 't'")" 0
expect "with the filter off, both" "$(on 1 -c "SET catalog_branch_filter = off" -c "SELECT count(*) FROM pg_class WHERE relname = 't'")" 2
expect "a lookup by id finds the other branch's table" "$(on 1 -c "SET catalog_branch_filter = off" -c "SELECT c.oid FROM pg_class c WHERE relname = 't' AND relnamespace <> 'app'::regnamespace" | tail -1 | xargs -I{} env PGOPTIONS='-c catalog_branch=1' "$INST/psql" -h /tmp -p $PORT -U postgres -d spike_c -X -qAt -c "SELECT {}::regclass::text <> '{}'")" t
echo "== C7. VACUUM: each branch's table keeps its own numbers; the horizon sees both"
on 1 -c "VACUUM app.t" >/dev/null; on 2 -c "VACUUM app.t" >/dev/null
echo "  reltuples by branch: 1 $(on 1 -c "SELECT reltuples::int FROM pg_class WHERE oid = 'app.t'::regclass"), 2 $(on 2 -c "SELECT reltuples::int FROM pg_class WHERE oid = 'app.t'::regclass")"
expect "datfrozenxid is the min over both branches' tables" "$(on 1 -c "SELECT (SELECT datfrozenxid FROM pg_database WHERE datname = current_database())::text::int8 <= min(relfrozenxid::text::int8) FROM pg_class WHERE relkind = 'r' AND relfrozenxid <> '0'")" t

echo "== C8. logical decoding: a stream on a branch carries that branch's changes"
on 1 -c "SELECT 1 FROM pg_create_logical_replication_slot('spike_c', 'test_decoding')" >/dev/null
on 1 -c "INSERT INTO app.t (v, p) VALUES (501, 1)" >/dev/null
on 2 -c "INSERT INTO app.t (v, p) VALUES (502, 1)" >/dev/null
on 2 -c "INSERT INTO pub.p VALUES (3, 'shared, from 2')" >/dev/null
peek() { on "$1" -c "SELECT string_agg(coalesce(substring(data from 'v\[integer\]:([0-9]+)'), substring(data from 'note\[text\]:''([a-z]+)')), ',' ORDER BY lsn) FROM pg_logical_slot_peek_changes('spike_c', NULL, NULL) WHERE data LIKE 'table%'"; }
expect "on branch 1: its row and the shared one" "$(peek 1)" "501,shared"
expect "on branch 2: its row and the shared one" "$(peek 2)" "502,shared"
expect "on no branch: all of them" "$(peek 0)" "501,502,shared"
on 0 -c "SELECT pg_drop_replication_slot('spike_c')" >/dev/null

echo "== C9. a role owning objects on one branch"
on 2 -c "ALTER TABLE app.only OWNER TO owner2" >/dev/null
echo "  DROP ROLE from branch 1: $(on 1 -c "DROP ROLE owner2" | head -3 | tr '\n' ' ')"
echo "  DROP ROLE from branch 0: $(on 0 -c "DROP ROLE owner2" | head -3 | tr '\n' ' ')"

echo "== C11. autovacuum visits every branch's tables"
"$INST/psql" -h /tmp -p $PORT -U postgres -d postgres -X -q -c "ALTER SYSTEM SET autovacuum_naptime = 1" -c "ALTER SYSTEM SET autovacuum_vacuum_threshold = 10" -c "ALTER SYSTEM SET autovacuum_vacuum_scale_factor = 0" -c "SELECT pg_reload_conf()" >/dev/null
on 1 -c "DELETE FROM app.t WHERE v <= 40" >/dev/null
on 2 -c "DELETE FROM app.t WHERE v <= 30" >/dev/null
vacuumed() { on "$1" -c "SELECT last_autovacuum IS NOT NULL FROM pg_stat_user_tables WHERE schemaname = 'app' AND relname = 't'"; }
for i in $(seq 1 30); do [ "$(vacuumed 1)$(vacuumed 2)" == "tt" ] && break; sleep 1; done
expect "branch 1's t autovacuumed" "$(vacuumed 1)" t
expect "branch 2's t autovacuumed" "$(vacuumed 2)" t
expect "each branch's dead rows gone" "$(on 1 -c "SELECT n_dead_tup FROM pg_stat_user_tables WHERE schemaname = 'app' AND relname = 't'")/$(on 2 -c "SELECT n_dead_tup FROM pg_stat_user_tables WHERE schemaname = 'app' AND relname = 't'")" "0/0"
"$INST/psql" -h /tmp -p $PORT -U postgres -d postgres -X -q -c "ALTER SYSTEM RESET ALL" -c "SELECT pg_reload_conf()" >/dev/null

echo "== C12. temporary tables on a branch"
expect "a temp table on branch 2" "$(on 2 -c "CREATE TEMP TABLE scratch (x int)" -c "INSERT INTO scratch VALUES (1), (2)" -c "SELECT count(*) FROM scratch" -c "SELECT count(*) FROM pg_class WHERE relname = 'scratch'" | tr '\n' ' ')" "2 1 "
expect "a temp table may share a versioned table's name" "$(on 1 -c "CREATE TEMP TABLE t (x int)" -c "SELECT count(*) FROM pg_temp.t" -c "SELECT count(*) FROM app.t" | tr '\n' ' ')" "0 51 "

echo "== C13. extensions: shared, or one per branch"
expect "an extension in a shared schema serves both branches" "$(on 1 -c "CREATE EXTENSION hstore SCHEMA pub" >/dev/null; on 2 -c "SELECT ('a=>1'::pub.hstore) OPERATOR(pub.->) 'a'")" 1
expect "a branch may not have its own while a shared one exists" "$(on 2 -c "CREATE EXTENSION hstore SCHEMA app" | grep -c 'already exists')" 1
on 1 -c "DROP EXTENSION hstore" >/dev/null
expect "branch 1 installs its own, at 1.6" "$(on 1 -c "CREATE EXTENSION pg_trgm SCHEMA app" -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_trgm'")" 1.6
expect "branch 2 installs its own, at 1.5" "$(on 2 -c "CREATE EXTENSION pg_trgm VERSION '1.5' SCHEMA app" -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_trgm'")" 1.5
expect "each branch uses its own" "$(on 1 -c "SELECT round(app.similarity('abc', 'abd')::numeric, 2)")/$(on 2 -c "SELECT round(app.similarity('abc', 'abd')::numeric, 2)")" "0.33/0.33"
expect "one pg_trgm listed on each branch" "$(on 1 -c "SELECT count(*) FROM pg_extension WHERE extname = 'pg_trgm'")/$(on 2 -c "SELECT count(*) FROM pg_extension WHERE extname = 'pg_trgm'")" "1/1"
expect "branch 2 updates its own" "$(on 2 -c "ALTER EXTENSION pg_trgm UPDATE TO '1.6'" -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_trgm'")" 1.6
expect "a shared one is refused while a branch has its own" "$(on 0 -c "CREATE EXTENSION pg_trgm SCHEMA pub" | grep -c 'already exists')" 1
expect "a session on no branch sees neither" "$(on 0 -c "SELECT count(*) FROM pg_extension WHERE extname = 'pg_trgm'")" 0
on 2 -c "DROP EXTENSION pg_trgm" >/dev/null
expect "dropping branch 2's leaves branch 1's" "$(on 1 -c "SELECT count(*) FROM pg_extension WHERE extname = 'pg_trgm'")/$(on 2 -c "SELECT count(*) FROM pg_extension WHERE extname = 'pg_trgm'")" "1/0"
expect "branch 1's still works" "$(on 1 -c "SELECT round(app.similarity('abc', 'abd')::numeric, 2)")" 0.33

echo "== C10. dropping a branch's schema leaves the other"
on 2 -c "DROP SCHEMA app CASCADE" >/dev/null
expect "2 has no app" "$(on 2 -c "SELECT count(*) FROM app.t" | grep -c 'does not exist')" 1
expect "1 still has it" "$(on 1 -c "SELECT count(*) FROM app.t")" 51
"$INST/pg_ctl" -D "$DATA" -w stop -m fast >/dev/null
exit $fail
