#!/bin/bash
# One deep history on the history store: a table of ROWS rows, COMMITS
# commits of CHANGES random row updates each, all on main, all held (the
# window deeper than the history).  At each milestone (1k, 10k, 100k, 1M
# ...) the interval's commit and sever cost, and: a log of 100, the merge
# bases with a branch one commit off ten back and with one off the first
# commit, row diffs to the commits 1, 1,000 and half the history back, the
# committing backend's private memory (not the shared buffers it touched)
# and the store's size.  A sever (a full
# pass over the window) every 1,000 commits to 10,000, then every 10,000.
#   usage: vc_deep.sh BIN_DIR NEW_DATA_DIR [COMMITS [ROWS [CHANGES]]]
# BIN_DIR is an installed build's bin, with test_prolly installed;
# NEW_DATA_DIR must not exist (a cluster is made there, on port 55498).
set -eu
B=$1
D=$2
COMMITS=${3:-1000000}
ROWS=${4:-100000}
CHANGES=${5:-4}
P=55498
test -e "$D" && { echo "$D exists"; exit 1; }
"$B/initdb" -D "$D" -A trust >/dev/null 2>&1
printf "port = $P\nautovacuum = off\nchunk_free_in_background = off\nmax_wal_size = 16GB\nshared_buffers = 1GB\n" >> "$D/postgresql.conf"
"$B/pg_ctl" -D "$D" -l "$D/log" -w start >/dev/null
trap '"$B/pg_ctl" -D "$D" -m fast stop >/dev/null 2>&1' EXIT
q() { "$B/psql" -X -q -At -p $P -d postgres "$@"; }
calc() { awk "BEGIN { print ($1) }"; }

q -c "CREATE EXTENSION test_prolly" -c "SELECT test_vc_open()" >/dev/null
q -c "SELECT test_vc_bench_load('main', $ROWS, 100000)" >/dev/null
q -c "SELECT test_vc_branch('old', 'main')" -c "SELECT test_vc_commit('old', 'old')" >/dev/null
printf "%9s %10s %6s %9s %8s %9s %10s %10s %10s %13s %9s %9s\n" commits held severs sever_ms log100 mb_short mb_long_ms diff1 diff1k diffhalf private_MB store_MB

# (the commits made in batches of 1,000, a statement and transaction each,
# in one session; a coprocess so its state, and peak memory, carry on)
coproc PSQL { "$B/psql" -X -q -At -p $P -d postgres; }
ask() { echo "$1" >&"${PSQL[1]}"; read -r line <&"${PSQL[0]}"; echo "$line"; }
done=0; milestone=1000; cus=0; cn=0; sms=0; sn=0
while [ $done -lt $COMMITS ]; do
	us=$(ask "SELECT test_vc_bench_deep_commits('main', 1000, $CHANGES, $ROWS);")
	done=$((done + 1000)); cus=$(calc "$cus + $us * 1000"); cn=$((cn + 1000))
	every=$(( done <= 10000 ? 1000 : 10000 ))
	if [ $((done % every)) -eq 0 ]; then
		sus=$(ask "SELECT test_vc_bench_sever($((COMMITS + 100)));")
		sms=$(calc "$sms + $sus / 1000"); sn=$((sn + 1))
	fi
	if [ $done -eq $milestone ]; then
		peak=$(ask "SELECT round(test_backend_private_mb()::numeric);")
		probe=$(q -F' ' -c "SELECT held, round(log100_us), round(mb_short_us), round(mb_long_ms::numeric, 1),
			round(diff1_ms::numeric, 1) || '/' || diff1_rows, round(diff1k_ms::numeric, 1) || '/' || diff1k_rows,
			round(diffhalf_ms::numeric, 1) || '/' || diffhalf_rows
			FROM test_vc_bench_deep_probe('main', 'old')")
		store=$(q -c "SELECT (pg_relation_size('pg_history.chunks') + pg_relation_size('pg_history.chunk_index')) / 1048576")
		read held log100 mbs mbl d1 d1k dh <<< "$probe"
		printf "%9d %10s %6d %9.1f %8s %9s %10s %10s %10s %13s %9s %9s   commit_us %.1f\n" $done $held $sn \
			$(calc "$sn > 0 ? $sms / $sn : 0") $log100 $mbs $mbl $d1 $d1k $dh $peak $store \
			$(calc "$cus / $cn")
		cus=0; cn=0; sms=0; sn=0; milestone=$((milestone * 10))
	fi
done
