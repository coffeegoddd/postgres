# Standalone build of the prolly tree prototype.
#
#   make          build libprolly.a
#   make check    build and run the tests

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-parameter

SRCS = phash.c chunkfmt.c chunkstore.c memstore.c filestore.c dirstore.c prolly.c vc.c image.c kfplan.c kfspacing.c
OBJS = $(SRCS:.c=.o)
HDRS = prolly_util.h phash.h chunkfmt.h chunkstore.h prolly.h vc.h image.h kfplan.h kfspacing.h
TESTS = test/test_prolly test/test_vc test/test_lifetime test/test_window test/test_remote test/test_image test/test_dirstore test/test_kfplan test/test_kfspacing test/test_rewrite

all: libprolly.a

$(OBJS): $(HDRS)

libprolly.a: $(OBJS)
	$(AR) rcs $@ $^

# The history store inside Postgres, against the running test server.
test/test_pgstore: test/test_pgstore.c bench/pgstore.c bench/pgstore.h test/test_util.h test/test_repo.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -o $@ test/test_pgstore.c bench/pgstore.c libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq
pgstore-check: test/test_pgstore
	./test/test_pgstore
# The same against waldb (port 5495) and its hot standby (5492), both
# made by contrib/prolly_wal/spike: the store is opened on the standby too.
pgstore-standby-check: test/test_pgstore
	$(PGINST)/bin/pg_ctl -D /home/dustin/pgv2-bench/waldb -l /home/dustin/pgv2-bench/waldb/server.log -w start
	$(PGINST)/bin/pg_ctl -D /home/dustin/pgv2-bench/waldb_standby -l /home/dustin/pgv2-bench/waldb_standby/server.log -w start
	$(PGINST)/bin/psql -h /tmp -p 5495 -U postgres -d postgres -X -qAt \
		-c "SELECT 1 FROM pg_database WHERE datname = 'prolly_pgstore'" | grep -q 1 || \
		$(PGINST)/bin/psql -h /tmp -p 5495 -U postgres -d postgres -X -q -c "CREATE DATABASE prolly_pgstore"
	PROLLY_PG="host=/tmp port=5495 user=postgres dbname=prolly_pgstore" \
	PROLLY_PG_STANDBY="host=/tmp port=5492 user=postgres dbname=prolly_pgstore" ./test/test_pgstore
	$(PGINST)/bin/pg_ctl -D /home/dustin/pgv2-bench/waldb_standby -w stop -m fast
	$(PGINST)/bin/pg_ctl -D /home/dustin/pgv2-bench/waldb -w stop -m fast

test/%: test/%.c test/test_util.h test/test_repo.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -o $@ $< libprolly.a

check: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(OBJS) libprolly.a $(TESTS)

.PHONY: all check clean rebuild-check durability-check trigger-check gen-check branch-check pgstore-check pgstore-standby-check

# The window benchmark against a running server (see bench/winbench.c).
PGINST ?= /home/dustin/cursor_src/pg-v2-build/inst
bench/winbench: bench/winbench.c bench/pgstore.c bench/pgstore.h test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -o $@ $< bench/pgstore.c libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq -pthread

# Rebuilding a schema, every index included, from the store (see bench/rebuild.c).
REBUILD_SRCS = bench/rebuild.c bench/ws_pg.c bench/ws_codec.c bench/ws_logical.c bench/ws_stream.c \
	bench/ws_tags.c bench/ws_viol.c bench/ws_move.c bench/ws_diff.c bench/ws_merge.c bench/ws_history.c bench/pgoutput.c
bench/rebuild: $(REBUILD_SRCS) bench/pgstore.c bench/pgstore.h bench/ws.h bench/pgoutput.h test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -DPGBIN='"$(PGINST)/bin"' -o $@ $(REBUILD_SRCS) bench/pgstore.c libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq

