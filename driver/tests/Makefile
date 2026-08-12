# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Userspace test suite for the SLASH kernel module.
# Uses the kselftest harness for TAP output.

CC       ?= gcc
CFLAGS   ?= -Wall -Werror -O2
CFLAGS   += -I../libslash/include

TESTS    := test_slash_ctldev test_slash_qdma test_slash_hotplug

HEADERS  := kselftest.h kselftest_harness.h slash_test_helpers.h \
            ../libslash/include/slash/uapi/slash_interface.h \
            ../libslash/include/slash/uapi/slash_hotplug.h

all: $(TESTS)

%: %.c $(HEADERS)
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
