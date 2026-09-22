# wire format info parser

The human-readable protocol source language is defined in
[doc/WF_FILE_FORMAT.md](doc/WF_FILE_FORMAT.md).

The source checker can be run directly through Cargo:

```sh
cargo run --bin wfc -- check examples/example-telemetry.wf
```

With no `check` command, compilation is implied.  The compiler emits a
position-independent binary database and its symbolic C header:

```sh
cargo run --bin wfc -- examples/example-telemetry.wf \
    --output build/example_telemetry
```

This creates `build/example_telemetry.wfb` and `build/example_telemetry.h`.
The binary may be read from storage, copied to memory, or embedded verbatim
with an assembler `.incbin`.  The application chooses how it stores and finds
the bytes.  `wfc` does not generate a loader or impose a linking policy.

The compiled binary layout is defined in
[doc/WFB_FILE_FORMAT.md](doc/WFB_FILE_FORMAT.md).

## What it is

wire_format is a compact, data-driven binary protocol encoder built around a
format string interpreter.  Each message type in a protocol is described by
a short format string literal baked into the executable.  A single generic
parser walks the string and emits the correct wire bytes.  No special
handler function is needed per message type.

The approach is directly inspired by terminfo, the UNIX terminal capability
database, which uses format strings and an RPN stack to describe how to
construct terminal escape sequences.  wire_format takes the same mechanism
and applies it to arbitrary binary protocols.

---

## Why it exists

The conventional approach to implementing a binary protocol is to write
a dedicated encode/decode function for every message type.  This works,
but has a real cost:

- Adding a new message type means writing new code.
- Every message type is a separate maintenance surface.
- Bugs in encoding logic tend to be duplicated across similar message
  types because each is hand-rolled independently.

wire_format inverts this.  The parser is written once.  Adding a new message
type means adding a format string — a string literal that describes the wire
layout.  No new code path, no new function, no new test surface for the
encoding machinery itself.

This matters most in embedded systems and protocol implementations where
message types accumulate over time.  A growing protocol does not require
a growing set of encoder functions; it requires a growing table of
format strings.

---

## How it works

### The RPN stack

wire_format uses a small integer stack (RPN, reverse Polish notation — the
same model as Forth and the original terminfo).  Format strings push values,
manipulate them, and then emit bytes.  This avoids the need for a
recursive-descent expression parser while still supporting arithmetic,
bitwise logic, and conditional branching.

### Parameters

The caller supplies an array of `int64_t` parameters before parsing.
Format string `%p1` pushes parameter 1 onto the stack, `%p2` pushes
parameter 2, and so on.  The compact spelling exists for parameters 1
through 9.  Braces address the complete parameter array:

```text
%p{1}   %p{9}   %p{10}   ...   %p{16}
```

Generated formats always use the braced spelling.  `%p10` does **not** mean
parameter 10: it retains its original interpretation as `%p1` followed by an
ordinary ASCII `0` byte (`0x30`).  A zero, empty, malformed, or out-of-range
braced index is rejected instead of indexing outside the parameter array.

### Emit specifiers

| Specifier | Effect |
|-----------|--------|
| `%c`      | emit low byte of TOS |
| `%b`      | emit 1 byte (alias for `%c`, explicit binary intent) |
| `%w`      | emit 2 bytes big-endian (uint16) |
| `%W`      | emit 4 bytes big-endian (uint32) |
| `%q`      | emit 8 bytes big-endian (uint64) |
| `%r`/`%r1` | emit raw bytes: TOS = count, next = pointer |
| `%r2`/`%r4` | emit a count of native uint16/uint32 elements as big-endian |
| `%[n]`    | call format *n* of the table set by `wi_set_formats()` |
| `%:`      | pop a repeat count for the next `%[n]` |

### Arithmetic and logic

| Specifier | Effect |
|-----------|--------|
| `%+` `%-` `%*` `%/` `%m` | binary arithmetic |
| `%&` `%\|` `%^` `%~`     | bitwise AND/OR/XOR/NOT |
| `%A` `%O` `%!`           | logical AND/OR/NOT |
| `%=` `%>` `%<`           | comparisons (push 0 or 1) |

### Message nesting — the format string call

