#!/bin/bash
# Reproduces vc_bench's dirty hard-reset cases by hand, with the counters
# around each reset: a prolly table of N rows (1 KB payloads), a history of
# H commits each updating the same K evenly spaced keys, a scratch branch at
# the head, then for each case a dirty working set (1,000-row UPDATE
# batches, each its own transaction) and dolt_reset('--hard', HEAD~D).
#
#   reset_hard_repro.sh init  DATA BIN PORT SOCKDIR [no-fold-worker]
#                                                 # initdb with vc_bench's settings
#   reset_hard_repro.sh load  BIN PORT SOCKDIR N [H] [K]  # table, baseline commit, history
#   [FOLD_AT=f] reset_hard_repro.sh case  BIN PORT SOCKDIR N D FRACTION [H]
#
# init's no-fold-worker sets prolly_fold_worker = off, so that what is
# folded before a reset is FOLD_AT's alone.  load resumes an interrupted
# load at whichever phase it stopped in (the rows, the baseline commit,
# the fixture branch, the history's commits).  FOLD_AT folds the working
# set (prolly_checkpoint()) after that fraction of the dirty batches, as a
# fold worker that fell behind leaves it: with the fold worker off,
# FOLD_AT=0.4 reproduces a reset that read the
# working set's distance from the commit from its baseline and the change
# finder's count, and so folded and wrote back the dirty rows.  The server
# must be started (pg_ctl) before load and case.  Example, 10 GB:
#
#   reset_hard_repro.sh init $DATA $BIN 15492 $SOCK
#   pg_ctl -D $DATA -l log -w start
#   reset_hard_repro.sh load $BIN 15492 $SOCK 9615384
#   reset_hard_repro.sh case $BIN 15492 $SOCK 9615384 500 0.5
set -euo pipefail
cmd=$1; shift
case $cmd in
init)
	DATA=$1 BIN=$2 PORT=$3 SOCK=$4 OPT=${5:-}
	"$BIN/initdb" -D "$DATA" -U postgres --auth-local=trust --auth-host=trust \
		--no-locale -E UTF8 > /dev/null
	cat >> "$DATA/postgresql.conf" <<CONF
listen_addresses = ''
port = $PORT
unix_socket_directories = '$SOCK'
shared_buffers = '256MB'
autovacuum = off
max_wal_size = '8GB'
checkpoint_timeout = '30min'
CONF
	if [ "$OPT" = no-fold-worker ]; then
		echo "prolly_fold_worker = off" >> "$DATA/postgresql.conf"
	elif [ -n "$OPT" ]; then
		echo "unknown init option: $OPT" >&2; exit 2
	fi
	;;
load)
	BIN=$1 PORT=$2 SOCK=$3 N=$4 H=${5:-500} K=${6:-1000}
	P() { "$BIN/psql" -X -q -v ON_ERROR_STOP=1 -h "$SOCK" -p "$PORT" -U postgres "$@"; }
	# (resumes an interrupted load after the rows already there)
	if ! P -d postgres -Atc "SELECT 1 FROM pg_database WHERE datname = 'bench_n$N'" | grep -q 1; then
		P -d postgres -c "CREATE DATABASE bench_n$N"
		P -d "bench_n$N" <<SQL
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE TABLE bench_rows (id BIGINT PRIMARY KEY, revision BIGINT NOT NULL,
	payload TEXT NOT NULL) USING prolly;
SQL
	fi
	Q() { P -d "bench_n$N" -At "$@" | tail -1; }
	# (the rows and the baseline commit, unless it was made)
	if [ "$(Q -c "SELECT count(*) FROM dolt_log WHERE message = 'vc_bench baseline'")" = 0 ]; then
		FROM=$(Q -c "SELECT coalesce(max(id), 0) + 1 FROM bench_rows")
		# (rows in batches of 10,000, each its own transaction; the payload
		# 1,024 base64 characters of sha256 digests of the id and block)
		P -d "bench_n$N" <<SQL
