# wire format info parser

## What it is

wire_format is a compact binary protocol encoder and decoder built around a
format-string interpreter. A format string describes how to read or write a
message: which values to use, how many bits or bytes they occupy, how fields
relate, and which other message formats to call.

The interpreter is shared by all messages. A protocol supplies a table of
format strings, and the application supplies values and buffers. Adding a
message using the available operations means adding a table entry; it does
not require a dedicated encode/decode function.

The approach is inspired by terminfo, the UNIX terminal capability database.
Terminfo uses format strings and an RPN stack to construct terminal escape
sequences. wire_format applies the same mechanism to binary protocols, in
both directions. Strings can be written by hand or generated from readable
field definitions, introduced after the interpreter and nesting model below.

## Why it exists

Handwritten protocol implementations commonly repeat the same operations:
shifting bit fields, choosing byte order, tracking lengths, handling optional
fields and checking buffer limits. Maintaining a separate codec for every
message duplicates those responsibilities as a protocol grows.

wire_format puts those operations in one interpreter and keeps message layouts
in data. Related messages can share subformats. A format table can live in
firmware or be loaded from storage, so an application can use additional
message definitions without rebuilding the interpreter.

The library handles wire representation. The application still decides when
to send a message and how to manage transport, sessions and protocol behavior.

---

## How it works

The application chooses a format string and supplies a buffer. For encoding,
it also supplies the field values to write; for decoding, the buffer contains
the received bytes. The interpreter reads the format string from left to
right, executing each `%` specifier as an operation. Operations can obtain a
caller value, perform a calculation, read or write bytes, or call another
format in the table.

These operations pass integer values to one another through a stack. This
lets a format describe both the layout of a message and calculations such as
deriving a length or combining several fields into one byte.

### The RPN stack

RPN (reverse Polish notation), also called postfix notation, puts an operator
after its operands. Instead of writing `(2 + 3) * 4`, write:

```text
2 3 + 4 *
```

The interpreter reads this from left to right. It pushes 2 and 3 onto the
stack. The `+` operation pops those values and pushes their sum, 5. It then
pushes 4, and `*` pops 5 and 4 and pushes 20. A push adds a value to the top
of the stack; a pop removes the most recently pushed value.

The instruction order already specifies the calculation order. The runtime
does not need to resolve operator precedence, match arithmetic parentheses,
or build an expression tree before evaluating it. Each operation executes as
it is read, using the stack to hold intermediate values. This keeps the
interpreter small and lets it use a fixed-size stack without allocating
memory for expressions.

wire_format uses this same mechanism for both arithmetic and byte operations.
To write a field, one operation pushes its value and a later operation pops
that value and emits the required bytes. Decode operations push values read
from the input so subsequent operations can store or check them.

For example, `%p1%w%p2%b` pushes parameter 1 and writes it as a big-endian
16-bit value, then pushes parameter 2 and writes one byte. Inputs `0x1234`
and `0x56` produce `12 34 56`. The corresponding decoder is `%S%Pa%B%Pb`:
read a 16-bit value into variable `a`, then a byte into `b`.

### Parameters

The application supplies numbered integer parameters before parsing.
Format string `%p1` pushes parameter 1 onto the stack, `%p2` pushes
parameter 2, and so on.  The compact spelling exists for parameters 1
through 9.  Braces address the complete parameter array:

```text
%p{1}   %p{9}   %p{10}   ...   %p{16}
```

The parameter-based compiler uses the braced spelling.  `%p10` does **not** mean
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
| `%v`      | emit the zero-copy byte slice in the popped slot |
| `%R`      | decode all remaining input into the popped zero-copy slice slot |
| `%d` / `%D` | emit / decode an unsigned 64-bit SDNV |
| `%N`      | pop slice slot, then byte count; capture that many input bytes |
| `%z`      | push the length of the popped slice slot |
| `%V`      | pop slice slot, then byte count; verify length and emit the slice |
| `%k`      | push the count of the popped record-list slot |
| `%J[n]`   | pop child field count, list slot, record count; process message *n* per row |
| `%[n]`    | call format *n* of the shared format table |
| `%:`      | pop a repeat count for the next `%[n]` |

### Arithmetic and logic

| Specifier | Effect |
|-----------|--------|
| `%+` `%-` `%*` `%/` `%m` | binary arithmetic |
| `%&` `%\|` `%^` `%~`     | bitwise AND/OR/XOR/NOT |
| `%A` `%O` `%!`           | logical AND/OR/NOT |
| `%=` `%>` `%<`           | comparisons (push 0 or 1) |

### Message nesting — shared format strings

A format table gives each reusable string a numeric index. `%[n]` means
"process the string at index n, then continue here." A message can therefore
reference a shared substring wherever it needs the same layout.

Consider a coordinate pair used in two messages:

| Index | Purpose | Format string |
|---|---|---|
| 0 | Shared coordinate pair | `%p1%w%p2%w` |
| 1 | Coordinate pair between markers AA and BB | `%{170}%b%[0]%{187}%b` |
| 2 | Coordinate pair between markers CC and DD | `%{204}%b%[0]%{221}%b` |

With parameters `0x1122` and `0x3344`, the strings produce:

```text
Format 0:       11 22 33 44
Format 1: AA    11 22 33 44    BB
Format 2: CC    11 22 33 44    DD
```

In format 1, `%{170}%b` writes AA. `%[0]` processes the coordinate string,
writing the two 16-bit values. Reaching the end of that shared string returns
to format 1, where `%{187}%b` writes BB. Format 2 references exactly the same
coordinate definition. Changing entry 0 changes the shared layout in both.

The same mechanism works for decoding. A shared decode string `%S%Pa%S%Pb`
reads two 16-bit values and stores them in variables `a` and `b`. A parent
format can read its header, call that string, and continue reading its trailer.

