# Standalone build of the prolly tree prototype.
#
#   make          build libprolly.a
#   make check    build and run the tests

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-parameter

SRCS = phash.c chunkfmt.c chunkstore.c memstore.c prolly.c vc.c image.c
OBJS = $(SRCS:.c=.o)
HDRS = prolly_util.h phash.h chunkfmt.h chunkstore.h prolly.h vc.h image.h
TESTS = test/test_prolly test/test_vc test/test_lifetime test/test_window test/test_remote test/test_image

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

.PHONY: all check clean