SELECT format(\$q\$INSERT INTO bench_rows
	SELECT i, 0, substr(translate(encode((SELECT string_agg(sha256(int8send(i) || int8send(b)), ''::bytea)
		FROM generate_series(0, 33) b), 'base64'), E'\n', ''), 1, 1024)
	FROM generate_series(%s, %s) i\$q\$, s, least(s + 9999, $N))
FROM generate_series($FROM, $N, 10000) s
\gexec
SELECT dolt_commit('-Am', 'vc_bench baseline', '--allow-empty');
SQL
	fi
	# (the fixture branch, unless it was made)
	if [ "$(Q -c "SELECT count(*) FROM dolt_branches WHERE name = 'fixture'")" = 0 ]; then
		P -d "bench_n$N" -c "SELECT dolt_branch('fixture', dolt_hashof('HEAD'))"
	fi
	# (the history's commits after those made; one whose UPDATE ran but
	# whose commit didn't is made again, its UPDATE writing the same rows)
	DONE=$(Q -c "SELECT dolt_checkout('fixture')" -c "SELECT count(*) FROM dolt_log WHERE message LIKE 'revision %'")
	P -d "bench_n$N" <<SQL
SELECT dolt_checkout('fixture');
SELECT format('UPDATE bench_rows SET revision = %s WHERE id IN (SELECT 1 + i::bigint * $N / $K FROM generate_series(0, $K - 1) i)', r),
	format('SELECT dolt_commit(''-Am'', ''revision %s'')', r)
FROM generate_series($DONE + 1, $H) r
\gexec
SQL
	;;
case)
	BIN=$1 PORT=$2 SOCK=$3 N=$4 D=$5 F=$6 H=${7:-500}
	P() { "$BIN/psql" -X -q -v ON_ERROR_STOP=1 -h "$SOCK" -p "$PORT" -U postgres -d "bench_n$N" "$@"; }
	P <<SQL
\set ON_ERROR_STOP 1
SELECT dolt_checkout('fixture');
SELECT dolt_hashof('HEAD') AS latest, dolt_hashof('HEAD~$D') AS old \gset
SELECT 'run_' || substr(md5(random()::text), 1, 8) AS scratch \gset
SELECT dolt_branch(:'scratch', :'latest');
SELECT dolt_branch(:'scratch' || '_t$D', :'old');
SELECT dolt_checkout(:'scratch');
SELECT ceil($F * $N)::bigint AS cnt \gset
\timing on
-- (FOLD_AT: the fraction of the batches after which the working set is
-- folded, as the fold worker would have folded it so far)
SELECT format('UPDATE bench_rows SET revision = $H + 1 WHERE id IN (SELECT 1 + i::bigint * $N / %s FROM generate_series(%s, %s) i)',
	:cnt, s, least(s + 999, :cnt - 1)) FROM generate_series(0, :cnt - 1, 1000) s
	WHERE s < ${FOLD_AT:-1} * :cnt \gexec
SELECT ${FOLD_AT:-1} < 1 AS fold \gset
\if :fold
SELECT prolly_checkpoint() IS NOT NULL AS folded;
\endif
SELECT format('UPDATE bench_rows SET revision = $H + 1 WHERE id IN (SELECT 1 + i::bigint * $N / %s FROM generate_series(%s, %s) i)',
	:cnt, s, least(s + 999, :cnt - 1)) FROM generate_series(0, :cnt - 1, 1000) s
	WHERE s >= ${FOLD_AT:-1} * :cnt \gexec
\timing off
\echo dirty rows :cnt
SELECT pg_stat_force_next_flush();
CREATE TEMP TABLE before AS SELECT name, value FROM pg_stat_prolly;
\timing on
SELECT dolt_reset('--hard', :'old');
\timing off
SELECT pg_stat_force_next_flush();
SELECT 1;
SELECT a.name, a.value - b.value AS delta FROM pg_stat_prolly a JOIN before b USING (name)
	WHERE a.value <> b.value ORDER BY 1;
SELECT dolt_hashof('HEAD') = :'old' AS head_ok,
	(SELECT revision FROM bench_rows WHERE id = 1) AS rev1;
SELECT dolt_checkout('fixture');
SELECT dolt_branch('-D', :'scratch');
SELECT dolt_branch('-D', :'scratch' || '_t$D');
SQL
	;;
esac