An ordinary `%[n]` call shares the current parameters, value stack and
variables. A value stored by the child remains available after it returns;
using the same variable again replaces that value. Give different fields
different variables when their decoded values must be retained together.

#### Repeating a shared string — `%:`

`%:` pops a repeat count and applies it to the next `%[n]` call:

| Format string | Effect |
|---|---|
| `%{3}%:%[0]` | Process entry 0 three times |
| `%p1%:%[0]` | Use parameter 1 as the repeat count |
| `%{2}%{3}%*%:%[0]` | Calculate the repeat count: 2 × 3 |
| `%{0}%:%[0]` | Skip the call |

With the coordinate parameters above, the first string produces
`11 22 33 44 11 22 33 44 11 22 33 44`. Each repetition uses the same
parameters. Repetition does not advance to another set of field values.

The count applies to the next call even if other operations occur between
`%:` and `%[n]`. That call consumes the count; subsequent calls run once
unless another `%:` supplies a new count. Reads and writes remain bounded
on every repetition.

#### Distinct child records — `%J[n]`

A list of different coordinate pairs needs a fresh set of field values for
each child. `%J[n]` applies shared string `n` once per record, with each
record's own values, byte slices and nested lists.

The numbered-slot form takes three stack operands: record count, list slot,
then child field count. For example:

```text
%{2}%{0}%{2}%J[0]
```

This processes two records from list slot 0, each containing two fields,
using format 0. If their coordinates are `(0x1122, 0x3344)` and
`(0x5566, 0x7788)`, the output is `11 22 33 44 55 66 77 88`.

The named form, `%J[n]{field-name}`, selects a list by field name. Its stack
operands are the record count followed by a terminator value; 256 represents
end of input rather than a delimiter byte. A finite count processes exactly
that many records. Each child has its own context, and named fields may also
refer to enclosing records. The later source-format section describes how
to declare these lists and generate the corresponding strings.

#### Reference order and nesting depth

A table entry may reference only entries with a **lower index**. Put shared
pieces first and the messages that compose them afterward. In the table above,
entries 1 and 2 can call entry 0. Entry 0 cannot call either parent, and no
entry can call itself. Every permitted call moves toward a lower index, so
self-recursion and cycles cannot execute.

Nesting depth measures the longest chain of calls, not the number of entries.
The coordinate table has depth two: a parent calls the coordinate substring.
Adding entry 3 with `%[1]%[2]` gives depth three: entry 3 calls a parent, which
calls the coordinate substring. Adding many independent parent formats would
leave the depth at two. The runtime checks this depth against its configured
limit before using the table.

### Arrays of scalar values — `%rN`

`%rN` reads successive elements from a supplied buffer and writes their values:

| Format string | Parameters | Output |
|---|---|---|
| `%p1%p2%r1` | Buffer address, byte count | Raw bytes |
| `%p1%p2%r2` | Buffer address, 16-bit element count | Two big-endian bytes per element |
| `%p1%p2%r4` | Buffer address, 32-bit element count | Four big-endian bytes per element |

Bare `%r` is equivalent to `%r1`. The element size must be 1, 2 or 4.
The multi-byte forms interpret the supplied elements in the host's native
order and emit them in big-endian order, like `%w` or `%W`.

Use `%rN` to walk scalar values, `%J[n]` to walk distinct child records, and
`%:` to repeat a shared string with unchanged parameters.

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

`%E` pops a fault-bit mask from the stack and records it. A format can use a
conditional to raise a fault when a field is invalid, for example
`%{1}%E` for the first fault bit. Application policy determines which faults
stop parsing immediately and which are retained for inspection afterward.

Fault-bit meanings belong to the application. The public API section below
explains how to select an abort policy and inspect the result.

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

Results are captured into variables with `%Pa`, `%Pb`, and so on, and made
available to the application after parsing.

#### 64-bit values and the signed stack

`%q` and `%Q` move all eight bytes exactly, in both directions — a value
written with `%q` and read back with `%Q` is bit-identical.

The original stack operations interpret values as signed 64-bit integers.
Moving all 64 bits through `%q` and `%Q` preserves them, but `%>` and `%<`
compare them as signed values. Use the unsigned `%u` operations when a
calculation or comparison must treat the high bit as part of a positive
unsigned value. The version 4 compiler uses those unsigned operations for
its expressions.

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

After parsing, variable `a` holds the version and variable `b` holds IHL.

### Notes

- `%x` packs into the output accumulator; `%X` loads an input byte on first
  use. `%f` emits/resets the output accumulator or discards the decoded byte.
- Fields wider than 8 bits that straddle byte boundaries must be split
  across two `%x`/`%X` operations with appropriate masking.
- The stack order is `value`, `width`, `position` — push the value first,
  then width, then position (TOS).

---

The C interpreter also has sequential `%i`/`%j` bit operations for named
records. They consume a width of 0–64 and advance through packed bits without
requiring per-byte positions. These are the operations generated for version 4
`uint` fields.

## Public C API

The functions declared in [`wire_format.h`](wire_format.h) and
[`wire_info.h`](wire_info.h) are application-facing interfaces. The first
executes format strings directly; the second loads compiled named-record
tables and uses the same interpreter. Applications do not call the parser's
internal operation handlers.

### Executing format strings directly

A caller-owned `wi_vars_t` holds the current buffer positions, parameters,
variables and parser status. Initialize it for the direction you need, attach
any shared formats or data, then execute a string:

