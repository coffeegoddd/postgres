#!/bin/bash
# Version-control operations on Dolt itself, the same workload as
# winbench --lifecycle runs on the prototype: a table t(id bigint primary
# key, k int, v int, pad text) with an index on k, ROWS rows, and CHANGES
# row changes a commit in 10 transactions (half updates of k, a tenth
# updates of pad, a fifth inserts, a fifth deletes).  Times each Dolt
# commit, diffs of the working state against commits in history, switches
# between branch heads and resets to commits in history, through
# dolt sql-server and the mysql client.
#   dolt_vc_ops.sh [ROWS] [CHANGES] [COMMITS]
set -u
ROWS=${1:-1000000}
CHANGES=${2:-1000}
COMMITS=${3:-10}
DIR=/home/dustin/pgv2-bench/doltvc/db_$$
PORT=3311
mkdir -p "$DIR"
cd "$DIR"
dolt init --name bench --email bench@example.com >/dev/null
dolt sql-server --host 127.0.0.1 --port $PORT > "$DIR.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; wait $SERVER 2>/dev/null' EXIT
for i in $(seq 1 100); do mysql -h 127.0.0.1 -P $PORT -u root -e "SELECT 1" >/dev/null 2>&1 && break; sleep 0.2; done
DB=$(basename "$DIR")
q() { mysql -h 127.0.0.1 -P $PORT -u root -D "$DB" -N -B -e "$1"; }
ms() { echo $(( $(date +%s%N) / 1000000 )); }
median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
NEXT=$((ROWS + 1))
SEQ=0
rnd_ids() { shuf -i 1-$((NEXT - 1)) -n $1 | paste -sd, -; }

workload() {	# CHANGES changes in 10 transactions
	local per=$((CHANGES / 10)) nu np ni nd sql
	nu=$((per * 5 / 10)); np=$((per / 10)); ni=$((per * 2 / 10)); nd=$((per - nu - np - ni))
	for t in $(seq 1 10); do
		SEQ=$((SEQ + 1))
		sql="START TRANSACTION;"
		sql+="UPDATE t SET k = (id * 31 + $SEQ) % 1000003, v = $SEQ WHERE id IN ($(rnd_ids $nu));"
		sql+="UPDATE t SET pad = lpad(concat('u', id, '.', $SEQ), 100, 'y'), v = $SEQ WHERE id IN ($(rnd_ids $np));"
		sql+="INSERT INTO t SELECT g, (g * 7919) % 1000003, $SEQ, lpad(g, 100, 'x') FROM (SELECT $NEXT + n AS g FROM (WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM s WHERE n < $((ni - 1))) SELECT n FROM s) x) y;"
		NEXT=$((NEXT + ni))
		sql+="DELETE FROM t WHERE id IN ($(rnd_ids $nd));"
		sql+="COMMIT;"
		q "$sql"
	done
}

q "CREATE TABLE t (id bigint PRIMARY KEY, k int, v int, pad text, KEY t_k (k))"
q "SET cte_max_recursion_depth = 2000000; INSERT INTO t WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < $ROWS) SELECT n, (n * 7919) % 1000003, 0, lpad(n, 100, 'x') FROM s"
q "CALL dolt_commit('-Am', 'load')" >/dev/null
echo "# Dolt $(dolt version | head -1 | awk '{print $3}'): $(q "SELECT count(*) FROM t") rows, $CHANGES changes a commit"

C=(); for i in $(seq 1 $COMMITS); do workload; t0=$(ms); q "CALL dolt_commit('-am', 'main $i')" >/dev/null; C+=($(( $(ms) - t0 ))); done
printf "  %-44s %7s ms\n" "commit (median of $COMMITS)" "$(printf '%s\n' "${C[@]}" | median)"

# two branches from commits in history, a few commits each
for b in 0 1; do
	q "CALL dolt_branch('b$b', 'HEAD~$((2 + b * 3))')" >/dev/null
	for i in 1 2 3; do q "CALL dolt_checkout('b$b'); UPDATE t SET v = v + 1 WHERE id IN ($(rnd_ids $CHANGES)); CALL dolt_commit('-am', 'b$b $i')" >/dev/null; done
done

D=(); for i in 1 2 3 4 5; do workload; back=$((1 + RANDOM % 8)); t0=$(ms); q "SELECT count(*) FROM dolt_diff('HEAD~$back', 'WORKING', 't')" >/dev/null; D+=($(( $(ms) - t0 ))); done
printf "  %-44s %7s ms\n" "diff WORKING against a commit (median of 5)" "$(printf '%s\n' "${D[@]}" | median)"

S=(); for i in 1 2 3 4 5; do t0=$(ms); q "CALL dolt_checkout('b$((i % 2))'); SELECT count(*) FROM t WHERE id = 1" >/dev/null; S+=($(( $(ms) - t0 ))); done
printf "  %-44s %7s ms\n" "switch to a branch head (median of 5)" "$(printf '%s\n' "${S[@]}" | median)"

R=(); for i in 1 2 3 4 5; do
	target=$(q "SELECT commit_hash FROM dolt_log LIMIT 1 OFFSET $((1 + RANDOM % 8))")
	t0=$(ms); q "CALL dolt_reset('--hard', '$target')" >/dev/null; R+=($(( $(ms) - t0 )))
done
printf "  %-44s %7s ms\n" "reset to a commit in history (median of 5)" "$(printf '%s\n' "${R[@]}" | median)"
echo "  (each figure includes the mysql client's connection, a few ms)"
