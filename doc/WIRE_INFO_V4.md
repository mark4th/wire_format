# Named records in wire_info format strings

The C `wfc` compiler reads `.wf` sources (JSON5) and writes `.wi` files
containing **encode/decode format strings and their lookup table**. `wire_info`
loads that table, binds caller records, and invokes `wi_parse()` in
`wire_format.c`. All field operations, expressions, conditions and child calls
execute through that interpreter's RPN stack and dispatch table.

There is no separate binary schema VM or runtime JSON parser. Protocol
metadata contains names, field types and child indices for introspection;
it does not encode serialization instructions. A new message using the
existing primitives requires a new table entry, not new C codec code.

## Build

```sh
make wire-info wfc
./wfc protocol.wf --output build/protocol
make test-info
make install-info PREFIX="$HOME/.local"
strings build/protocol.wi
```

Libraries are `target/wire-info/libwire_info.a` and `libwire_info.so.4` (with a
`.so` symlink). Both include the loader and the existing format interpreter.
`INFO_BUILD` selects an independent output directory. All applications reuse
these libraries. `install-info` installs the C compiler, libraries and public
headers. Python is used by the tests and example catalog builder, not `wfc`.

Versions 1–3 keep their existing source, binary layout and format operations.
Version 4 adds named record operands and metadata within the same `WI\0\0`
file family. The record adapter `wire_info_open` currently accepts version 4;
earlier consumers keep their existing `.wi` loading APIs. The Rust compiler
and parser currently support versions 1–3, not the named-record extensions.

## Source

```json
{
  "wire_format": 4,
  "protocol": {
    "name": "example",
    "messages": [{
      "name": "message",
      "fields": [
        {"name": "version", "type": "uint", "width": 3, "constant": 1},
        {"name": "kind", "type": "uint", "width": 5},
        {"name": "size", "type": "uint", "width": 16, "value": ["len", "payload"]},
        {"name": "payload", "type": "bytes", "length": "size"},
        {"name": "check", "type": "crc", "algorithm": 1}
      ]
    }]
  }
}
```

Duplicate properties, unknown properties/types/operators, unknown field
references, recursive records and invalid property/type combinations are
rejected. Message names are unique per file; field names are unique per message.
Children must reference **earlier** messages. Limits are 128 fields per record,
7 record levels, 32 source-expression levels and 64 RPN stack entries.
Compiled output is limited to 16 MiB. There are no protocol codec callbacks. A child may reference enclosing fields; such a child requires
that context and cannot necessarily be decoded standalone.

Messages may include `description` and `vectors`; protocols may additionally
include `standard` and `reference`. Vectors are build-time tests, not runtime
instructions: `name`, `values`, `wire` (hex or byte array), and optional
`strict_prefixes`. Run them with `info/vectors.py --library LIB --images DIR
sources.wf ...`; compilation itself does not execute vectors.

### Field types

All fields have `name`, `type`, and optionally `description`, `when`, `min`,
`max`, `value` or `constant`. `value` derives an encoded numeric field;
`constant` also checks it on decode. Use an `assert` for other relationships.
`min`/`max` constrain numeric values, not byte/list lengths; check those with
an assertion using `len`/`count`. Conditional fields consume nothing when
`when` evaluates to zero. Decode clears absent values but preserves allocated
child-record storage.

| Type | Representation / additional properties |
|---|---|
| `uint` | MSB-first bits, `width` 0–64; optional `byte_order: "little-endian"` for whole octet widths |
| `bytes` | Borrowed byte slice; `length` expression is exact on both paths; `decode_length` derives only the decoded length; absent length consumes remaining input on decode |
| `sdnv` | Unsigned base-128 value, at most ten octets, checked for overflow |
| `records` | Child `message`, `count` expression (default one); optional `until` marker octet, or 256 for end of input |
| `parameter` | Caller-supplied out-of-band numeric value, consumes no bits |
| `computed` | Virtual numeric `value`, consumes no bits; optional `decode_value` instead when decoding |
| `assert` | Requires its `value` expression to be nonzero; consumes no bits |
| `mark` | Captures absolute byte position in the stream |
| `crc` | Algorithms 1=CCITT-FALSE, 2=X25, 3=CRC32C; optional `from`, `cbor`, `to_end` |
| `cbor-uint` | Definite unsigned CBOR integer |
| `cbor-bytes`, `cbor-text` | Definite CBOR string with length header and borrowed bytes |
| `cbor-array` | CBOR array header; numeric value is count; UINT64_MAX means indefinite |
| `decimal`, `bcd` | Fixed `width` of 1–19 decimal digits in ASCII or packed BCD |
| `terminated-uint` | Up to eight big-endian octets; stop when `(octet & termination_mask) == termination_value`; defaults 1/1; optional encoded octet `length` |