| Function | Application responsibility and result |
|---|---|
| `wi_init(&state, output, capacity, params, count)` | Initialize encoding with an output buffer and optional `int64_t` parameters |
| `wi_decode_init(&state, input, length, params, count)` | Initialize decoding with the received bytes and optional parameters |
| `wi_set_formats(&state, formats, count)` | Attach the table referenced by `%[n]` and `%J[n]`; returns 0 on success or -1 for invalid table size/depth |
| `wi_set_slice(&state, slot, bytes, length)` | Attach a byte slice for `%v`, `%V`, `%z`, `%N` and `%R`; returns 0 or -1 |
| `wi_set_record_list(&state, slot, &list)` | Attach a bounded record list for numbered `%J[n]`; returns 0 or -1 |
| `wi_parse(&state, format)` | Execute a format string; returns the number of bytes emitted |

Call the attachment functions **after initialization**, because initialization
clears the context. `%p1` addresses the first parameter, while slice and list
slots are zero-based. The table and attached buffers must remain valid during
parsing. Parameters are copied into the context; table entries and byte slices
are borrowed.

The following complete example uses the shared coordinate format introduced
above, then decodes the resulting message:

```c
#include "wire_format.h"

int main(void)
{
    const char *encode_formats[] = {
        "%p1%w%p2%w",
        "%{170}%b%[0]%{187}%b"
    };
    const char *decode_formats[] = {
        "%S%Pa%S%Pb",
        "%B%Pc%[0]%B%Pd"
    };
    int64_t params[] = {0x1122, 0x3344};
    uint8_t bytes[6];
    wi_vars_t state;

    wi_init(&state, bytes, sizeof(bytes), params, 2);
    if (wi_set_formats(&state, encode_formats, 2) != 0)
    {
        return 1;
    }
    size_t used = wi_parse(&state, "%[1]");
    if (state.overrun || state.faults || used != sizeof(bytes))
    {
        return 1;
    }

    wi_decode_init(&state, bytes, used, NULL, 0);
    if (wi_set_formats(&state, decode_formats, 2) != 0)
    {
        return 1;
    }
    wi_parse(&state, "%[1]");
    return state.overrun || state.faults || state.in_pos != used ||
           state.vars[0] != 0x1122 || state.vars[1] != 0x3344 ||
           state.vars[2] != 0xAA || state.vars[3] != 0xBB;
}
```

Compile it against `wire_format.c`, or link the reusable library described
below. On decode, `state.in_pos` reports consumed bytes and `state.vars[0]`
through `[25]` correspond to variables `a` through `z`. The return value of
`wi_parse()` is the emitted length, so it is not the decode-consumption count.
Successive decode calls on the same context continue at `in_pos`; initialize
again when starting an independent input buffer.

For distinct child records, `wi_record_list_t` supplies a `records` array,
`capacity`, `field_count` and an encode `count`. Each `wi_record_t` row holds
its scalar `values`, byte `slices` and child `lists` at field ordinals. Decode
sets the resulting count and checks it against capacity. This is the storage
interface behind the numbered `%J[n]` form.

### Errors, fault policy and limits

Always check `state.overrun` after parsing. Buffer exhaustion, stack bounds
and malformed operations stop execution with that flag set; a short emitted
length alone does not distinguish success from partial output. Discard partial
output on error.

`%E` records fault bits in `state.faults`. Set `state.abort_mask` after
initialization to choose which faults stop parsing immediately; inspect
`state.faults` afterward to apply the application's validation policy.
Fault bits reset for each parse, while the abort mask is retained.

An ordinary `%[n]` call skips invalid indices, missing entries and forward/self
references. Return-stack exhaustion sets `overrun`. The outer string supplied
to `wi_parse()` is separate from the table and can call any table entry;
references within those entries must still point backward.

`wi_set_formats()` checks actual call depth, including the extra outer frame.
`WI_CALL_DEPTH` defaults to 8 and `WI_MAX_FORMATS` to 64. They are build-time
limits; if overriding them, compile the library and callers with matching
settings. The current C value stack holds 64 entries, the parameter array
holds 16, and there are 26 letter variables.

### Using compiled named-record tables

`wire_info.h` provides a higher-level public interface for compiled tables
with named fields. It currently accepts version 4 `.wi` images, introduced
in the source-format section next. It still executes their format strings
through `wi_parse()`.

| Function | Purpose |
|---|---|
| `wire_info_open()` | Validate a compiled image already supplied in memory and create a borrowed view |
| `wire_info_find()`, `wire_info_field()` | Find message and field indices by name; return -1 when absent |
| `wire_info_message_name()`, `wire_info_field_count()`, `wire_info_field_name()`, `wire_info_field_type()`, `wire_info_child()` | Inspect message/field metadata and child layouts |
| `wire_info_format()` | Obtain the selected encode or decode format string |
| `wire_info_encode()`, `wire_info_decode()` | Bind a caller-owned record and execute the chosen format |
| `wire_info_error()` | Convert a returned status to a readable description |

`wire_info_record_t` points to an array of `wire_info_value_t` fields and its
capacity. Values hold numbers, borrowed byte slices or bounded child-record
lists. Keep the image and record storage alive while using them; decoded byte
slices also require the input buffer to remain alive. Encode/decode return a
status and report bytes emitted/consumed through `used`. Errors set `used` to
zero, but records and output may already be partially modified.

---

## Human-readable definitions: `.wf` to `.wi`

A `.wf` file describes a protocol in human-readable form: message names,
fields in wire order, widths, constants, relationships and expected bytes.
The build-time compiler, `wfc`, turns that description into a machine-readable
binary `.wi` database containing **format strings and their lookup table**.
The runtime still interprets those strings using the operations described above.

```text
Build time:  protocol.wf ── wfc ──> protocol.wi + protocol.h
Runtime:     caller values + format string table ── wi_parse ──> wire bytes
             incoming bytes + decode string table ── wi_parse ──> field values
```