`%[n]` embeds format string *n* of a caller-supplied table into the format
being parsed.  It is a subroutine call: the rest of the current string is
pushed, parsing continues inside the called one, and running off the end
of it returns to where the call was made.

```c
static const char f_coord[] = "%p1%w%p2%w";      // a reusable sub-message
static const char *table[]  = { f_coord };

wi_init(&v, buf, sizeof buf, params, 2);
wi_set_formats(&v, table, 1);                    // ⚠ AFTER wi_init
wi_parse(&v, "%{170}%c%[0]%{187}%c");            // AA <coord> BB
```

A message that contains another message is written once and referenced,
rather than copied into every format string that needs it.

⚠ `wi_set_formats()` must be called **after** `wi_init()` or
`wi_decode_init()` — both `memset` the whole struct, so setting the table
first silently loses it.

⚠ The called format shares the caller's parameters and its `a`–`z`
variables.  Sharing parameters is deliberate: a sub-format reads the same
array.  On **decode** it means a callee storing into `%Pa` overwrites the
caller's `a` — give parent and child disjoint letters, or read the
child's values out before calling again.

#### Arrays — `%rN`

`%rN` emits a count of elements from a buffer, big-endian like `%w` and
`%W`:

```c
"%p1%p2%r1"    // p1 = pointer, p2 = count.  bytes
"%p1%p2%r2"    // ...uint16 array
"%p1%p2%r4"    // ...uint32 array
```

Bare `%r` remains an alias for `%r1`, preserving existing byte-buffer
formats. When a digit follows `%r`, it is an element size and must be 1,
2, or 4; any other size sets `overrun`.

The multi-byte forms read native `uint16_t` or `uint32_t` elements and emit
their values in big-endian order exactly as `%w` or `%W` does for one value.
This produces the same wire representation on little- and big-endian hosts.

★ **This is also why `%:` cannot walk an array.** A repeat runs a format
again with the *same* parameters, so it emits one element N times.
Walking is a property of the emitter, not of the loop — which is why the
two are separate specifiers rather than one.

---

#### Repeating a call — `%:`

`%:` pops a repeat count off the RPN stack and applies it to the next
`%[n]`:

```c
"%{3}%:%[0]"          // call format 0 three times
"%p1%:%[0]"           // ...as many times as the caller passed
"%{2}%{3}%*%:%[0]"    // ...computed
```

Taking the count from the stack rather than baking a digit into the
string means it is not limited to a single digit, and it can be computed
from parameters or arithmetic.

⚠ **`%:` repeats something constant.** The parameters do not advance
between iterations, so it cannot walk an array of distinct values — use
`%rN` for a scalar array, and a C loop calling `wi_parse()` per element
for an array of records. That is the same rule decoding already follows.

**Zero works.**  `%{0}%:%[0]` does not call at all — the count is known
*before* the call rather than after, which a count placed at the end of a
loop body could never manage; that shape can only give do-while.

`%:` arms the *next* `%[n]`, not merely an adjacent one, so a count can
be computed, a header emitted, and then the call made.  Every `%[n]`
consumes it — **including one that is refused** — so a single `%:` arms
exactly one call and a stray one cannot leak into a later.

⚠ **Loop on encode; loop in C on decode.**  On encode the count is the
sender's own data.  On decode it would come off the wire, and a format
string cannot express "and stop if the input ran out" — the bound that
belongs in the caller:

```c
for (i = 0; i < ancount && (size_t)(p - buf) < len; i++)
    wi_parse(&v, wi_dns_rr);        // one fixed-size record
```

That is how the DNS decoder reads a variable number of answer records,
and it is the right shape: the parser only ever runs a fixed-length
format and the caller re-checks the buffer between records.

---

#### Nesting depth

`WI_CALL_DEPTH` (default 8) bounds the call stack, and is meant to be
set for the protocol using it — `-DWI_CALL_DEPTH=n`.  The right value is
how deep your own message table nests, which `wi_set_formats()` measures
and enforces; the default is a starting point, not a limit the library
can know for you.  Each frame is a return
address, the caller's index, the called format's start and a loop
counter — 28 bytes, so eight frames is 224.

