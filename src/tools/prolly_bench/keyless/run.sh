#!/bin/bash
# Run one keyless benchmark script on a fresh server, then report its size.
#
#   run.sh WORKDIR ENGINE SCRIPT [BINDIR]
#
# ENGINE is heap or prolly (a Postgres server from BINDIR, on port 5593 or
# 5594) or dolt (dolt sql-server on port 3399, dolt from PATH).  SCRIPT is
# a script gen.py wrote into WORKDIR, without ".sql".  Each run makes its
# own data directory under WORKDIR and refuses one that exists.  Results go
# to WORKDIR/out-SCRIPT.tsv: step, median ms, each repetition's ms.
set -euo pipefail

if [ $# -lt 3 ]; then
	echo "usage: run.sh WORKDIR ENGINE SCRIPT [BINDIR]" >&2
	exit 2
fi
WORK=$(cd "$1" && pwd)
ENGINE=$2
SCRIPT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$WORK/out-$SCRIPT.tsv

(cd "$HERE" && go build -o "$WORK/kbench" .)

case $ENGINE in
heap|prolly)
	BIN=${4:?"BINDIR (a Postgres install's bin) is needed for $ENGINE"}
	[ "$ENGINE" = heap ] && PORT=5593 || PORT=5594
	DATA=$WORK/pgdata-$SCRIPT
	if [ -e "$DATA" ]; then
		echo "$DATA exists" >&2
		exit 1
	fi
	"$BIN/initdb" -D "$DATA" -U postgres -A trust > /dev/null
	"$BIN/pg_ctl" -D "$DATA" -l "$WORK/server-$SCRIPT.log" -w \
		-o "-p $PORT -k /tmp -c shared_buffers=1GB -c max_wal_size=8GB" start > /dev/null
	trap '"$BIN/pg_ctl" -D "$DATA" -m fast -w stop > /dev/null' EXIT
	"$WORK/kbench" pgx "host=/tmp port=$PORT user=postgres dbname=postgres" \
		"$WORK/$SCRIPT.sql" | tee "$OUT"
	echo "database_bytes	$(du -sb "$DATA/base" | cut -f1)" | tee -a "$OUT"
	;;
dolt)
	DIR=$WORK/dolt-$SCRIPT
	if [ -e "$DIR" ]; then
		echo "$DIR exists" >&2
		exit 1
	fi
	mkdir -p "$DIR/kb"
	(cd "$DIR/kb" && dolt init > /dev/null)
	(cd "$DIR" && exec dolt sql-server -P 3399 -H 127.0.0.1 > "$WORK/server-$SCRIPT.log" 2>&1) &
	SERVER=$!
	trap 'kill $SERVER; wait $SERVER 2> /dev/null || true' EXIT
	for _ in $(seq 1 50); do
		mysql -h 127.0.0.1 -P 3399 -u root -e 'select 1' > /dev/null 2>&1 && break
		sleep 0.2
	done
	"$WORK/kbench" mysql "root@tcp(127.0.0.1:3399)/kb" "$WORK/$SCRIPT.sql" | tee "$OUT"
	echo "database_bytes	$(du -sb "$DIR/kb/.dolt" | cut -f1)" | tee -a "$OUT"
	;;
*)
	echo "ENGINE must be heap, prolly or dolt" >&2
	exit 2
	;;
esac