This lets authors maintain a readable field description instead of constructing
encode and decode strings by hand. Handwritten format strings and tables remain
supported through `wi_parse()` and `wi_set_formats()`; using the compiler is
optional. The compiler does not generate a separate C codec for each message.

### Source syntax and versions

The C compiler reads UTF-8 JSON5: ordinary JSON plus comments, trailing commas,
single-quoted strings, unquoted keys and hexadecimal integer literals. For
example, `0xFF` is the integer 255, while `"0xFF"` is a string. Integers used
as wire values must fit in an unsigned 64-bit value. Unknown and duplicate
schema properties are rejected rather than ignored.

The top-level object has exactly two properties:

| Property | Meaning |
|---|---|
| `wire_format` | Integer source-format version: 1, 2, 3 or 4 |
| `protocol` | The protocol object described below |

The version chooses a source vocabulary and caller-data model. It is not the
version number of the network protocol being described.

| Version | Source capabilities | Application interface | Compilers |
|---|---|---|---|
| 1 | Fixed scalar and packed-bit records | Parameters, variables and generated ordinals | C and Rust |
| 2 | Version 1 plus a final variable-length byte field | Also attached byte slices | C and Rust |
| 3 | Version 2 plus SDNVs, exact-length slices, counted records and choices | Also bounded record lists | C and Rust |
| 4 | Named records, expressions, conditions, checksums and additional encodings | `wire_info` named-record adapter | C |

Versions 1–3 are compatible extensions of the original source model. Version 4
uses explicit expressions and named fields; its declarations are documented
separately below rather than mixed into the earlier vocabulary. All use runtime
format strings. The DNS example uses version 1; the CCSDS definitions use
version 4. The Rust compiler and interpreter currently support versions 1–3.

### Protocol and message objects

| Protocol property | Versions 1–3 | Version 4 |
|---|---|---|
| `name` | Required identifier | Required identifier |
| `description` | Required nonempty text | Optional nonempty text |
| `standard`, `reference` | Optional nonempty text | Optional nonempty text |
| `byte_order` | Required: `big-endian` or `little-endian` | Set on individual `uint` fields instead |
| `bit_order` | Required: `msb-first` or `lsb-first` | `uint` fields use MSB-first bit packing |
| `messages` | Required nonempty message array | Required array of 1–64 messages |

| Message property | Meaning |
|---|---|
| `name` | Required, unique within the protocol |
| `description` | Required nonempty text in versions 1–3; optional in version 4 |
| `fields` | Required array in wire order; nonempty in versions 1–3 |
| `vectors` | Optional array of named examples with expected wire bytes |

Message order determines table indices. A child message must be declared
before any message that references it, preserving the interpreter's rule that
calls go to earlier table entries. Field names must be unique within a message.
No padding or native C structure layout is inferred.

Versions 1–3 identifiers start with a lower-case ASCII letter and contain only
lower-case letters, digits and single hyphens; they cannot end in a hyphen.
The generated header converts hyphens to underscores and names to upper case.
Version 4 identifiers may also contain upper-case letters, underscores and dots,
with a maximum length of 191 bytes. Reserve `parent.` for enclosing-record
references. Text and identifiers cannot contain NUL characters.

### A complete fixed-record example

Save this as `status.wf`:

```json5
{
  wire_format: 1,
  protocol: {
    name: 'example-status',
    description: 'A three-byte status message',
    byte_order: 'big-endian',
    bit_order: 'msb-first',
    messages: [
      {
        name: 'status',
        description: 'Version, mode and sample count',
        fields: [
          { name: 'version', type: 'bits', width: 3 },
          { name: 'reserved', type: 'bits', width: 1, constant: 0 },
          { name: 'mode', type: 'bits', width: 4 },
          { name: 'sample-count', type: 'u16' },
        ],
        vectors: [
          {
            name: 'nominal',
            values: { version: 1, mode: 2, 'sample-count': 0x1234 },
            wire: [0x22, 0x12, 0x34],
          },
        ],
      },
    ],
  },
}
```

The first three fields fill one byte. The next field occupies two big-endian
bytes. The caller supplies `version`, `mode` and `sample-count`; the compiler
supplies the reserved zero. The third caller value becomes `%p{3}%w` in the
encoder, while the decoder reads and stores that field and checks the constant.
Authors describe this layout once; `wfc` produces both programs.

### Version 1: fields, constants and layout

Every field has `name` and `type`, with the following additional properties:

| Type | Additional properties | Representation |
|---|---|---|
| `bits` | Required `width` from 1–64; optional `constant` | Packed bits in protocol `bit_order` |
| `u8` | Optional `constant` | One byte |
| `u16`, `u32`, `u64` | Optional `constant` | 2, 4 or 8 bytes in protocol `byte_order` |

`width` is forbidden on the fixed-width scalar types. Constant values must
fit the declared width. A constant has no caller parameter: encoding emits it,
and decoding raises the generated `*_WI_FAULT_FIXED_FIELD` mask (bit 0) if it differs. The application
chooses its fault-abort policy. Field names such as `reserved` or `version`
have no special behavior without an explicit `constant`.

Fields consume the record in array order. Adjacent `bits` fields can cross
byte boundaries; scalar fields must start at a byte boundary, and the complete
message must end at one. Alignment errors are reported, never padded.
`msb-first` fills each byte from bit 7 toward bit 0; `lsb-first` fills it from
bit 0 toward bit 7. `byte_order` applies to scalar types independently.

The parameter model supports at most 16 caller fields per message. A
caller-supplied `bits` value may be at most 63 bits because its generated
arithmetic uses the signed stack. Use an aligned `u64` for a full-width caller
value; constants can use all 64 bits.

### Version 2: a variable byte tail

