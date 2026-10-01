# The compiled `.wi` format

Version numbers in this document identify **compiled-format revisions**, not
library releases. The current compilers emit a revision matching the input
`.wf` source revision. See [versioning and feature history](VERSIONING.md)
for what each revision added, when its implementation appeared, and the
backward-compatibility policy. The initial sections describe revision 1;
later sections specify revisions 2–4.

## Purpose

`wfc` compiles one human-readable `.wf` protocol description into two files:

```text
protocol.wf -> protocol.wi + protocol.h
```

The `.wi` contains the protocol data.  The `.h` gives symbolic names to the
numeric indexes and offsets in that data.  Neither file decides how an
application stores or obtains the binary.  Reading it from a file, placing it
in flash with `.incbin`, copying it from another device, and converting it to a
language array are all application choices.

The format contains no pointers, native structures, or implicit padding.  All
multi-octet integers are unsigned and little-endian.  Unless stated otherwise,
an offset is an absolute octet offset from the beginning of the `.wi`.

## File organization

```text
file header
message records
caller-field value tables
alignment padding
string-offset sections
string table
```

The string table is last.  This is deliberate: the preceding sections have
fixed-size entries, while strings do not.

## File header

The version 1 header is 64 octets:

| Offset | Size | Meaning |
|-------:|-----:|---------|
| 0 | 4 | magic: `57 49 00 00`, or `WI\0\0` |
| 4 | 2 | compiled-format version: `1` |
| 6 | 2 | header size: `64` |
| 8 | 4 | total file size |
| 12 | 4 | protocol and compiled-file flags |
| 16 | 4 | message count |
| 20 | 4 | message-record size: `32` |
| 24 | 4 | message-record table offset |
| 28 | 4 | caller-field value-table offset |
| 32 | 4 | caller-field value-table size |
| 36 | 4 | protocol string-section offset |
| 40 | 4 | protocol string count: `4` |
| 44 | 4 | complete string-offset section offset |
| 48 | 4 | complete string-offset entry count |
| 52 | 4 | string-table offset |
| 56 | 4 | string-table size |
| 60 | 4 | CRC-32C of the complete file, stored little-endian |

Protocol flag bit 0 is set when the described wire protocol uses little-endian
scalars.  Bit 1 is set when it numbers bit fields least-significant bit first.
Bit 31 is the mandatory `WI_FLAG_FILE_CRC32C` compiled-file integrity flag.
Bits 0 and 1 describe the protocol, not the `.wi`; the `.wi` integers are
always little-endian. All other flag bits are zero.

The CRC uses the reflected CRC-32C (Castagnoli) polynomial `0x82f63b78`, an
initial value of `0xffffffff`, and a final XOR of `0xffffffff`. It covers every
octet in the file, including the header, tables, alignment padding and string
table. Header octets 60 through 63 are treated as zero during calculation.
Readers must reject an image whose stored and calculated CRC values differ.

The protocol string section contains four 32-bit entries:

| Slot | String |
|-----:|--------|
| 0 | protocol name |
| 1 | protocol description |
| 2 | standard |
| 3 | reference |

An absent optional string has the offset `0xffffffff`.  Required strings
always have an entry.

## Message records

There is one 32-octet record for every message, in source order:

| Offset | Size | Meaning |
|-------:|-----:|---------|
| 0 | 4 | this message's string-section offset |
| 4 | 4 | this message's string count |
| 8 | 4 | fixed wire size in octets |
| 12 | 4 | caller-supplied field count |
| 16 | 4 | caller-field width-table offset, or zero |
| 20 | 4 | caller-field swap-table offset, or zero |
| 24 | 4 | reserved; zero in version 1 |
| 28 | 4 | reserved; zero in version 1 |

The message ordinal is its zero-based record number.  The associated header
names both the ordinal and the absolute record offset.

The width table contains one octet per caller-supplied field, in declaration
order.  Each octet is that field's width in bits.  The swap table has the same
number of entries.  A swap entry is zero for a bit field, an octet, or a
big-endian scalar.  It is 2, 4, or 8 for a little-endian scalar of that many
octets.  This describes the transformation needed when the common parser's
big-endian scalar operators are used.  A message without caller-supplied
fields has zero for both table offsets.

## Message string sections

A message string section is an array of 32-bit offsets.  The fixed entries are:

| Slot | String |
|-----:|--------|
| 0 | message name |
| 1 | message description |
| 2 | compiled encode program |
| 3 | compiled decode program |
| 4 onward | field names in declaration order |

The field-name entries include both caller-supplied and constant fields.  The
associated header names every slot.  It separately names caller-field
ordinals, which include only caller-supplied fields.

## String offsets and the string table

Every entry in a protocol or message string section is an offset relative to
the beginning of the common string table at file-header offset 52.  It is not
an absolute file offset and it is not a pointer.

For example, obtaining a message's encode program is conceptually:

```text
entry = message-string-section + (encode-slot * 4)
relative = little-endian-u32(entry)
string = wi-base + string-table-offset + relative
```

This is the same two-stage arrangement used by compiled `terminfo`: a named
slot selects an entry in a string-offset section, and that entry selects bytes
in a shared string table.

Strings are UTF-8 followed by one NUL octet.  Compiled encode and decode
programs use the ASCII subset.  A source string containing an embedded NUL is
rejected.  Version 1 does not require string deduplication, and applications
must not depend on two equal strings sharing an offset.

## The associated C header

The generated header contains only integer constants and comments.  It does
not include `wire_format.h`, define a native representation of the file, or
declare storage for the `.wi`.

For each protocol it names:

- file and section sizes and offsets;
- protocol string slots;
- every message ordinal and record offset;
- every message string-section offset and string slot;
- every caller-field ordinal; and
- every message's caller-field count and fixed wire size.

The header also names fixed-field fault bit 0.  A compiled decode program
raises this bit with `%E` when a received constant differs from its declared
value.  Whether that is fatal remains application policy.

Hyphens in source identifiers become underscores and names are converted to
uppercase.  For example, protocol `example-telemetry`, message `status`, and
field `sample-count` produce names including:

```c
#define EXAMPLE_TELEMETRY_MESSAGE_STATUS                       0u
#define EXAMPLE_TELEMETRY_STATUS_STRINGS_OFFSET                /* ... */
#define EXAMPLE_TELEMETRY_STATUS_STRING_ENCODE                 2u
#define EXAMPLE_TELEMETRY_STATUS_ORDINAL_SAMPLE_COUNT          2u
```

The numeric value of an offset is generated in place of the comment above.
The names are conveniences, not a required loading API.

## Validation and versioning

Before using a `.wi`, an application must validate its CRC before exposing any
contained strings or metadata. It must also validate the magic, version,
header and file sizes, section ranges, record size, string offsets, and NUL
termination. The CRC detects accidental corruption; it is not an authenticity
or tamper-resistance mechanism. A signature or authenticated containing image
is still required when the threat model includes deliberate modification.

The CRC requirement was added in library release 0.2.0 without changing
source-format revisions 1 through 4. Newly compiled files set flag bit 31 and
contain the CRC; current readers reject files without it. Existing `.wi` files
from release 0.1.0 must be recompiled from their `.wf` sources.

## Version 2 additions

Version 2 retains the 64-octet header and 32-octet message-record layout.
Header offset 4 contains compiled-format version 2. Message-record offset 24
uses bit 0 to indicate that the message ends in a variable `bytes` field.
The wire-size entry at offset 8 is then the fixed-prefix/minimum size.

The byte field has a zero width-table entry and uses its ordinary caller-field
ordinal as a slice slot. Its encode program contains `%v`; its decode program
contains `%R`. All other reserved bits and fields remain zero.

## Version 3 additions

Version 3 retains the same header and message-record layout. A message record's
variable-size flag is set when any field is `sdnv`, `bytes`, `records`, or
`choice`; the wire-size entry is the fixed/minimum size.

SDNV fields compile to `%d`/`%D`. Reciprocal `length_of`/`length_from`
relationships compile to `%z`, `%V`, and `%N`. Reciprocal
`count_of`/`count_from` relationships use `%k` and `%J[n]` over caller-owned
record lists. Selector-driven choices compile to conditionals containing one
`%J[n]` call for each declared case. These additions change format programs
and runtime inputs, but do not add binary sections or change record offsets.

## Version 4 additions: named records

Version 4 keeps the header, message rows and encode/decode string slots and
adds field metadata plus named operands for the same runtime interpreter.
See [the version 4 guide](WIRE_INFO_V4.md).