# Against the running server: every commit of the fixture must rebuild, and
# each deliberate fault must be caught.
rebuild-check: bench/rebuild
	./bench/rebuild
	./bench/rebuild --stream
	./bench/rebuild --ddl
	./bench/rebuild --merge
	./bench/rebuild --move
	for b in row sequence matview nohide unlogged; do \
		if ./bench/rebuild --steps 1 --break $$b >/dev/null 2>&1; then \
			echo "rebuild: --break $$b went unnoticed"; exit 1; \
		else echo "rebuild: --break $$b caught"; fi; \
	done
	for b in cast isnull toast noreemit noflush; do \
		if ./bench/rebuild --stream --steps 3 --break $$b >/dev/null 2>&1; then \
			echo "rebuild: --stream --break $$b went unnoticed"; exit 1; \
		else echo "rebuild: --stream --break $$b caught"; fi; \
	done
	for b in notags:18,22,23,24,25 noviol:11,12,13,14,27,28; do \
		if ./bench/rebuild --merge --break $${b%%:*} --only $${b#*:} >/dev/null 2>&1; then \
			echo "rebuild: --merge --break $${b%%:*} went unnoticed"; exit 1; \
		else echo "rebuild: --merge --break $${b%%:*} caught"; fi; \
	done
	if ./bench/rebuild --move --break keepall >/dev/null 2>&1; then \
		echo "rebuild: --move --break keepall went unnoticed"; exit 1; \
	else echo "rebuild: --move --break keepall caught"; fi
	./bench/rebuild --history-rls
	if ./bench/rebuild --history-rls --break oldpolicies >/dev/null 2>&1; then \
		echo "rebuild: --history-rls --break oldpolicies went unnoticed"; exit 1; \
	else echo "rebuild: --history-rls --break oldpolicies caught"; fi
	./bench/stats_test.sh

# Version control runs no user code: the trigger fixture (triggers, rules
# and event triggers of every kind and enable state) through every mode,
# and prolly.quiet()/unquiet() checked directly.
trigger-check: bench/rebuild
	./bench/rebuild --fixture bench/trigger_fixture
	./bench/rebuild --fixture bench/trigger_fixture --stream
	./bench/rebuild --fixture bench/trigger_fixture --move
	./bench/rebuild --fixture bench/trigger_fixture --merge
	$(PGINST)/bin/psql -h /tmp -p 5499 -U postgres -d postgres -X -q \
		-c "DROP DATABASE IF EXISTS quiet_test" -c "CREATE DATABASE quiet_test"
	$(PGINST)/bin/psql -h /tmp -p 5499 -U postgres -d quiet_test -X -q -v ON_ERROR_STOP=1 \
		-o /dev/null -f bench/prolly_tags.sql
	$(PGINST)/bin/psql -h /tmp -p 5499 -U postgres -d quiet_test -X -q -v ON_ERROR_STOP=1 \
		-o /dev/null -f bench/quiet_test.sql

# Identity and generated columns: a commit's stored generated values are
# copied wherever rows are written back (rebuild, stream, every move) and
# computed only for rows a merge makes; the identities' sequences follow.
gen-check: bench/rebuild
	./bench/rebuild --fixture bench/gen_fixture
	./bench/rebuild --fixture bench/gen_fixture --stream
	./bench/rebuild --fixture bench/gen_fixture --move
	./bench/rebuild --fixture bench/gen_fixture --merge
	if ./bench/rebuild --fixture bench/gen_fixture --stream --break nogenpub >/dev/null 2>&1; then \
		echo "rebuild: --stream --break nogenpub went unnoticed"; exit 1; \
	else echo "rebuild: --stream --break nogenpub caught"; fi
	if ./bench/rebuild --fixture bench/gen_fixture --merge --break noregen >/dev/null 2>&1; then \
		echo "rebuild: --merge --break noregen went unnoticed"; exit 1; \
	else echo "rebuild: --merge --break noregen caught"; fi

# Branched catalogs: against a server built with
# contrib/prolly_branch/spike/catalog_branch.patch (CATINST), the catalog
# test, then the harness on branch 1 beside a decoy branch 2 schema of the
# same name, in every mode.
CATINST ?= /home/dustin/cursor_src/pg-v2-cat/inst/bin
CATCONN ?= host=/tmp port=5496 user=postgres
branch-check: bench/rebuild
	bash ../../contrib/prolly_branch/spike/catalog_branch_test.sh
	$(CATINST)/pg_ctl -D /home/dustin/pgv2-bench/catdb2 -l /home/dustin/pgv2-bench/catdb2/server.log -w start
	for m in "" --stream --move --merge; do \
		PGOPTIONS="-c catalog_branch=1" ./bench/rebuild --conninfo "$(CATCONN)" --bindir $(CATINST) \
			--fixture bench/branch_fixture $$m || exit 1; \
	done
	$(CATINST)/pg_ctl -D /home/dustin/pgv2-bench/catdb2 -w stop -m fast

# Crash safety and backups, in a cluster of their own (made on first use
# in CRASHDB, on port 5498): every crash point of a commit recovered, then
# a base backup recovered to a point in time.
CRASHDB ?= /home/dustin/pgv2-bench/crashdb
durability-check: bench/rebuild
	./bench/rebuild --crash-suite --conninfo "host=/tmp port=5498 user=postgres" --pgdata $(CRASHDB)
	./bench/rebuild --backup-suite --conninfo "host=/tmp port=5498 user=postgres" --pgdata $(CRASHDB)

bench/branchbench: bench/branchbench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -o $@ $< libprolly.a

bench/diskbench: bench/diskbench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -o $@ $< libprolly.a

bench/deepbench: bench/deepbench.c bench/pgstore.c bench/pgstore.h test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -o $@ $< bench/pgstore.c libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq

# Where restoring pages beats replaying rows, against a running server (see bench/kfbench.c).
bench/storebench: bench/storebench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -o $@ $< libprolly.a

bench/kfbench: bench/kfbench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -o $@ $< libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq

# Keyframes across the freeze horizon, in a throwaway cluster of its own (see bench/xidbench.c).
bench/xidbench: bench/xidbench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -DPGBIN='"$(PGINST)/bin"' -o $@ $< libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq

REMOTE_SRCS = bench/remote.c bench/packstore.c
bench/remotebench: bench/remotebench.c $(REMOTE_SRCS) bench/pgstore.c bench/remote.h bench/packstore.h bench/pgstore.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I. -I$(PGINST)/include -o $@ bench/remotebench.c $(REMOTE_SRCS) bench/pgstore.c libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq -lcurl -lcrypto -lzstd
bench/objserver/objserver: bench/objserver/main.go
	cd bench/objserver && GOPROXY=off go build -o objserver main.go