★ **This is not the table size.**  An N-entry table *can* nest N deep,
since every call strictly decreases the index — but almost none do, and
refusing a forty-message protocol because it theoretically could would be
wrong.  `wi_set_formats()` therefore measures the table's actual depth
and refuses only a table that really would overflow the stack:

```
depth[k] = 1 + max(depth[j]) for every %[j] in format k
```

The ordering rule makes that a single pass upward from index 0 — the
table is a DAG that is already topologically sorted, so there is no
recursion and no cycle check to write.  A flat table of forty formats
measures depth 1 and is accepted.

---

#### No forward references

**A format may only call a format with a strictly lower index.**

That single rule is what makes recursion impossible.  Every call
decreases the index, the index cannot go below zero, so a call chain
always terminates — a cyclic table is not something that runs badly, it
is something that **cannot be written**.

It covers the case that is invisible in any single format string, a cycle
that exists only in the table:

```c
// neither of these looks wrong on its own
static const char f_ping[] = "%{2}%c%[5]";   // index 4 calls 5  ← refused
static const char f_pong[] = "%{3}%c%[4]";   // index 5 calls 4  ← fine
```

And it covers self-reference for free — `%[3]` inside format 3 is a cycle
of length one, which is why the comparison is *strictly* less rather than
"not greater".

The string passed to `wi_parse()` is not in the table and ranks above all
of it, so a top-level message may call anything.

`WI_CALL_DEPTH` (8) remains as a backstop on stack usage for large
tables; with the ordering rule it should never be the thing that stops a
call chain.

Every failure in `%[n]` is silent by design — a bad index, a forward
reference, a `NULL` slot, no table at all, or exceeding the depth —
because a wire encoder that aborts mid-message leaves a half-written
buffer, which is worse than a short one that the length field already
describes.

---

### Literals

| Specifier | Effect |
|-----------|--------|
| `%{123}`  | push decimal literal 123 |
| `%'x'`    | push ASCII value of character x |

### Variables

`%Pa` stores TOS into variable `a`; `%ga` retrieves it.
Lower-case `a`-`z` and upper-case `A`-`Z` name the same 26 variable slots, so `a` and `A` are aliases.

### Conditionals

```
%? <condition> %t <then-part> %e <else-part> %;
```

The `%e` else clause is optional.

### Faults

`%E` pops a 32-bit fault mask, ORs it into the parser's `faults` field, and
continues unless that mask intersects the caller-supplied `abort_mask`. This
keeps validation policy outside the format string: one caller can treat a fault
as diagnostic information while another can stop parsing immediately.

Fault names are application-level constants. A JSON or table-driven frontend can
spell a fault as `BAD_MAGIC` and compile it down to `%{1}%E`.

```c
#define FAULT_BAD_MAGIC  (1u << 0)

wi_decode_init(&v, frame, frame_len, NULL, 0);
v.abort_mask = FAULT_BAD_MAGIC;
wi_parse(&v, frame_header_format);
if (v.faults & FAULT_BAD_MAGIC) {
    /* reject frame */
}
```

`faults` resets at the start of each `wi_parse()` call. `abort_mask` is caller
policy and is left unchanged.

---

## The DNS proof of concept

DNS (RFC 1035) was chosen as the demonstration protocol because it is
well-known, fully specified, binary, and small enough to implement
completely in a short session.  It exercises the key features of
wire_format: fixed-width big-endian fields, literal constants, and raw
buffer emission for variable-length data.

### DNS query header

A DNS query header is 6 × uint16 fields in big-endian order:

```
ID | flags | QDCOUNT | ANCOUNT | NSCOUNT | ARCOUNT
```

The wire_format format string for this is:

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
./dns_demo [hostname]
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

## Buffer overruns

Reads and writes are bounds-checked and set `v.overrun`, which stops the
parse.

⚠ Both were `assert()` before `%:` existed.  With asserts enabled that
aborts the process; under `-DNDEBUG` it walks off the end of the buffer.
Neither is an option for a parser fed by a network, and a counted loop
makes both reachable from a single bad count.

The return value of `wi_parse()` is the length produced, which for a
truncated encode is a short but entirely plausible number — **`v.overrun`
is the only thing that says it is short because the buffer ran out.**

```c
wi_init(&v, buf, sizeof buf, params, n);
len = wi_parse(&v, fmt);
if (v.overrun) { /* buf was too small - do not transmit len bytes */ }
```