Set `wire_format` to 2 to allow a final `bytes` field:

```json5
fields: [
  { name: 'kind', type: 'u8' },
  { name: 'payload', type: 'bytes' },
]
```

It must start at a byte boundary, be the last field, and have no `width` or
`constant`. Encode attaches its bytes with `wi_set_slice()`; decode borrows all
remaining input into that slice slot. The caller must bound the input to the
message being decoded. The generated wire size is the fixed-prefix/minimum
size, with a separate flag indicating variable size.

### Version 3: lengths, nested records and alternatives

Set `wire_format` to 3 to add the following declarations. Earlier scalar,
constant and layout rules still apply.

**SDNV integers.** `type: 'sdnv'` represents an unsigned base-128 integer using
one to ten bytes. The generated strings use `%d` and `%D`; unterminated or
64-bit-overflowing input fails.

**Exact byte lengths.** A preceding scalar or SDNV length field and a `bytes`
field reference each other using `length_of` and `length_from`:

```json5
fields: [
  { name: 'length', type: 'sdnv', length_of: 'payload' },
  { name: 'payload', type: 'bytes', length_from: 'length' },
  { name: 'trailer', type: 'u8' },
]
```

The encoder derives the length from the attached slice, so it is not another
caller parameter. The decoder borrows exactly that many bytes, leaving the
trailer available. Both references are required. `length_of` cannot be attached
to a `bits`, `bytes`, `records` or `choice` field. An unbounded `bytes` field
still consumes the remaining input and must be last.

**Counted records.** Declare the child message first, then give a preceding
scalar/bit/SDNV count field a reciprocal `count_of` reference:

```json5
fields: [
  { name: 'item-count', type: 'sdnv', count_of: 'items' },
  { name: 'items', type: 'records', message: 'item', count_from: 'item-count' },
]
```

Attach a `wi_record_list_t` with `wi_set_record_list()`. Its rows hold each
child's values, slices and child lists at the generated ordinals. The format
uses `%k` and `%J[n]` to process distinct records; decode rejects counts beyond
the supplied capacity. Derived length/count fields cannot also be constants.

**Alternatives.** A `choice` selects an earlier child message using a preceding
scalar selector:

```json5
{
  name: 'content',
  type: 'choice',
  select_from: 'kind',
  cases: [
    { value: 0, message: 'data' },
    { value: 1, message: 'report' },
  ],
}
```

Each case contains `value` and `message`; selector values must be unique. The
caller supplies a one-record list for the selected child. An unmatched value
selects no child; the application must enforce a closed selector set if its
protocol requires one. Choice policy is not inferred from message names.

### Version 4: named fields and expressions

Version 4 uses `wire_info_record_t` values in field order. It retains a slot
for every field, including constants and computed values. A numeric field uses
`number`; a byte field uses `bytes` and `length`; a record list uses `records`,
`count` and `capacity`. All storage belongs to the caller.

Every field has `name` and `type`. Optional common properties are:

| Property | Effect |
|---|---|
| `description` | Descriptive field text |
| `value` | Expression deriving the value on encode; also used by `computed` and `assert` on decode |
| `constant` | Supplies the encoded value and checks it on decode; mutually exclusive with `value` |
| `when` | Expression controlling whether the field is present |
| `min`, `max` | Inclusive limits on the numeric value |

An absent field consumes no bits. Decode clears its value, byte length and
record count while preserving allocated child storage. `min`/`max` constrain
numbers; use an assertion on `len` or `count` to constrain collections.

| Type | Additional properties and behavior |
|---|---|
| `uint` | Required `width` expression, 0–64 bits, packed MSB-first; optional `byte_order` is `big-endian` (default) or `little-endian` for widths 8, 16, …, 64 |
| `bytes` | Optional exact `length` expression, or `decode_length` applying only on decode; without either, encode the supplied slice and decode remaining input |
| `sdnv` | Unsigned base-128 integer, at most ten bytes |
| `records` | Required earlier child `message`; optional `count` expression and `until` delimiter |
| `parameter` | Caller-supplied out-of-band number; no wire bits; supply it on both encode and decode |
| `computed` | Required `value` expression; optional `decode_value` overrides it on decode; no wire bits |
| `assert` | Required `value` expression must be nonzero; otherwise fail; no wire bits |
| `mark` | Capture the absolute byte position; no wire bits |
| `crc` | Optional `algorithm`, `from`, `cbor`, `to_end`; details below |
| `cbor-uint` | Unsigned CBOR integer |
| `cbor-bytes`, `cbor-text` | Definite CBOR string header followed by byte-slice contents |
| `cbor-array` | Array header only; value is an element count or `0xFFFFFFFFFFFFFFFF` for indefinite length |
| `decimal`, `bcd` | Required `width` expression from 1–19 digits; ASCII decimal or packed BCD |
| `terminated-uint` | Up to eight big-endian bytes; optional encoded `length`, `termination_mask` and `termination_value` |

`length` and `decode_length` are mutually exclusive. Type-specific properties
apply only to their listed types; for example, `algorithm` belongs to `crc`,
and `decode_value` belongs to `computed`.

Byte slices, SDNVs, records, CBOR, CRCs and decimal encodings begin on byte
boundaries; complete records end on byte boundaries. Little-endian `uint`
fields need a whole-byte width but may begin within a packed byte. There is no
implicit padding. CBOR rejects non-minimal integer/length encodings; text bytes
are not UTF-8 validated. Indefinite byte/text strings are not supported.

For `terminated-uint`, the last byte satisfies
`(byte & termination_mask) == termination_value`; earlier bytes must not.
The default mask and stop value are both 1. The mask must be a nonzero byte,
and the stop value must fit it. Without an encoded length, use the minimum
number of bytes needed for the supplied value.

