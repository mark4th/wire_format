# wire_format Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -O2

SRCS    = wire_format.c dns.c dns_demo.c
OBJS    = $(SRCS:.c=.o)
TARGET  = dns_demo
TESTS   = call_test quad_test
WFC_SRCS = compiler/main.c compiler/json5.c compiler/source.c \
	compiler/check.c compiler/wfb.c compiler/util.c
WFC_HEADERS = compiler/json5.h compiler/wfc.h

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

quad_test: quad_test.c wire_format.c
	$(CC) $(CFLAGS) -o $@ $^

call_test: call_test.c wire_format.c
	$(CC) $(CFLAGS) -o $@ $^

wfc: $(WFC_SRCS) $(WFC_HEADERS)
	$(CC) $(CFLAGS) -Werror -Icompiler -o $@ $(WFC_SRCS)

test: $(TESTS) wfc
	./call_test
	./quad_test
	./tests/wfc_conformance.sh

# ⚠ -MMD -MP.  without these a change to wire_format.h rebuilds NOTHING
# and links a stale object against the new struct - which is exactly how
# adding the %[n] fields to wi_vars_t produced a stack smash in dns_demo
# that looked like a parser bug and was a build bug.

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) $(TESTS) wfc

.PHONY: clean test