---

## Decoding incoming messages

wire_format can also walk an incoming buffer and extract field values.  The
same RPN stack, variables, and conditional logic are available; the
difference is that `%B`, `%S`, `%L`, and `%Q` *read* bytes from an input buffer
and push them onto the stack rather than popping bytes and writing them out.

### Decode specifiers

| Specifier | Effect |
|-----------|--------|
| `%B`      | read 1 byte from input → push |
| `%S`      | read 2 bytes big-endian → push as uint16 |
| `%L`      | read 4 bytes big-endian → push as uint32 |
| `%Q`      | read 8 bytes big-endian → push as uint64 |

Results are captured into named variables with `%Pa`, `%Pb`, ... and read
back from `wi_vars_t.vars[]` after parsing.

#### 64-bit values and the signed stack

`%q` and `%Q` move all eight bytes exactly, in both directions — a value
written with `%q` and read back with `%Q` is bit-identical.

⚠ The value stack is `int64_t`, so a `uint64_t` above `INT64_MAX` is
carried as a *negative* `int64_t`. That costs nothing when the value is
only being moved: cast `wi_vars_t.vars[]` back to `uint64_t` at the call
site and the bytes are right. It matters only if a format *compares* such
a value in place with `%>`, `%<` or `%=`, which compare signed.

ⓘ A 64-bit id with a small tag in its top byte never reaches that range,
which is the usual case for this specifier.

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

uint16_t txid    = (uint16_t)v.vars[0];  // a
uint16_t flags   = (uint16_t)v.vars[1];  // b
uint16_t ancount = (uint16_t)v.vars[3];  // d
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

uint16_t type     = (uint16_t)v.vars[0];
uint32_t ttl      = (uint32_t)v.vars[2];
uint16_t rdlength = (uint16_t)v.vars[3];
```

---

## Bit fields

Not all binary protocols are byte-oriented.  IP headers, DNS flags, and many
embedded protocols pack multiple fields into individual bytes using specific
bit positions.  wire_format supports this with three specifiers that operate
on a per-byte bit accumulator.

### Bit field specifiers

| Specifier | Effect |
|-----------|--------|
| `%x`      | encode: pop position, width, value → pack field into accumulator |
| `%X`      | decode: pop position, width → extract field from accumulator → push |
| `%f`      | encode: emit accumulator byte and reset; decode: discard current accumulator byte |

Position is measured from the LSB, so positioning is a left-shift
operation.  Width is the number of bits.  Both are popped from the stack,
making them fully computable via the RPN arithmetic operators.

### Encode example — IP header first byte

The first byte of an IPv4 header packs two 4-bit fields: version (bits
7:4) and IHL (bits 3:0).

```c
// params: p1 = version (4), p2 = IHL (5)  →  wire byte = 0x45
const char wi_ip_first_byte[] =
    "%p1%{4}%{4}%x"   // version: width=4, position=4
    "%p2%{4}%{0}%x"   // IHL:     width=4, position=0
    "%f";              // emit the composed byte
```

### Decode example — same byte

```c
const char wi_ip_first_byte_dec[] =
    "%{4}%{4}%X%Pa"   // extract version (width=4, pos=4) → a
    "%{4}%{0}%X%Pb"   // extract IHL     (width=4, pos=0) → b
    "%f";              // advance past the byte
