# winfo — wire format info parser

## What it is

winfo is a compact, data-driven binary protocol encoder built around a
format string interpreter.  Each message type in a protocol is described
by a short format string literal baked into the executable.  A single
generic parser walks the string and emits the correct wire bytes.  No
special handler function is needed per message type.

The approach is directly inspired by terminfo, the UNIX terminal
capability database, which uses format strings and an RPN stack to
describe how to construct terminal escape sequences.  winfo takes the
same mechanism and applies it to arbitrary binary protocols.

---

## Why it exists

The conventional approach to implementing a binary protocol is to write
a dedicated encode/decode function for every message type.  This works,
but has a real cost:

- Adding a new message type means writing new code.
- Every message type is a separate maintenance surface.
- Bugs in encoding logic tend to be duplicated across similar message
  types because each is hand-rolled independently.

winfo inverts this.  The parser is written once.  Adding a new message
type means adding a format string — a string literal that describes the
wire layout.  No new code path, no new function, no new test surface for
the encoding machinery itself.

This matters most in embedded systems and protocol implementations where
message types accumulate over time.  A growing protocol does not require
a growing set of encoder functions; it requires a growing table of
format strings.

---

## How it works

### The RPN stack

winfo uses a small integer stack (RPN, reverse Polish notation — the
same model as Forth and the original terminfo).  Format strings push
values, manipulate them, and then emit bytes.  This avoids the need for
a recursive-descent expression parser while still supporting arithmetic,
bitwise logic, and conditional branching.

### Parameters

The caller supplies an array of `int64_t` parameters before parsing.
Format string `%p1` pushes parameter 1 onto the stack, `%p2` pushes
parameter 2, and so on.

### Emit specifiers

| Specifier | Effect |
|-----------|--------|
| `%c`      | emit low byte of TOS |
| `%b`      | emit 1 byte (alias for `%c`, explicit binary intent) |
| `%w`      | emit 2 bytes big-endian (uint16) |
| `%W`      | emit 4 bytes big-endian (uint32) |
| `%r`      | emit raw buffer: TOS = length, next = pointer |

### Arithmetic and logic

| Specifier | Effect |
|-----------|--------|
| `%+` `%-` `%*` `%/` `%m` | binary arithmetic |
| `%&` `%\|` `%^` `%~`     | bitwise AND/OR/XOR/NOT |
| `%A` `%O` `%!`           | logical AND/OR/NOT |
| `%=` `%>` `%<`           | comparisons (push 0 or 1) |

### Literals

| Specifier | Effect |
|-----------|--------|
| `%{123}`  | push decimal literal 123 |
| `%'x'`    | push ASCII value of character x |

### Variables

`%Pa` stores TOS into variable `a`; `%ga` retrieves it.
Lower-case `a`–`z` and upper-case `A`–`Z` are available.

### Conditionals

```
%? <condition> %t <then-part> %e <else-part> %;
```

The `%e` else clause is optional.

---

## The DNS proof of concept

DNS (RFC 1035) was chosen as the demonstration protocol because it is
well-known, fully specified, binary, and small enough to implement
completely in a short session.  It exercises the key features of winfo:
fixed-width big-endian fields, literal constants, and raw buffer
emission for variable-length data.

### DNS query header

A DNS query header is 6 × uint16 fields in big-endian order:

```
ID | flags | QDCOUNT | ANCOUNT | NSCOUNT | ARCOUNT
```

The winfo format string for this is:

```c
const char wi_dns_header[] =
    "%p1%w"      // transaction ID
    "%p2%w"      // flags
    "%p3%w"      // QDCOUNT
    "%{0}%w"     // ANCOUNT = 0
    "%{0}%w"     // NSCOUNT = 0
    "%{0}%w";    // ARCOUNT = 0
```

Six fields, one line each.  The format string is the documentation of
the wire layout.

### DNS question section

The question section contains a length-prefixed label sequence (QNAME),
a query type, and a query class.  The label encoding is handled by a
small helper (`dns_encode_name`) which splits on dots and prepends
lengths.  The resulting byte buffer is emitted via `%r`:

```c
const char wi_dns_question[] =
    "%p1%p2%r"   // encoded QNAME (pointer + length via %r)
    "%p3%w"      // QTYPE
    "%p4%w";     // QCLASS
```

### Running the demo

```
make
./winfo_demo [hostname]
```

Default hostname is `example.com`.  Queries Google's public resolver
(8.8.8.8) for A records and prints the results.

