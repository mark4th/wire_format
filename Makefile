# wire_format Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -O2

SRCS    = wire_format.c dns.c dns_demo.c
OBJS    = $(SRCS:.c=.o)
TARGET  = dns_demo
TESTS   = call_test

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

call_test: call_test.c wire_format.c
	$(CC) $(CFLAGS) -o $@ $^

test: call_test
	./call_test

# ⚠ -MMD -MP.  without these a change to wire_format.h rebuilds NOTHING
# and links a stale object against the new struct - which is exactly how
# adding the %[n] fields to wi_vars_t produced a stack smash in dns_demo
# that looked like a parser bug and was a build bug.

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) $(TESTS)

.PHONY: clean test
