#!/bin/bash
# One deep history on the history store: a table of ROWS rows, COMMITS
# commits of CHANGES random row updates each, all on main, all held (the
# window deeper than the history).  At each milestone (1k, 10k, 100k, 1M
# ...) and at the end: the interval's cost of a commit, and of a sever (a
# full pass over the window, cutting nothing), every 1,000 commits to 10,000
# and then every 10,000; a log of 100; the merge bases with a branch one
# commit off ten back and with one off the load commit; row diffs to the
# commits 1, 1,000 and half the line back; the committing backend's private
# memory between statements (not a peak: not what a sever allocates); and
# the store's size.
#
# commit_us is a commit inside a transaction of 1,000 of them, without the
# transaction's commit.  with_sever_us adds the interval's severs, spread
# over its commits (the prototype's deepbench's commit_us includes them,
# at a sever every 1,000 commits).
#   usage: vc_deep.sh BIN_DIR NEW_DATA_DIR [COMMITS [ROWS [CHANGES]]]
# BIN_DIR is an installed build's bin, with test_prolly installed;
# NEW_DATA_DIR must not exist (a cluster is made there, on port 55498);
# COMMITS is a multiple of 1,000.
set -eu
B=$1
D=$2
COMMITS=${3:-1000000}
ROWS=${4:-100000}
CHANGES=${5:-4}
P=55498
test $((COMMITS % 1000)) -eq 0 || { echo "COMMITS is not a multiple of 1,000"; exit 1; }
test -e "$D" && { echo "$D exists"; exit 1; }
"$B/initdb" -D "$D" -A trust >/dev/null 2>&1
printf "port = $P\nautovacuum = off\nchunk_free_in_background = off\nmax_wal_size = 16GB\nshared_buffers = 1GB\n" >> "$D/postgresql.conf"
"$B/pg_ctl" -D "$D" -l "$D/log" -w start >/dev/null
trap '"$B/pg_ctl" -D "$D" -m fast stop >/dev/null 2>&1' EXIT
q() { "$B/psql" -X -q -At -v ON_ERROR_STOP=1 -p $P -d postgres "$@"; }
calc() { awk "BEGIN { print ($1) }"; }

q -c "CREATE EXTENSION test_prolly" -c "SELECT test_vc_open()" >/dev/null
q -c "SELECT test_vc_bench_load('main', $ROWS, 100000)" >/dev/null
q -c "SELECT test_vc_branch('old', 'main')" -c "SELECT test_vc_commit('old', 'old')" >/dev/null
printf "%9s %10s %9s %13s %6s %9s %8s %9s %10s %9s %10s %13s %10s %9s\n" commits held commit_us with_sever_us \
	severs sever_ms log100 mb_short mb_long_ms diff1 diff1k diffhalf private_MB store_MB

# (the commits made in batches of 1,000, a statement and transaction each,
# in one session, its memory measured; an error ends it, and so the script)
coproc PSQL { "$B/psql" -X -q -At -v ON_ERROR_STOP=1 -p $P -d postgres; }
ask() {
	echo "$1" >&"${PSQL[1]}"
	IFS= read -r line <&"${PSQL[0]}" || { echo "the session ended: $1" >&2; exit 1; }
	echo "$line"
}
done=0; milestone=1000; cus=0; cn=0; sms=0; sn=0
while [ $done -lt $COMMITS ]; do
	us=$(ask "SELECT test_vc_bench_deep_commits('main', 1000, $CHANGES, $ROWS);")
	done=$((done + 1000)); cus=$(calc "$cus + $us * 1000"); cn=$((cn + 1000))
	every=$(( done <= 10000 ? 1000 : 10000 ))
	if [ $((done % every)) -eq 0 ]; then
		sus=$(ask "SELECT test_vc_bench_sever($((COMMITS + 100)));")
		sms=$(calc "$sms + $sus / 1000"); sn=$((sn + 1))
	fi
	if [ $done -eq $milestone ] || [ $done -eq $COMMITS ]; then
		private=$(ask "SELECT coalesce(round(test_backend_private_mb()::numeric), -1);")
		probe=$(q -F' ' -c "SELECT held, round(log100_us), round(mb_short_us), round(mb_long_ms::numeric, 1),
			round(diff1_ms::numeric, 1) || '/' || diff1_rows, round(diff1k_ms::numeric, 1) || '/' || diff1k_rows,
			round(diffhalf_ms::numeric, 1) || '/' || diffhalf_rows
			FROM test_vc_bench_deep_probe('main', 'old')")
		store=$(q -c "SELECT (pg_relation_size('pg_history.chunks') + pg_relation_size('pg_history.chunk_index')) / 1048576")
		read -r held log100 mbs mbl d1 d1k dh <<< "$probe"
		printf "%9d %10s %9.1f %13.1f %6d %9.1f %8s %9s %10s %9s %10s %13s %10s %9s\n" $done "$held" \
			"$(calc "$cus / $cn")" "$(calc "($cus + $sms * 1000) / $cn")" $sn \
			"$(calc "$sn > 0 ? $sms / $sn : 0")" "$log100" "$mbs" "$mbl" "$d1" "$d1k" "$dh" "$private" "$store"
		cus=0; cn=0; sms=0; sn=0
		[ $done -eq $milestone ] && milestone=$((milestone * 10))
	fi
done
