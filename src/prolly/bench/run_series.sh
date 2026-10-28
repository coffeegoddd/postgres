#!/bin/sh
# Runs the window benchmark series one after another (they share table t).
#
#   run_series.sh size     database size: 1M, 10M, 100M, 450M rows (~85 GB)
#   run_series.sh branch   branch count: 1, 8, 32 branches on 10M rows
#   run_series.sh window   window size: 10, 100 commits on 10M rows
#
# Results go to $OUT (default ./results); summarize with summarize.py.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
W=$HERE/winbench
PGDATA=${PGDATA:-/home/dustin/pgv2-data}
OUT=${OUT:-results}
mkdir -p "$OUT"

run() {
	name=$1; shift
	echo "== $name: $*"
	"$W" --pgdata "$PGDATA" "$@" > "$OUT/$name.csv" 2>&1 || {
		echo "FAILED $name"; tail -5 "$OUT/$name.csv"; exit 1; }
	python3 "$HERE/summarize.py" "$OUT/$name.csv"
}

case "$1" in
size)
	for rows in 1000000 10000000; do
		run size_$rows --rows $rows --commits 20 --changes 2000 --window 10 \
			--checkouts 20 --branches 2 --branch-commits 3 --verify 1
	done
	for rows in 100000000 450000000; do
		run size_$rows --rows $rows --commits 20 --changes 2000 --window 10 \
			--checkouts 20 --branches 2 --branch-commits 3 --verify 1 --no-final-verify
	done
	;;
branch)
	for b in 1 8 32; do
		run branch_$b --rows 10000000 --commits 20 --changes 2000 --window 10 \
			--checkouts 30 --branches $b --branch-commits 5 --verify 1 --no-sql
	done
	;;
window)
	for w in 10 100; do
		run window_$w --rows 10000000 --commits 120 --changes 1000 --window $w \
			--checkouts 40 --verify 1 --no-sql --no-final-verify
	done
	;;
*)
	echo "usage: $0 size|branch|window" >&2; exit 2 ;;
esac
