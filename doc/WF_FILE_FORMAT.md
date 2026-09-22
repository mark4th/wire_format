# The `.wf` source format

## Purpose

A `.wf` file is the human-readable description of the wire layout used by
one protocol.  `wfc` reads that description, checks it, and generates a
compiled `.wfb` database plus a header naming its contents.

The application decides how the compiled database is made available.  It may
load the `.wfb`, embed it in an executable, or place it in firmware storage.
The source language and compiler do not add run-time protocol discovery or
choose a storage mechanism.

A `.wf` file describes how records are represented on the wire.  It does not
describe what an application does with a record, when it sends one, how it
retries a failed transaction, or which state follows another state.  Those
are properties of the application and its protocol engine.

Version 1 deliberately describes fixed-size records.  This is sufficient for
the primary headers of the first CCSDS definitions.  Variable-length byte
strings, calculated lengths, checksums, and conditional fields will be added
only after their source representation and run-time bounds are equally clear.

## A complete example

```text
wire-format 1

protocol example-telemetry
description "A small example used to document the source language"
standard "Example 1"
reference "https://example.invalid/example-1"

byte-order big-endian
bit-order msb-first

message status
description "A three-octet status record"

field version bits 3
constant reserved bits 1 = 0
field mode bits 4
field sample-count u16

vector nominal
version = 1
mode = 2
sample-count = 0x1234
wire 22 12 34
end-vector

end-message
end-protocol
```

This record begins with three fields packed into one octet.  The `u16` begins
at the next octet and follows the file's declared byte order.  The caller
supplies `version`, `mode`, and `sample-count`.  The generated encoder supplies
the reserved zero itself, and the generated decoder reports a nonzero reserved
field.

## Lexical rules

A `.wf` file is UTF-8 text.  The language keywords and identifiers use ASCII.

- A blank line is ignored.
- `#` begins a comment which continues to the end of the line.  A `#` inside
  a quoted string is ordinary text.
- One statement occupies one physical line.  Version 1 has no continuation
  character and no semicolons.
- Keywords are lower case and case-sensitive.
- An identifier begins with `a` through `z`.  The remaining characters may
  be lower-case letters, digits, or hyphens.
- An identifier may not begin or end with a hyphen, and may not contain two
  adjacent hyphens.
- Decimal integers and hexadecimal integers beginning with `0x` are accepted.
  Underscores may separate digits and have no value.
- A string is enclosed in double quotes.  The escapes `\\`, `\"`, `\n`, and
  `\t` are accepted.  A string may not cross a physical line.
- The `wire` statement contains whitespace-separated hexadecimal octets.
  Every octet is written as exactly two hexadecimal digits without `0x`.

Identifiers are part of the generated interface and are therefore stable
names, not prose labels.  The generated C header changes hyphens to
underscores.

## File structure

The first non-comment statement must be:

```text
wire-format 1
```

It is followed by exactly one protocol:

```text
protocol protocol-name
description "What this protocol definition covers"
standard "Optional standard name and revision"
reference "Optional source or URL"

byte-order big-endian
bit-order msb-first

message first-message
...
end-message

message second-message
...
end-message

end-protocol
```

`description` is required for the protocol.  `standard` and `reference` are
optional and may each occur once.  A file must contain at least one message.
Statements occur in the order shown.  When both optional statements are
present, `standard` precedes `reference`.  A description, standard, or
reference which is present must contain something other than whitespace.

`byte-order` and `bit-order` are required even when a file currently uses only
one-octet fields.  An omitted order is an ambiguity, not permission for the
compiler to choose the host's order.

Version 1 accepts these byte orders:

```text
byte-order big-endian
byte-order little-endian
```

Version 1 accepts these bit orders:

```text
bit-order msb-first
bit-order lsb-first
```

The order applies to every message in the file.  Two parts of a protocol with
different orders belong in separate `.wf` files.

## Messages

A message begins with `message name` and ends with `end-message`.  Its first
statement must be a description:

```text
message status
description "The fixed status header"
...
end-message
```

A message contains one or more field declarations followed by zero or more
test vectors.  Declarations and vectors may not be interleaved.

Message and field identifiers must be unique in their respective scopes.
Generated values appear in field declaration order; no source or target ABI
structure layout is used.

The complete statement grammar is:

```text
file             = version protocol-header message+ "end-protocol"
version          = "wire-format" "1"
protocol-header  = "protocol" identifier
                   "description" string
                   [ "standard" string ]
                   [ "reference" string ]
                   "byte-order" byte-order
                   "bit-order" bit-order
byte-order       = "big-endian" | "little-endian"
bit-order        = "msb-first" | "lsb-first"
message          = "message" identifier
                   "description" string
                   declaration+ vector*
                   "end-message"
declaration      = field | constant
field            = "field" identifier type
constant         = "constant" identifier type "=" integer
type             = "bits" integer | "u8" | "u16" | "u32" | "u64"
vector           = "vector" identifier
                   assignment* wire+
                   "end-vector"
assignment       = identifier "=" integer
wire             = "wire" hex-octet+
```

Each quoted word in this grammar is a keyword appearing at the start of its
own physical line.  The remaining items on that grammar row are its arguments;
the grammar does not join several statements onto one source line.  Blank
lines and comments may occur between any two statements.

### Caller-supplied fields

`field` declares a value supplied by the caller during encoding and returned
to the caller during decoding:

```text
field apid bits 11
field sequence-count u16
```

Version 1 has only unsigned fields.  The available types are:

| Type | Wire representation |
|------|---------------------|
| `bits N` | An unsigned field from 1 through 64 bits wide |
| `u8` | One octet |
| `u16` | Two octets in the declared byte order |
| `u32` | Four octets in the declared byte order |
| `u64` | Eight octets in the declared byte order |