#### Expression vocabulary

An expression is an unsigned integer literal, a field-name string, or a prefix
array such as `["+", "header-size", ["len", "payload"]]`. Integer literals may
use decimal or hexadecimal notation. Field names resolve in the current
record, then enclosing records; `parent.` explicitly skips one scope. Repeat
the prefix to reach further ancestors.

| Expression | Meaning |
|---|---|
| `["+", x, y]`, `-`, `*`, `/`, `%` | Unsigned arithmetic and remainder |
| `["&", x, y]`, `\|`, `^`, `<<`, `>>` | Bit operations and shifts |
| `["==", x, y]`, `!=`, `<`, `<=`, `>`, `>=` | Comparisons returning 0 or 1 |
| `["and", x, y]`, `["or", x, y]`, `["not", x]` | Boolean operations |
| `["select", condition, yes, no]` | Select one of two values |
| `["len", "field"]`, `["count", "field"]` | Byte length or record count |
| `["byte", "field", index]` | Read a byte from a slice |
| `["remaining"]`, `["position"]`, `["start"]` | Remaining buffer bytes, absolute byte position, current record start |
| `["peek"]`, `["index"]` | Next input byte without consuming it; current child-record index |
| `["item", "list", "member", index]` | Numeric member of an indexed child |
| `["unique", "list", "member"]` | Test whether the child member values are unique |
| `["count-equal", "list", "member", value]` | Count children whose member equals a value |

Addition, subtraction and multiplication check overflow/underflow; division
by zero and shifts outside 0–63 fail. Shifts otherwise use unsigned 64-bit
semantics. All operands, including both branches of `select`, are evaluated
eagerly. Guard potentially invalid operations with field `when` conditions.

Decode expressions should use fields already read or caller-supplied parameters.
Encode expressions may use later inputs, for example deriving a payload length
before writing the payload. `remaining` is buffer capacity minus current
position, not the eventual encoded message length. `peek` is decode-only.

#### Nested records and optional bodies

`records` defaults to one child. An explicit `count` processes that many
children. Setting `until` to a byte value selects delimiter-terminated decoding;
`until: 256` selects end of input. With `until` and no count, the default count
is `0xFFFFFFFFFFFFFFFF`, meaning use the caller's list count on encode and
continue to the delimiter/end on decode. Caller capacity still bounds decoding.

The delimiter is left unread; describe it as a subsequent constant field if
it belongs to the message. A finite count ignores the delimiter. Every child
in an unbounded sequence must consume bytes. Bound the input to the containing
message when using an end-of-input list or byte slice.

Declare optional bodies as `records` fields with a `when` expression testing
a selector. Reusable components remain separate child definitions; profile
variations such as width or checksum presence can stay in one format.

Here is a complete named-record source, `batch.wf`:

```json5
{
  "wire_format": 4,
  "protocol": {
    "name": "example-batch",
    "messages": [
      {
        "name": "item",
        "fields": [
          {"name": "value", "type": "uint", "width": 16}
        ]
      },
      {
        "name": "batch",
        "fields": [
          {"name": "item-count", "type": "uint", "width": 8,
           "value": ["count", "items"]},
          {"name": "items", "type": "records", "message": "item",
           "count": "item-count"}
        ],
        "vectors": [
          {
            "name": "two-items",
            "values": {"items": [{"value": 0x1234}, {"value": 0xABCD}]},
            "wire": [0x02, 0x12, 0x34, 0xAB, 0xCD],
            "strict_prefixes": true
          }
        ]
      }
    ]
  }
}
```

The child occupies table entry 0. The batch encoder derives its count, writes
it, and calls `%J[0]{items}` with each child's record context. The decoder uses
the received count and checks the provided storage capacity. No per-message C
loop or codec is generated.

Version 4 limits are 64 messages, 128 fields per record, seven record levels,
32 source-expression levels, 64 runtime stack entries and 16 MiB of compiled
output. A child referring to enclosing fields requires that context and may
not be usable as a standalone message.

#### Checksums and indefinite arrays

A `crc` field uses `algorithm` 1 for CRC-16/CCITT-FALSE (default), 2 for
CRC-16/X25 or 3 for CRC-32C. Its `from` expression defaults to the current
record's starting byte; normally the range ends before the checksum itself.
Checksums are emitted big-endian.

`cbor: true` adds a CBOR byte-string header and includes the checksum bytes,
treated as zero during calculation. `to_end: true` defers calculation/checking
until the current record ends, which includes any trailing delimiter. At most
one deferred checksum is supported per record; children may have their own.
Both options are Boolean properties.

For a `cbor-array`, `0xFFFFFFFFFFFFFFFF` is an internal marker for an indefinite
array, encoded as the single byte `0x9F`. End that array with an explicit
`uint` field of width 8 and constant `0xFF`. An ordinary count produces a
counted-array header. The array header itself does not traverse the elements;
subsequent fields or child records describe them.

### Test vectors and byte notation

A vector supplies `name`, `values` and `wire`. `values` contains application
inputs; `wire` is the independently expected sequence of bytes. Use byte arrays
with two-digit hex entries (`0x00` through `0xFF`), up to 16 entries per line,
for wire fixtures and byte-valued inputs. Scalar markers such as
`0xFFFFFFFFFFFFFFFF` are individual integers, not byte arrays.

In versions 1–3, every caller field appears exactly once in `values`; constants
and derived fields are omitted. `wire` must be a nonempty byte array. `wfc check` and compilation both encode and decode the supported vectors and compare
all values and bytes. Version 3 messages containing `records` or `choice` are
covered by runtime tests rather than inline vectors; leaf-message vectors
remain supported.

