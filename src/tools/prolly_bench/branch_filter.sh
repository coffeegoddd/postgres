#!/bin/bash
#
# branch_filter.sh: what the catalog filter costs a session at many
# branches of one versioned schema.
#
# Makes BRANCHES branches (default 1000), each with a versioned schema of
# FUNCTIONS functions and TABLES tables (defaults 1000 and 20), in a
# cluster of its own, then times, in one session on the first branch,
# full scans of pg_proc and pg_attribute: the first (which learns each
# object it meets), a second, one after a session on another branch
# created an object, and one after this session changed branch, and an
# unfiltered one for comparison.  The second and the two after it should
# cost about the same: the filter keeps what it learned, and nothing is
# rebuilt.
#
# usage: branch_filter.sh BINDIR WORKDIR
#
# src/tools/prolly_bench/branch_filter.sh

set -eu
BIN=$1
WORK=$2
BRANCHES=${BRANCHES:-1000}
FUNCTIONS=${FUNCTIONS:-1000}
TABLES=${TABLES:-20}
PORT=${PORT:-5498}
DATA=$WORK/branch_filter_data
SOCK=$WORK

mkdir -p "$WORK"
if [ -e "$DATA" ]; then
	echo "$DATA exists; remove it or choose another WORKDIR" >&2
	exit 1
fi
"$BIN/initdb" -D "$DATA" -U postgres --no-sync >/dev/null
cat >> "$DATA/postgresql.conf" <<CONF
port = $PORT
unix_socket_directories = '$SOCK'
listen_addresses = ''
fsync = off
max_locks_per_transaction = 4096
CONF
"$BIN/pg_ctl" -D "$DATA" -l "$DATA/log" -w start >/dev/null
trap '"$BIN/pg_ctl" -D "$DATA" -w stop -m fast >/dev/null' EXIT

psql() { "$BIN/psql" -h "$SOCK" -p "$PORT" -U postgres -d postgres -X -q -At "$@"; }

start=$(date +%s)
for b in $(seq 1 "$BRANCHES"); do
	psql -c "SELECT pg_branch_id_allocate()" >/dev/null
	PGOPTIONS="-c catalog_branch=$b" psql -c "
		CREATE SCHEMA app VERSIONED;
		DO \$\$BEGIN
			FOR i IN 1..$FUNCTIONS LOOP
				EXECUTE format('CREATE FUNCTION app.f%s(int) RETURNS int LANGUAGE sql IMMUTABLE RETURN \$1 + %s', i, i);
			END LOOP;
			FOR i IN 1..$TABLES LOOP
				EXECUTE format('CREATE TABLE app.t%s (a int, b text, c int, d text)', i);
			END LOOP;
		END\$\$;"
done
echo "made $BRANCHES branches of $FUNCTIONS functions and $TABLES tables in $(( $(date +%s) - start )) s"
psql -c "SET catalog_branch_filter = off" -c "SELECT 'pg_proc rows ' || count(*) FROM pg_proc" \
	-c "SELECT 'pg_attribute rows ' || count(*) FROM pg_attribute"

QUERY="SELECT count(*) FROM pg_proc; SELECT count(*) FROM pg_attribute;"
timed() {	# label: run QUERY in the open session, report milliseconds
	echo "\\echo $1"
	echo "\\timing on"
	echo "$QUERY"
	echo "\\timing off"
}
{
	# (parallel workers would each learn the objects anew)
	echo "SET max_parallel_workers_per_gather = 0;"
	timed "first scans on branch 1"
	timed "again"
	echo "\\! PGOPTIONS='-c catalog_branch=2' $BIN/psql -h $SOCK -p $PORT -U postgres -d postgres -X -q -c 'CREATE FUNCTION app.extra() RETURNS int LANGUAGE sql RETURN 1'"
	timed "after a function made on branch 2"
	echo "SET catalog_branch = 3;"
	timed "after changing to branch 3"
	echo "SET catalog_branch_filter = off;"
	timed "unfiltered: every branch"
} | PGOPTIONS="-c catalog_branch=1" psql 2>&1
