#!/bin/bash
# Version control on the history store at scale: what a commit, a branch
# made, a log, a diff and a merge cost as the commits and branches held
# grow (1,000 to MAX of each).  Commits set one of a fixed set of rows in
# turn (KEYS), so the table stays the same size as history grows: a cost
# that grows with history shows as one that grows down the rows.  Each
# commit's and branch's figure is the average of the last batch (BATCH
# operations, one transaction a batch, as a session committing would); a
# log's, a diff's and a merge's the median of three, after a checkpoint,
# in a session that has done one already (a backend's first use of the
# store, its relations opened and their caches filled, costs more than the
# operation: a session a measure would time that).
#   usage: vc_scale.sh BIN_DIR NEW_DATA_DIR [MAX_COMMITS [MAX_BRANCHES [KEYS]]]
# BIN_DIR is an installed build's bin, with test_prolly installed;
# NEW_DATA_DIR must not exist (a cluster is made there, on port 55495).
set -eu
B=$1
D=$2
MAXC=${3:-1000000}
MAXB=${4:-1000000}
KEYS=${5:-1000}
BATCH=1000
P=55495
test -e "$D" && { echo "$D exists"; exit 1; }
"$B/initdb" -D "$D" -A trust >/dev/null 2>&1
printf "port = $P\nautovacuum = off\nchunk_free_in_background = off\nmax_wal_size = 8GB\nshared_buffers = 1GB\n" >> "$D/postgresql.conf"
"$B/pg_ctl" -D "$D" -l "$D/log" -w start >/dev/null
# (stopped however the script ends)
trap '"$B/pg_ctl" -D "$D" -m fast stop >/dev/null 2>&1' EXIT
q() { "$B/psql" -X -q -At -p $P -d postgres "$@"; }
mb() { q -c "SELECT round(($1) / 1048576.0, 1)"; }
f() { printf "%.1f" "$1"; }

q -c "CREATE EXTENSION test_prolly" >/dev/null
q -c "SELECT test_vc_open()" >/dev/null
q -c "SELECT test_vc_commit('main', 'base')" >/dev/null
# (a branch at the start: the diff of main against it spans every row set)
q -c "SELECT test_vc_branch('start', 'main')" >/dev/null

echo "commits held   us/commit   log of 100 (us)   diff vs start (us, rows)   merge (us, gets, puts)   store MB"
held=0
next=1000
while [ $held -lt "$MAXC" ]; do
	us=$(q -c "SELECT test_vc_bench_commits('main', $BATCH, $KEYS)")
	held=$((held + BATCH))
	if [ $held -ge $next ]; then
		# (the batches' pages written out first: a measure taken as the
		# checkpointer writes them would time that)
		q -c "CHECKPOINT"
		# (four statements in one session, each its own call: the last three)
		log=$(for i in 1 2 3 4; do echo "SELECT test_vc_bench_log('main', 100);"; done | q | tail -3 | sort -n | sed -n 2p)
		diff=$(for i in 1 2 3 4; do echo "SELECT round(us), rows FROM test_vc_bench_diff('start', 'main');"; done |
			q -F' ' | tail -3 | sort -n | sed -n 2p)
		# (merges of branches made now, one commit on each side apart)
		# (one session for the three: the merge timed the last of what it runs)
		merge=$(for i in 1 2 3; do
			echo "SELECT test_vc_branch('m$held-$i', 'main');"
			echo "SELECT test_vc_bench_edit('m$held-$i', $KEYS + $i * 2, 1, 1, $held);"
			echo "SELECT test_vc_bench_edit('main', $KEYS + $i * 2 + 1, 1, 1, $held);"
			echo "SELECT 'MERGE', round(us), gets, puts FROM test_vc_bench_merge('main', 'm$held-$i');"
		done | q -F' ' | sed -n 's/^MERGE //p' | sort -n | sed -n 2p)
		store=$(mb "pg_relation_size('pg_history.chunks') + pg_relation_size('pg_history.chunk_index')")
		printf "%12d   %9s   %15s   %24s   %22s   %8s\n" $held "$(f $us)" "$(f $log)" "$diff" "$merge" "$store"
		next=$((next * 10))
	fi
done

echo
echo "branches held   us/branch"
made=0
next=1000
while [ $made -lt "$MAXB" ]; do
	us=$(q -c "SELECT test_vc_bench_branches('b', $made, $BATCH)")
	made=$((made + BATCH))
	if [ $made -ge $next ]; then
		printf "%13d   %9s\n" $made "$(f $us)"
		next=$((next * 10))
	fi
done
# (and a commit with that many branches held)
us=$(q -c "SELECT test_vc_bench_commits('main', $BATCH, $KEYS)")
echo "a commit with $made branches held: $(f $us) us"