```

After parsing, `v.vars[0]` holds the version and `v.vars[1]` holds IHL.

### Notes

- `%x` and `%X` both auto-load a byte into the accumulator on first use;
  `%f` flushes (encode) or advances (decode) the accumulator boundary.
- Fields wider than 8 bits that straddle byte boundaries must be split
  across two `%x`/`%X` operations with appropriate masking.
- The stack order is `value`, `width`, `position` — push the value first,
  then width, then position (TOS).

---

## Compiled protocol database

`wfc` turns a `.wf` source into one `.wfb` database.  The file contains a fixed
header, one fixed-size record per message, field-value metadata, an array of
string offsets, and one common string table.  Every location stored in the
file is an integer offset.  It contains no pointers, native C structures, or
host alignment.

The associated `.h` names message ordinals, message-record offsets,
string-section offsets, string slots, caller-field ordinals, and fixed wire
sizes.  A message string section contains offsets into the common string table,
following the same indirection used by compiled `terminfo` entries.  It holds
the message name, description, encode and decode programs, and field names.

How an application makes the `.wfb` bytes available is deliberately not part
of the format.  A hosted application can load the file.  Firmware can place it
in flash with `.incbin`, package it in another image, or copy it from external
storage.  All of those choices expose the same bytes to the interpreter.

---

## Extending to a new protocol

Adding a new message type requires:

1. Define the format string describing the wire layout.
2. Call `wi_init()` with the output buffer and parameters.
3. Call `wi_parse()` with the format string.

No new parser code.  No new encoding function.  The format string is
both the specification and the implementation of the message layout.

If the format string language lacks a specifier needed by the protocol,
add one operator to the dispatch table in `wire_format.c`.  All existing
format strings continue to work unchanged.

---

## Extending the format string language

The dispatch table in `wire_format.c` is a flat array of `{ character,
function }` pairs.  Adding a new specifier is two steps:

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

wire_format fits protocols where messages have a fixed or semi-fixed binary
layout: DNS, DHCP, custom embedded protocols, sensor data frames, game
network protocols, instrumentation buses.  It is less suited to
text-based protocols (HTTP/1.1) or highly dynamic layouts where the
structure itself depends on runtime negotiation.

The format string table can live in ROM on embedded targets.  The parser
has no dynamic allocation; `wi_vars_t` is caller-supplied and can be
stack-allocated.

---

## Rust port

A standalone Rust port is included as a separate crate so the format-string
protocol-builder idea can be evaluated independently.

```sh
cargo test
cargo run --example telemetry_demo
```

The Rust API keeps message definitions as strings and uses caller-supplied
buffers. It implements the same scalar, bit-field, conditional, fault,
format-call, repeat, typed-array, and 64-bit operations as the C parser.

The APIs differ where Rust can retain the type information which C receives as
a pointer:

```rust
let params = [
    Param::Raw(&bytes),       // use with %r or %r1
    Param::from(bytes.len()),
    Param::U16s(&shorts),     // use with %r2
    Param::from(shorts.len()),
    Param::U32s(&longs),      // use with %r4
    Param::from(longs.len()),
];
```

`WireFormat::set_formats(&formats)` supplies the table used by `%[n]` and
checks the actual nesting depth before parsing. The implementation uses a fixed
return stack and remains `no_std` and allocation-free. Buffer exhaustion is a
Rust `Error::OutputFull` or `Error::InputEof` rather than C's `overrun` flag.
Fault policy is set with `WireFormat::set_abort_mask()`, and raised fault bits
are available with `WireFormat::faults()` after a parse.

## Files

| File            | Purpose |
|-----------------|---------|
| `wire_format.h` | public API and types |
| `wire_format.c` | C format string parser |
| `dns.h`         | DNS constants and format string declarations |
| `dns.c`         | DNS query construction and response parsing |
| `dns_demo.c`    | C command-line DNS demo |
| `Cargo.toml` | Cargo workspace manifest |
| `rust/Cargo.toml` | Rust crate manifest |
| `rust/src/lib.rs` | Rust crate facade and public re-exports |
| `rust/src/error.rs` | Rust error type |
| `rust/src/param.rs` | Rust public parameters and internal stack values |
| `rust/src/format.rs` | Rust format-string scanner helpers |
| `rust/src/ops.rs` | Rust arithmetic, variable, and bit-field helpers |
| `rust/src/parser.rs` | Rust `WireFormat` parser engine |
| `rust/src/tests.rs` | Rust unit tests |
| `rust/src/bin/wfc/` | `.wf` lexer, parser, checker, `.wfb` compiler, and atomic output writer |
| `rust/tests/wfc_cli.rs` | `wfc` command-line and `.incbin` integration tests |
| `doc/WF_FILE_FORMAT.md` | normative `.wf` source-language definition |
| `doc/WFB_FILE_FORMAT.md` | normative compiled binary format |
| `examples/example-telemetry.wf` | checked and compiled `.wf` example |
| `rust/examples/telemetry_demo.rs` | Rust protocol-frame demo |

---

## License

MIT — do whatever you want with it.
