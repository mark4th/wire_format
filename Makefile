# wire_format Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -O2

TESTS   = call_test quad_test
WFC_SRCS = compiler/main.c compiler/json5.c compiler/source.c \
	compiler/check.c compiler/wi.c compiler/util.c compiler/records.c
WFC_HEADERS = compiler/json5.h compiler/wfc.h

DNS_DIR = examples/dns
DNS_BUILD = target/examples/dns
DNS_HEADER = $(DNS_BUILD)/dns_protocol.h
DNS_DATABASE = $(DNS_BUILD)/dns_protocol.wi
DNS_OBJECTS = $(DNS_BUILD)/dns.o $(DNS_BUILD)/dns_demo.o \
	$(DNS_BUILD)/dns_protocol.o $(DNS_BUILD)/wire_format.o
DNS_TARGET = $(DNS_BUILD)/dns_demo
DNS_TEST = $(DNS_BUILD)/dns_test

all: wire-info dns_demo

dns_demo: $(DNS_TARGET)

$(DNS_TARGET): $(DNS_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

$(DNS_HEADER) $(DNS_DATABASE) &: wfc $(DNS_DIR)/dns.wf
	mkdir -p $(DNS_BUILD)
	./wfc $(DNS_DIR)/dns.wf --output $(DNS_BUILD)/dns_protocol

$(DNS_BUILD)/dns.o: $(DNS_DIR)/dns.c $(DNS_DIR)/dns.h $(DNS_HEADER)
	mkdir -p $(DNS_BUILD)
	$(CC) $(CFLAGS) -Werror -I. -I$(DNS_DIR) -I$(DNS_BUILD) -MMD -MP -c -o $@ $<

$(DNS_BUILD)/dns_demo.o: $(DNS_DIR)/dns_demo.c $(DNS_DIR)/dns.h
	mkdir -p $(DNS_BUILD)
	$(CC) $(CFLAGS) -Werror -I. -I$(DNS_DIR) -I$(DNS_BUILD) -MMD -MP -c -o $@ $<

$(DNS_BUILD)/dns_protocol.o: $(DNS_DIR)/dns_protocol.S $(DNS_DATABASE)
	mkdir -p $(DNS_BUILD)
	$(CC) $(CFLAGS) -Werror -I. -I$(DNS_DIR) -I$(DNS_BUILD) -MMD -MP -c -o $@ $<

$(DNS_BUILD)/wire_format.o: wire_format.c wire_format.h wire_record.h wire_format_record.inc
	mkdir -p $(DNS_BUILD)
	$(CC) $(CFLAGS) -Werror -I. -MMD -MP -c -o $@ $<

$(DNS_BUILD)/dns_test.o: $(DNS_DIR)/dns_test.c $(DNS_DIR)/dns.h
	mkdir -p $(DNS_BUILD)
	$(CC) $(CFLAGS) -Werror -I. -I$(DNS_DIR) -I$(DNS_BUILD) -MMD -MP -c -o $@ $<

$(DNS_TEST): $(DNS_BUILD)/dns_test.o $(DNS_BUILD)/dns.o \
	$(DNS_BUILD)/dns_protocol.o $(DNS_BUILD)/wire_format.o
	$(CC) $(CFLAGS) -o $@ $^

quad_test: quad_test.c wire_format.c wire_format.h wire_record.h wire_format_record.inc
	$(CC) $(CFLAGS) -o $@ quad_test.c wire_format.c

call_test: call_test.c wire_format.c wire_format.h wire_record.h wire_format_record.inc
	$(CC) $(CFLAGS) -o $@ call_test.c wire_format.c

wfc: $(WFC_SRCS) $(WFC_HEADERS)
	$(CC) $(CFLAGS) -Werror -Icompiler -o $@ $(WFC_SRCS)

test: $(TESTS) wfc $(DNS_TEST) test-info
	./call_test
	./quad_test
	./$(DNS_TEST)
	./tests/wfc_conformance.sh

# ⚠ -MMD -MP.  without these a change to wire_format.h rebuilds NOTHING
# and links a stale object against the new struct - which is exactly how
# adding the %[n] fields to wi_vars_t produced a stack smash in dns_demo
# that looked like a parser bug and was a build bug.

-include $(DNS_OBJECTS:.o=.d) $(DNS_BUILD)/dns_test.d

clean:
	rm -f dns.o dns.d dns_demo.o dns_demo.d dns_demo
	rm -f $(DNS_OBJECTS) $(DNS_OBJECTS:.o=.d) $(DNS_TARGET)
	rm -f $(DNS_TEST) $(DNS_BUILD)/dns_test.o $(DNS_BUILD)/dns_test.d
	rm -f $(DNS_HEADER) $(DNS_DATABASE) $(TESTS) wfc

.PHONY: all clean dns_demo test

# Reusable table loader and format-string interpreter.
INFO_BUILD ?= target/wire-info
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
BINDIR ?= $(PREFIX)/bin
AR ?= ar

wire-info: $(INFO_BUILD)/libwire_info.a $(INFO_BUILD)/libwire_info.so

$(INFO_BUILD)/wire_info.o: wire_info.c wire_info.h wire_format.h wire_record.h
	mkdir -p $(INFO_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -fPIC -MMD -MP -c $< -o $@

$(INFO_BUILD)/wire_format.o: wire_format.c wire_format.h wire_format_record.inc wire_record.h
	mkdir -p $(INFO_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -fPIC -MMD -MP -c $< -o $@

$(INFO_BUILD)/libwire_info.a: $(INFO_BUILD)/wire_info.o $(INFO_BUILD)/wire_format.o
	$(AR) rcs $@ $^

$(INFO_BUILD)/libwire_info.so.4: $(INFO_BUILD)/wire_info.o $(INFO_BUILD)/wire_format.o
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -Wl,-soname,libwire_info.so.4 -o $@ $^ $(LDLIBS)

$(INFO_BUILD)/libwire_info.so: $(INFO_BUILD)/libwire_info.so.4
	ln -sf libwire_info.so.4 $@

test-info: wire-info wfc
	python3 info/test_info.py --library $(INFO_BUILD)/libwire_info.so

install-info: wire-info wfc
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR) $(DESTDIR)$(BINDIR)
	install -m 644 $(INFO_BUILD)/libwire_info.a $(DESTDIR)$(LIBDIR)/
	install -m 644 wire_info.h wire_format.h wire_record.h $(DESTDIR)$(INCLUDEDIR)/
	install -m 755 $(INFO_BUILD)/libwire_info.so.4 $(DESTDIR)$(LIBDIR)/
	ln -sf libwire_info.so.4 $(DESTDIR)$(LIBDIR)/libwire_info.so
	install -m 755 wfc $(DESTDIR)$(BINDIR)/

clean-info:
	rm -rf $(INFO_BUILD)

-include $(INFO_BUILD)/wire_info.d $(INFO_BUILD)/wire_format.d
.PHONY: wire-info test-info install-info clean-info