Bytes, SDNV, CBOR and records must start on octet boundaries. A complete
message must end on an octet boundary. Numeric CRCs are serialized big-endian.
Little-endian fields must be octet-sized, but their start can be bit-packed.
CBOR decoding rejects non-minimal integer/length encodings. Text bytes are not
UTF-8 validated by this runtime. Indefinite text/byte strings are not supported;
indefinite arrays are described using records and explicit break fields.

For `records`, an unbounded count is represented by UINT64_MAX. With `until`,
this is the default; the encoder uses the supplied list count and the decoder
reads until the delimiter/EOF, bounded by caller capacity. The marker is not
consumed: describe it as a subsequent constant field. A finite count ignores
the marker and reads that many records. End-of-input records require a buffer
bounded to the containing message; do not pass following messages as part of
that input. Every record in an unbounded sequence must consume bytes.

CRC `from` defaults to the current record's starting byte. Normally the range
ends just before the checksum. With `cbor: true`, a CBOR byte-string header
precedes the checksum and the range includes its zeroed checksum bytes.
`to_end: true` defers calculation/checking until the current record ends,
zeroing only the checksum bytes, so a trailing delimiter is covered too.
At most one deferred CRC is supported per record; nested records may each
have one. CRCs detect errors; they are not cryptographic authentication.

### Expressions

A number is an unsigned 64-bit literal. A string refers to a field's numeric
value. Local fields shadow enclosing fields; `parent.` explicitly skips a
scope. Reserve that prefix for references, not field names.

Expressions use prefix arrays:

- `["+", x, y]`, `-`, `*`, `/`, `%`, `&`, `|`, `^`, `<<`, `>>`.
- Comparisons `==`, `!=`, `<`, `<=`, `>`, `>=`; Boolean `and`, `or`, unary `not`.
- `["select", condition, yes, no]`. **All operands are evaluated eagerly**;
  use conditional fields to guard otherwise-invalid arithmetic/lookups.
- `["len", "field"]`, `["count", "field"]`, `["byte", "field", index]`.
- `["remaining"]`, `["position"]`, `["start"]` count bytes; remaining is the
  caller's input/output capacity minus current byte position. In encoding,
  prefer `len`/`count`; output capacity is not the encoded message length.
- `["peek"]` returns the next input byte without consuming it; decode only.
- `["index"]` returns the current record's index within its parent list.
- `["item", "list", "numeric-member", index]` reads a child numeric field.
- `["unique", "list", "numeric-member"]` tests uniqueness (quadratic in count).
- `["count-equal", "list", "numeric-member", value]` counts matches.

Addition, subtraction and multiplication reject overflow/underflow; division
by zero and shifts of 64 or more fail. Bit shifts otherwise follow unsigned
64-bit semantics. Decode expressions should reference fields already read or
parameters. Encoding expressions may reference later application inputs,
e.g. to derive a length before emitting its payload. Lookup/array errors fail
the operation; there is no unchecked pointer arithmetic supplied by schemas.

## Runtime API and ownership

`wire_info_open()` validates the image's layout, table bounds, strings,
format-string syntax/condition nesting, field types and child references.
Stack bounds and operand values are checked while interpreting the strings.
Retain the immutable image for the database's lifetime. Validation does not
prove a schema is semantically correct; encoding/decoding can still report a
schema error (for example a missing enclosing context or misalignment).
Images are data interpreted by the bounded engine, never executable code.

`wire_info_find`, `wire_info_field`, and metadata functions expose the schema.
A `wire_info_record_t` points to caller-owned `wire_info_value_t` fields in
schema order. Set capacity to the number allocated. For each child list,
provide a records pointer and capacity, and initialize every child's values
array/capacity. On encode set its count as well. Use zero initialization.
Byte fields supply bytes/length; numeric fields supply number. Set profile
parameters for both directions. No protocol struct is necessary.

Decode returns byte slices into input; neither input nor schema may be freed
while its views are in use. Encoding fills computed numeric values in records.
Do not overlap input byte slices with output buffers. Calls are independent
and thread-safe when caller records/output buffers are not shared mutably.

Every call returns a status. On success `used` is bytes emitted/consumed. On
error it is zero, but records/output may be partial and must be discarded.
Trailing input is allowed unless the schema consumes it; applications decoding
one complete frame should check `used == input_length`. Caller capacities bound
all list traversal and output. Choose practical list limits for untrusted
traffic, particularly with quadratic uniqueness assertions.

