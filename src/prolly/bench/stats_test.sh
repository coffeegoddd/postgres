#!/bin/bash
# After a page restore the planner's statistics no longer describe the
# table: the restore counts the tuples it wrote as inserted, so the
# table's change counter rises and autoanalyze takes it up, as it does
# after a move made of row changes.  Against the running test server.
set -u
B=${PGBIN:-/home/dustin/cursor_src/pg-v2-build/inst/bin}
q() { "$B/psql" -h /tmp -p 5499 -U postgres -d stats_test -X -q -At "$@"; }
"$B/psql" -h /tmp -p 5499 -U postgres -d postgres -X -q -c "DROP DATABASE IF EXISTS stats_test" -c "CREATE DATABASE stats_test"
mods() { q -c "SELECT n_mod_since_analyze FROM pg_stat_user_tables WHERE relname = 't'"; }
q -c "CREATE EXTENSION pageinspect" -c "CREATE EXTENSION prolly_pages" \
  -c "CREATE TABLE t (id int, v text)" -c "INSERT INTO t SELECT g, repeat('x', 40) FROM generate_series(1, 5000) g" \
  -c "SELECT pg_stat_force_next_flush()" >/dev/null
# the insert's counts reach the shared statistics first, then ANALYZE resets them
for i in $(seq 1 30); do [ "$(mods)" == 5000 ] && break; sleep 0.1; done
q -c "ANALYZE t" -c "SELECT pg_stat_force_next_flush()" >/dev/null
for i in $(seq 1 30); do [ "$(mods)" == 0 ] && break; sleep 0.1; done
[ "$(mods)" == 0 ] || { echo "stats_test: the counter did not settle at 0 after ANALYZE"; exit 1; }
# the table's own pages, written back
q -c "SELECT prolly_put_pages('t', 'main', array_agg(b ORDER BY b), string_agg(get_raw_page('t', b), ''::bytea ORDER BY b)) FROM generate_series(0, (pg_relation_size('t') / 8192 - 1)::int) b" \
  -c "SELECT pg_stat_force_next_flush()" >/dev/null
for i in $(seq 1 30); do [ "$(mods)" -ge 5000 ] && break; sleep 0.1; done
got=$(mods)
if [ "$got" -ge 5000 ]; then echo "stats_test: a page restore of 5000 tuples counts $got changes"; else echo "stats_test: FAIL, $got changes counted after restoring 5000 tuples"; exit 1; fi