```
querying example.com for A records (txid=0xa0ac, 29 bytes)
txid=0xa0ac  flags=0x8180  questions=1  answers=2
  A  ttl=179     172.66.147.243
  A  ttl=179     104.20.23.154
```

---

## Decoding incoming messages

winfo can also walk an incoming buffer and extract field values.  The same
RPN stack, variables, and conditional logic are available; the difference
is that `%B`, `%S`, and `%L` *read* bytes from an input buffer and push
them onto the stack rather than popping bytes and writing them out.

### Decode specifiers

| Specifier | Effect |
|-----------|--------|
| `%B`      | read 1 byte from input → push |
| `%S`      | read 2 bytes big-endian → push as uint16 |
| `%L`      | read 4 bytes big-endian → push as uint32 |

Results are captured into named variables with `%Pa`, `%Pb`, … and read
back from `wi_vars_t.atoz[]` after parsing.

### Initialisation

```c
wi_decode_init(&v, buf, buflen, NULL, 0);
wi_parse(&v, format_string);
```

`v.in_pos` advances as bytes are consumed.  Multiple `wi_parse()` calls on
the same `wi_vars_t` continue from where the previous call left off, so
header and body sections can be decoded in sequence without
re-initialising.

### DNS response header example

```c
const char wi_dns_resp_hdr[] =
    "%S%Pa"     // txid    -> a
    "%S%Pb"     // flags   -> b
    "%S%Pc"     // qdcount -> c
    "%S%Pd"     // ancount -> d
    "%S%Pe"     // nscount -> e
    "%S%Pf";    // arcount -> f

wi_decode_init(&v, response, rlen, NULL, 0);
wi_parse(&v, wi_dns_resp_hdr);

uint16_t txid    = (uint16_t)v.atoz[0];  // a
uint16_t flags   = (uint16_t)v.atoz[1];  // b
uint16_t ancount = (uint16_t)v.atoz[3];  // d
```

After this call `v.in_pos` is 12 (the DNS fixed header length), ready to
continue into the question or answer sections.  The DNS answer RR fixed
fields (type, class, TTL, rdlength) are decoded the same way after the
variable-length name is skipped:

```c
const char wi_dns_rr[] =
    "%S%Pa"     // type     -> a
    "%S%Pb"     // class    -> b
    "%L%Pc"     // ttl      -> c  (uint32)
    "%S%Pd";    // rdlength -> d

v.in_pos = (size_t)(p - buf);  // sync past skipped name
wi_parse(&v, wi_dns_rr);
p = buf + v.in_pos;

uint16_t type     = (uint16_t)v.atoz[0];
uint32_t ttl      = (uint32_t)v.atoz[2];
uint16_t rdlength = (uint16_t)v.atoz[3];
```

---

## Extending to a new protocol

Adding a new message type requires:

1. Define the format string describing the wire layout.
2. Call `wi_init()` with the output buffer and parameters.
3. Call `wi_parse()` with the format string.

No new parser code.  No new encoding function.  The format string is
both the specification and the implementation of the message layout.

If the format string language lacks a specifier needed by the protocol,
add one operator to the dispatch table in `winfo.c`.  All existing
format strings continue to work unchanged.

---

## Extending the format string language

The dispatch table in `winfo.c` is a flat array of `{ character, function }`
pairs.  Adding a new specifier is two steps:

1. Write a static function that operates on the stack or calls `b_emit()`.
2. Add one entry to the `ops[]` table.

For example, a little-endian uint16 emitter:

```c
static void _wl(void)
{
    uint16_t v = (uint16_t)fs_pop();
    b_emit(v & 0xff);
    b_emit(v >> 8);
}

// in ops[]:
{ 'v', _wl },
```

All existing format strings are unaffected.

---

## Applicability

winfo fits protocols where messages have a fixed or semi-fixed binary
layout: DNS, DHCP, custom embedded protocols, sensor data frames, game
network protocols, instrumentation buses.  It is less suited to
text-based protocols (HTTP/1.1) or highly dynamic layouts where the
structure itself depends on runtime negotiation.

The format string table can live in ROM on embedded targets.  The parser
has no dynamic allocation; `wi_vars_t` is caller-supplied and can be
stack-allocated.

---

## Files

| File       | Purpose |
|------------|---------|
| `winfo.h`  | public API and types |
| `winfo.c`  | format string parser |
| `dns.h`    | DNS constants and format string declarations |
| `dns.c`    | DNS query construction and response parsing |
| `demo.c`   | command-line demo |

---

## License

MIT — do whatever you want with it.