In version 4, `values` can contain nested arrays of record objects and profile
parameters. Unspecified values begin at zero/empty. Byte inputs may also be
text strings, encoded as UTF-8 by the example tools. The tools accept hex strings
for `wire` as well, but byte arrays keep source representation consistent.
`strict_prefixes: true` additionally requires every truncated prefix to fail;
leave it false/absent for layouts where shorter input can be a complete record.

Version 4 compilation checks the schema and generates strings; it does **not**
execute its vectors. Run them through the shared runtime explicitly:

```sh
make wire-info wfc
./wfc batch.wf --output build/batch
python3 info/vectors.py --library target/wire-info/libwire_info.so \
    --images build batch.wf
```

The Python example tools accept JSON with hexadecimal integers; use quoted
keys/strings and omit JSON5 comments/trailing commas when those tools read the
source. `wfc` itself accepts the fuller JSON5 syntax. The `batch.wf` example
above works with both. Vectors are build-time source data and are not stored
in the `.wi`; an application such as the terminal demo may package them into
its own separate example catalog.

### Compiling and using the database

Build the C compiler and compile the fixed-record example:

```sh
make wfc
./wfc check status.wf
./wfc status.wf --output build/status
strings build/status.wi
```

The last command shows the names and actual runtime format strings inside the
binary. Compilation is deterministic and produces `build/status.wi` and
`build/status.h`. Output files are staged before replacement, so validation
failures leave existing outputs intact.

The independent Rust compiler uses the same command interface for versions 1–3:

```sh
cargo run -p wfc -- check status.wf
cargo run -p wfc -- status.wf --output build/status-rust
```

The conformance suite compares C/Rust output byte for byte for those versions.
JSON parsing belongs to the build-host compiler and tools, not the target
runtime. The C compiler's parser lives in `compiler/`; Rust's JSON5/Serde
dependencies belong to `rust/wfc/`.

A `.wi` stores a header, message metadata, field metadata, string-offset tables
and a string table. Message string slots include the name, description,
**encode string, decode string**, and field names. Stored locations are offsets;
there are no native pointers or host structure overlays. The binary container
makes the table easy to load or embed while retaining executable format strings
as text for the interpreter.

| Output/API | Versions 1–3 | Version 4 |
|---|---|---|
| `.h` | Symbolic message/field ordinals, offsets, sizes and variable-size flags | Currently a message-count macro; look up messages and fields by name |
| `.wi` loading | Application supplies a loader/view; DNS demonstrates an embedded view | `wire_info_open()` validates the named-record table |
| Runtime call | Attach parameters/slices/record lists and call `wi_parse()` | `wire_info_encode()` / `wire_info_decode()` bind records and call the same `wi_parse()` |

The application chooses storage: read a `.wi` from a file, embed it with
assembler `.incbin`, place it in flash, or package it into a larger catalog.
Adding a format using existing operations requires changing the table, not
recompiling the interpreter. A new runtime operation requires a library change.

For version 4, `make wire-info` builds reusable static and shared libraries in
`target/wire-info/`. Keep the loaded image alive while using its metadata and
strings. Allocate and zero caller value arrays and bounded child-record storage;
set parameters for both directions. Decoded byte slices borrow the input.
On success, `used` reports the bytes consumed/emitted; on error it is zero,
though output and records may be partially modified. Check `used` against the
input length when decoding one complete message. `wire_info_format()` exposes
the selected string for inspection. `wire_info_open()` currently accepts
version 4 images; earlier consumers keep their existing loading interfaces.

The byte-level layouts and version-specific specifications are maintained in
[the version 1 source reference](doc/WF_FILE_FORMAT.md),
[version 2](doc/WF_FILE_FORMAT_V2.md),
[version 3](doc/WF_FILE_FORMAT_V3.md),
[the named-record guide](doc/WIRE_INFO_V4.md), and
[the `.wi` binary reference](doc/WI_FILE_FORMAT.md).

A `.wf` describes wire representation. Application state machines, transport,
retries, scheduling and cryptographic operations remain application behavior.

---

## The DNS proof of concept

DNS (RFC 1035) was chosen as the demonstration protocol because it is
well-known, fully specified, binary, and small enough to follow. Its source is
[`examples/dns/dns.wf`](examples/dns/dns.wf). The build checks that
source, compiles it to `.wi` plus a symbolic header, and embeds the `.wi` with
`.incbin`. The C code looks up the generated encode and decode programs instead
of containing hand-written format strings.

### DNS query header

A DNS query header is 6 × uint16 fields in big-endian order:

```
ID | flags | QDCOUNT | ANCOUNT | NSCOUNT | ARCOUNT
```

The source description for the query header is:

```json5
fields: [
  { name: 'transaction-id', type: 'u16' },
  { name: 'flags', type: 'u16' },
  { name: 'question-count', type: 'u16' },
  { name: 'answer-count', type: 'u16', constant: 0 },
  { name: 'authority-count', type: 'u16', constant: 0 },
  { name: 'additional-count', type: 'u16', constant: 0 },
]
```

`wfc` generates the format string, caller-field ordinals, wire size, and testable
wire image from that description.

### DNS question section

The question section contains a length-prefixed label sequence (QNAME), a query
type, and a query class. Source-format version 1 describes fixed-size records,
so the bounded `dns_encode_name()` helper handles QNAME. The fixed tail is a
second compiled message:

```json5
fields: [
  { name: 'query-type', type: 'u16' },
  { name: 'query-class', type: 'u16' },
]
```

The response header and fixed resource-record fields are compiled from the same
file. Variable-length names and resource data remain ordinary bounded DNS code.

### DNS response header example

The DNS example describes the header in JSON5:

