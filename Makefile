# wire_format Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -O2

SRCS    = wire_format.c dns.c dns_demo.c
OBJS    = $(SRCS:.c=.o)
TARGET  = dns_demo

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
