# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Userspace test suite for the SLASH kernel module.
# Uses the kselftest harness for TAP output.

CC       ?= gcc
CFLAGS   ?= -Wall -Werror -O2
CFLAGS   += -I../libslash/include

TESTS    := test_slash_qdma

all: $(TESTS)

%: %.c kselftest.h kselftest_harness.h ../libslash/include/slash/uapi/slash_interface.h
	$(CC) $(CFLAGS) -o $@ $<

run: $(TESTS)
	@fail=0; \
	for t in $(TESTS); do \
		echo "# Running $$t"; \
		./$$t || fail=1; \
		echo; \
	done; \
	exit $$fail

clean:
	rm -f $(TESTS)

.PHONY: all run clean