The value of a field must fit its declared width.  A compiler must diagnose an
out-of-range constant or vector value.  A generated encoder must reject an
out-of-range caller value rather than discard its high bits.

### Constant fields

`constant` declares a named field whose wire value is fixed by the protocol:

```text
constant version bits 3 = 0
constant marker u16 = 0x1acf
```

A constant is not present in the caller's value array.  The encoder emits it
automatically.  The decoder compares the received value with it and reports a
fixed-field fault when they differ.  Calling code decides whether that fault
invalidates the complete record, but it cannot accidentally treat the field
as caller-controlled data.

Names such as `reserved`, `spare`, `version`, and `marker` have no special
meaning.  Their meaning comes from the declaration and the protocol document.

## Bit layout

Declarations consume the wire record from left to right in source order.  No
padding or alignment is ever inserted.

`msb-first` means that the first bit occupies bit 7 of the current octet and
subsequent bits proceed toward bit 0.  The most significant bit of a field is
transmitted first.  A field which crosses an octet boundary continues at bit 7
of the next octet.

`lsb-first` means that the first bit occupies bit 0 of the current octet and
subsequent bits proceed toward bit 7.  The least significant bit of a field is
transmitted first.  A field which crosses an octet boundary continues at bit 0
of the next octet.

`byte-order` controls `u16`, `u32`, and `u64`.  It does not change the meaning
of `bits N`; `bit-order` completely defines those fields.

The following rules prevent implicit host-layout assumptions:

- A `u8`, `u16`, `u32`, or `u64` must begin on an octet boundary.
- A message must end on an octet boundary.
- Adjacent `bits N` declarations may cross octet boundaries.
- The compiler reports an alignment error instead of inserting padding.

The wire layout therefore remains identical on targets with different native
endianness, integer alignment, or C structure-packing rules.

## Test vectors

A vector belongs to the message which contains it:

```text
vector nominal
version = 1
mode = 2
sample-count = 0x1234
wire 22 12 34
end-vector
```

Every caller-supplied field must be assigned exactly once.  Constant fields
must not be assigned.  One or more `wire` statements may be used; their octets
are concatenated in source order.

Assignments may appear in any order.  Vector names must be unique within their
message.  `field` and `constant` names share one namespace within a message.

For each vector, `wfc check` must perform both tests:

1. Encode the assigned fields and compare every octet with `wire`.
2. Decode `wire`, compare every returned field with its assignment, and verify
   every constant field.

A mismatched size, missing field, duplicate field, unknown field, constant
assignment, encode mismatch, or decode mismatch is an error.

Vectors are source tests.  They are not included in production generated
data.  The `wire_format` protocol library requires at least one independently
derived vector for every committed message, even though the source language
allows a private definition to omit vectors.

## The compiler interface

The compiler is named `wfc`.  Its initial command line is:

```text
wfc check protocol.wf
wfc protocol.wf --output build/protocol
```

`check` stops after parsing, layout validation, and the two-way execution of
every vector.  Otherwise compilation is implied.

The compiler generates `build/protocol.wfb` and `build/protocol.h`.  The `.wfb`
is a position-independent compiled database.  The `.h` file gives names to its
message ordinals, message and string-section offsets, string slots,
caller-field ordinals, and fixed wire sizes.  Its constants describe the
binary; it contains no generated functions, structures, or storage policy.

The `.wfb` contains integer offsets rather than pointers or host-language
structures.  An application may load it, memory-map it, embed it with
`.incbin`, convert it to an array, or obtain it by any other means.  Those are
application decisions and are not compiler options.

Message names, descriptions, encode and decode programs, and field names are
stored as NUL-terminated strings in one common string table.  Each message has
a string-offset section whose entries are relative offsets into that table,
following the compiled `terminfo` model.  Test vectors are checked by `wfc`
but are not stored in the production database.

The complete binary layout is defined in `WFB_FILE_FORMAT.md`.

Output must be deterministic: the same source and compiler version produce
byte-for-byte identical generated files.  A failed compilation must not leave
a mixture of old and new output files.

The compiled format accepts at most 16 caller-supplied fields in one message,
which matches `WI_MAX_PARAMS`.  A caller-supplied `bits N` field may be at most 63
bits because bit-field arithmetic uses the signed interpreter stack.  An
octet-aligned 64-bit field should be declared `u64`; constants may use all 64
bits because they are compiled a fragment at a time.  These are diagnosed
compiled-format limits, never silent truncations.

## Required diagnostics

`wfc` rejects a file which contains any of the following:

- an unsupported source-language version;
- an unknown, misspelled, misplaced, or duplicate statement;
- an invalid or duplicate identifier;
- a missing required description or order declaration;
- an empty protocol or message;
- a field width outside its permitted range;
- a value which does not fit its field;
- a scalar field which is not octet-aligned;
- a message whose final bit is not on an octet boundary;
- a malformed or incorrect test vector; or
- a message which exceeds a documented compiled-format resource limit.

Diagnostics identify the source file and physical line.  When a particular
token is at fault they identify its column as well.  A compiled-format limit
is always an error; fields, messages, or vectors are never silently discarded.

## What version 1 does not contain

Version 1 has no:

- a prescribed run-time loader or storage mechanism;
- includes, macros, or conditional compilation;
- variable-length byte fields or arrays;
- calculated length fields;
- optional or conditional fields;
- checksums or error-correction calculations;
- floating-point or signed wire fields;
- host-language structure overlays;
- application state machines, timeouts, or retry rules; or
- generated per-message behavioral code.

These omissions are not claims that protocols do not need those features.
They keep the first language sufficient for fixed protocol headers without
hiding policy or unbounded work inside the format description.  A grammar
extension requires a new `wire-format` version so an older compiler rejects it
instead of quietly compiling a different record.
