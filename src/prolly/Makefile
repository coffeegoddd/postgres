# Standalone build of the prolly tree prototype.
#
#   make          build libprolly.a
#   make check    build and run the tests

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-parameter

SRCS = phash.c chunkfmt.c chunkstore.c memstore.c filestore.c dirstore.c prolly.c vc.c image.c kfplan.c
OBJS = $(SRCS:.c=.o)
HDRS = prolly_util.h phash.h chunkfmt.h chunkstore.h prolly.h vc.h image.h kfplan.h
TESTS = test/test_prolly test/test_vc test/test_lifetime test/test_window test/test_remote test/test_image test/test_dirstore test/test_kfplan test/test_rewrite

all: libprolly.a

$(OBJS): $(HDRS)

libprolly.a: $(OBJS)
	$(AR) rcs $@ $^

test/%: test/%.c test/test_util.h test/test_repo.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -o $@ $< libprolly.a

check: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(OBJS) libprolly.a $(TESTS)

.PHONY: all check clean rebuild-check durability-check trigger-check

# The window benchmark against a running server (see bench/winbench.c).
PGINST ?= /home/dustin/cursor_src/pg-v2-build/inst
bench/winbench: bench/winbench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -o $@ $< libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq -pthread

# Rebuilding a schema, every index included, from the store (see bench/rebuild.c).
REBUILD_SRCS = bench/rebuild.c bench/ws_pg.c bench/ws_codec.c bench/ws_logical.c bench/ws_stream.c \
	bench/ws_tags.c bench/ws_viol.c bench/ws_move.c bench/ws_diff.c bench/ws_merge.c bench/pgoutput.c
bench/rebuild: $(REBUILD_SRCS) bench/ws.h bench/pgoutput.h test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -DPGBIN='"$(PGINST)/bin"' -o $@ $(REBUILD_SRCS) libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq

# Against the running server: every commit of the fixture must rebuild, and
# each deliberate fault must be caught.
rebuild-check: bench/rebuild
	./bench/rebuild
	./bench/rebuild --stream
	./bench/rebuild --ddl
	./bench/rebuild --merge
	./bench/rebuild --move
	for b in row sequence matview nohide; do \
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

bench/deepbench: bench/deepbench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -o $@ $< libprolly.a

# Where restoring pages beats replaying rows, against a running server (see bench/kfbench.c).
bench/kfbench: bench/kfbench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -o $@ $< libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq

# Keyframes across the freeze horizon, in a throwaway cluster of its own (see bench/xidbench.c).
bench/xidbench: bench/xidbench.c test/test_util.h libprolly.a $(HDRS)
	$(CC) $(CFLAGS) -I. -I$(PGINST)/include -DPGBIN='"$(PGINST)/bin"' -o $@ $< libprolly.a \
		-L$(PGINST)/lib -Wl,-rpath,$(PGINST)/lib -lpq
