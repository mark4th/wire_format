# winfo Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -O2

SRCS    = winfo.c dns.c demo.c
OBJS    = $(SRCS:.c=.o)
TARGET  = winfo_demo

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