## Format-string operations

The existing `%{number}` literal, `%? … %t … %e … %;` conditional and
`%d`/`%D` SDNV operations remain in use. Named operands extend the existing
field/slice/list operations:

| Format | Stack / effect |
|---|---|
| `%g{name}`, `%P{name}` | Push/store the named numeric value; enclosing scopes and `parent.` are supported |
| `%z{name}`, `%k{name}` | Push the byte length / record count |
| `%v{name}`, `%R{name}` | Emit a whole byte slice / borrow remaining input |
| `%V{name}`, `%N{name}` | Pop length; emit/capture that exact byte slice |
| `%i`, `%j` | Pop value then width (width on top) and emit bits / pop width and read bits |
| `%J[n]{name}` | Pop count and terminator (terminator on top); apply earlier table entry n to the named record list |

`%u` introduces generic unsigned/record operations in the same interpreter:

| Suffix after `%u` | Effect |
|---|---|
| `+ - * / m & \| ^ [ ]` | Unsigned binary arithmetic, modulo, bit operations, left/right shift |
| `= ~ < l > g A O` | Equal, unequal, less, less/equal, greater, greater/equal, Boolean AND/OR |
| `!`, `?` | Boolean NOT; eager select (condition, true value, false value) |
| `r p o k n` | Remaining bytes, absolute position, record start, peek input, record index |
| `h{name}` | Pop byte index; push byte from named slice |
| `I{list}{member}` | Pop index; push the child's numeric member |
| `U{list}{member}`, `C{list}{member}` | Test uniqueness / pop value and count equal members |
| `Z{name}` | Clear an absent decoded field, preserving allocated child storage |
| `a` | Pop error status and predicate; fail with that status if predicate is false |
| `f` | Require a byte boundary |
| `s` | Pop width then value; push byte-swapped value |
| `b` | Pop CBOR major then value; emit/read the header and push its value |
| `d` | Pop BCD flag, digit count, value; emit/read decimal digits and push value |
| `T` | Pop stop value, mask, encoded length, value; emit/read terminated octets, push value |
| `K{name}` | Pop flags, checksum start, algorithm; emit/verify named CRC |
| `F` | Require byte alignment and finish a deferred CRC at record end |

CRC flags are bit 0 for CBOR byte-string framing and bit 1 for deferral to
record end. Algorithm identifiers are listed above. Scalar helpers with both
encode/decode behavior use the direction set by `wi_init`/`wi_decode_init`.
A record format must leave the RPN stack empty and end on a byte boundary.

For example, a byte field with constant 10 compiles to an encode fragment
`%{10}%P{tag}%g{tag}%{8}%i`. A matching decoder uses `%{8}%j%P{tag}` followed
by a comparison/assertion. These are the actual executed strings, available
through `wire_info_format(info, message, decode)` or `strings protocol.wi`.

## Binary layout

The file extends the existing [WI layout](WI_FILE_FORMAT.md). Header integers
are little-endian and all offsets are file-relative unless stated otherwise.
No host pointers or C structs are embedded.

The 64-byte header keeps magic `WI\0\0`, u16 version 4 at offset 4, u16 header
size 64 at 6, and the existing u32 slots:

| Offset | Value |
|---|---|
| 8, 12 | File size, protocol flags (zero) |
| 16, 20, 24 | Message count, message row size (32), message table offset (64) |
| 28, 32 | Metadata/value table offset and byte length |
| 36, 40 | Protocol string-offset section address and count (4) |
| 44, 48 | Combined string-offset sections address and u32 entry count |
| 52, 56 | String table address and byte length |
| 60 | Reserved zero |

Tables are contiguous in that order. The protocol's four string slots are
name, description, standard and reference. Each 32-byte message row contains:

| Offset | Value |
|---|---|
| 0, 4 | Message string-offset section address, count (4 + field count) |
| 8, 12 | Reserved minimum size (zero), field count |
| 16, 20 | Old width/swap table addresses (zero for named records) |
| 24 | Named-record flag (2) |
| 28 | This message's field metadata address |

Each field metadata row is two u32 values: type (the `wire_info_type_t` enum)
and child message index, or `0xffffffff` for non-record fields. Metadata has
no width expressions, conditions, checksum algorithms or instruction bytecode.
Those rules exist only in the format strings.

A message's string slots are name, description, **encode format, decode
format**, then field names in record order. String-offset entries are relative
to the string table; `0xffffffff` denotes an absent optional description.
Strings are NUL terminated. The format table passed to `wi_set_formats` uses
message order; `%J[n]` references a strictly earlier child entry.