```json5
fields: [
  { name: 'transaction-id', type: 'u16' },
  { name: 'flags', type: 'u16' },
  { name: 'question-count', type: 'u16' },
  { name: 'answer-count', type: 'u16' },
  { name: 'authority-count', type: 'u16' },
  { name: 'additional-count', type: 'u16' },
]
```

The generated database supplies the decode program and the generated header
supplies its field ordinals:

```c
wi_decode_init(&v, response, rlen, NULL, 0);
wi_parse(&v, response_header_format);

uint16_t txid = (uint16_t)v.vars[DNS_RESPONSE_HEADER_ORDINAL_TRANSACTION_ID];
uint16_t flags = (uint16_t)v.vars[DNS_RESPONSE_HEADER_ORDINAL_FLAGS];
uint16_t ancount = (uint16_t)v.vars[DNS_RESPONSE_HEADER_ORDINAL_ANSWER_COUNT];
```

After this call `v.in_pos` is 12 (the DNS fixed header length), ready to
continue into the question or answer sections.  The DNS answer RR fixed
fields (type, class, TTL, rdlength) are decoded the same way after the
variable-length name is skipped. Its program is also generated from JSON5:

```c
v.in_pos = (size_t)(p - buf);  // sync past skipped name
wi_parse(&v, resource_record_format);
p = buf + v.in_pos;

uint16_t type = (uint16_t)v.vars[DNS_RESOURCE_RECORD_ORDINAL_RECORD_TYPE];
uint32_t ttl = (uint32_t)v.vars[DNS_RESOURCE_RECORD_ORDINAL_TTL];
uint16_t rdlength = (uint16_t)v.vars[DNS_RESOURCE_RECORD_ORDINAL_DATA_LENGTH];
```

---

### Running the demo

```
make
./target/examples/dns/dns_demo [hostname]
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

## Extending the format string language

The dispatch table in `wire_format.c` is a flat array of `{ character,
function }` pairs.  Adding a new specifier is two steps:

1. Write a static function that operates on the stack or calls `b_emit()`.
2. Add one entry to the `ops[]` table.

For example, a little-endian uint16 emitter:

```c
static void _wl(wi_vars_t *wi)
{
    uint16_t value = (uint16_t)fs_pop(wi);
    b_emit(wi, (uint8_t)(value & 0xFF));
    b_emit(wi, (uint8_t)(value >> 8));
}

// in ops[]:
{ 'l', _wl },
```

Then teach the source compiler how to emit that operation if `.wf` definitions
need it. An operation admitted by the named-record loader also needs syntax
validation there. Implement new operations in Rust separately when required;
its support does not follow automatically from a C dispatch-table change.

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
cargo run -p wire_format --example telemetry_demo
```

The Rust API keeps message definitions as strings and uses caller-supplied
buffers. It implements the same scalar, bit-field, conditional, fault,
format-call, repeat, typed-array, and 64-bit operations as the C parser.

This covers the versions 1–3 interface. The C version 4 named-record extensions
and `wire_info` adapter have not been ported to Rust.

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
return stack and remains `no_std`, allocation-free, and dependency-free. The
host-only Rust compiler under `rust/wfc/` owns the JSON5 and Serde dependencies;
they are not linked into the run-time crate. Buffer exhaustion is a
Rust `Error::OutputFull` or `Error::InputEof` rather than C's `overrun` flag.
Fault policy is set with `WireFormat::set_abort_mask()`, and raised fault bits
are available with `WireFormat::faults()` after a parse.

## Files

| File            | Purpose |
|-----------------|---------|
| `wire_format.h` | public API and types |
| `wire_format.c` | C format string parser |
| `wire_format_record.inc` | Generic named-record operations included by the parser |
| `wire_info.h`, `wire_info.c` | Named-record API and version 4 table loader/adapter |
| `wire_record.h` | Caller-owned named-record types and interpreter context |
| `info/` | Named-record vector runner and compiler/runtime checks |
| `compiler/` | C JSON5 parser, source checker, and `.wi` compiler |
| `Cargo.toml` | Cargo workspace manifest |
| `rust/Cargo.toml` | Rust crate manifest |
| `rust/src/lib.rs` | Rust crate facade and public re-exports |
| `rust/src/error.rs` | Rust error type |
| `rust/src/param.rs` | Rust public parameters and internal stack values |
| `rust/src/format.rs` | Rust format-string scanner helpers |
| `rust/src/ops.rs` | Rust arithmetic, variable, and bit-field helpers |
| `rust/src/parser.rs` | Rust `WireFormat` parser engine |
| `rust/src/tests.rs` | Rust unit tests |
| `rust/wfc/` | independent Rust JSON5 checker and `.wi` compiler |
| `rust/wfc/tests/wfc_cli.rs` | Rust `wfc` command-line and `.incbin` integration tests |
| `tests/wfc_conformance.sh` | C/Rust byte-for-byte compiler conformance test |
| `doc/WF_FILE_FORMAT.md` | normative `.wf` source-format definition |
| `doc/WF_FILE_FORMAT_V2.md` | compatible variable-tail extension |
| `doc/WF_FILE_FORMAT_V3.md` | SDNV, bounded-slice, record, and choice extension |
| `doc/WIRE_INFO_V4.md` | Named-record source, format operations and metadata |
| `doc/WI_FILE_FORMAT.md` | normative compiled binary format |
| `schema/wire-format-v1.schema.json` | JSON Schema for source-format version 1 |
| `schema/wire-format-v2.schema.json` | JSON Schema for source-format version 2 |
| `schema/wire-format-v3.schema.json` | JSON Schema for source-format version 3 |
| `examples/dns/` | C DNS demo, JSON5 protocol source, and embedded database wrapper |
| `examples/source-format/` | Small source-compiler example |
| `examples/telemetry/` | Standalone Rust telemetry-frame example |

---

## License

MIT — do whatever you want with it.
