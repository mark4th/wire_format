# The `.wf` source format

This document specifies **source-format revision 1**, not library release 1.
Its `wfc` implementations written in C and Rust were added in development commit
[`ba74aaa`](https://github.com/mark4th/wire_format/commit/ba74aaa)
(2026-09-22); the current `.wi` container followed in `bf6fc14` (2026-09-23).
**First included in tagged library release 0.1.0 (C and Rust).** Revision 1
remains supported by the current C and Rust implementations. See
[versioning and feature history](VERSIONING.md) for release provenance and
the compatibility policy.

## Purpose

A `.wf` file is the human-readable description of one wire protocol.
`wfc` checks that description and compiles it into a position-independent
`.wi` database plus a C header naming the database contents.

JSON5 is used because protocol descriptions are source code, not data exchanged
on the wire. It permits comments, trailing commas, hexadecimal integers,
unquoted object keys, and single-quoted strings. The accepted object shape is
still strict: misspelled or unknown properties are errors.

The JSON Schema is
[../schema/wire-format-v1.schema.json](../schema/wire-format-v1.schema.json).
The schema documents the source structure. `wfc` also performs layout and test
vector checks which JSON Schema cannot express.

The application decides how the compiled database is made available. It may
load the `.wi`, embed it in an executable, or place it in firmware storage.
Neither the source format nor the compiler imposes a run-time loader.

A source file describes records on the wire. It does not describe what an
application does with a record, when it sends one, how it retries a failed
transaction, or which state follows another state. Those are properties of
the application and its protocol engine.

Version 1 describes fixed-size records.
The compatible variable-byte-tail extension is documented separately in
[WF_FILE_FORMAT_V2.md](WF_FILE_FORMAT_V2.md).
Version 3 is specified in [WF_FILE_FORMAT_V3.md](WF_FILE_FORMAT_V3.md).

## Complete example

```json5
{
  wire_format: 1,

  protocol: {
    name: 'example-telemetry',
    description: 'A small source-format example',
    standard: 'Example 1',
    reference: 'https://example.invalid/example-1',

    byte_order: 'big-endian',
    bit_order: 'msb-first',

    messages: [
      {
        name: 'status',
        description: 'A three-octet status record',

        fields: [
          { name: 'version', type: 'bits', width: 3 },

          // Constants are emitted and checked without becoming caller values.
          { name: 'reserved', type: 'bits', width: 1, constant: 0 },

          { name: 'mode', type: 'bits', width: 4 },
          { name: 'sample-count', type: 'u16' },
        ],

        vectors: [
          {
            name: 'nominal',
            values: {
              version: 1,
              mode: 2,
              'sample-count': 0x1234,
            },
            wire: [0x22, 0x12, 0x34],
          },
        ],
      },
    ],
  },
}
```

The record begins with three fields packed into one octet. The `u16` begins at
the next octet and follows `byte_order`. The caller supplies `version`, `mode`,
and `sample-count`. The generated encoder supplies the reserved zero, and the
generated decoder raises a fixed-field fault if it is not zero.

## Source rules

The file is UTF-8 JSON5. In addition to ordinary JSON, the compiler accepts:

- `//` and `/* ... */` comments;
- trailing commas;
- unquoted object keys when JSON5 permits them;
- single-quoted strings; and
- hexadecimal unsigned integers beginning with `0x`.

The file must contain exactly two top-level properties:

| Property | Meaning |
|----------|---------|
| `wire_format` | Source-format version. Version 1 requires the integer `1`. |
| `protocol` | The protocol object described below. |

Unknown properties are errors at every level. An integer used for a value or
constant must fit in an unsigned 64-bit value.

### Identifiers

Protocol, message, field, and vector names are identifiers. An identifier:

- starts with `a` through `z`;
- contains only lower-case ASCII letters, digits, and hyphens;
- does not end in a hyphen; and
- does not contain two adjacent hyphens.

Identifiers are stable generated-interface names, not prose labels. The C
header changes hyphens to underscores and converts names to upper case.

## The protocol object

| Property | Required | Meaning |
|----------|:--------:|---------|
| `name` | yes | Protocol identifier. |
| `description` | yes | Human-readable description. |
| `standard` | no | Standard name and revision. |
| `reference` | no | Source document, section, or URL. |
| `byte_order` | yes | `big-endian` or `little-endian`. |
| `bit_order` | yes | `msb-first` or `lsb-first`. |
| `messages` | yes | Nonempty array of message objects. |

Descriptions and present optional text must not be empty and must not contain
a NUL character. Message names must be unique within the protocol.

Orders are explicit even if the first version of a protocol happens to use
only octet fields. The compiler never selects the host's native order.

## Message objects

| Property | Required | Meaning |
|----------|:--------:|---------|
| `name` | yes | Message identifier. |
| `description` | yes | Human-readable description. |
| `fields` | yes | Nonempty array of field objects in wire order. |
| `vectors` | no | Array of independently derived test vectors. |

The order of `messages` determines the compiled message ordinals. The order of
`fields` defines the wire layout. Message, field, and vector names must be
unique in their respective scopes. Native structure layout is never used.

## Field objects

A caller-supplied field contains `name` and `type`:

```json5
{ name: 'apid', type: 'bits', width: 11 }
{ name: 'sequence-count', type: 'u16' }
```

A constant field also contains `constant`:

```json5
{ name: 'version', type: 'bits', width: 3, constant: 0 }
{ name: 'marker', type: 'u16', constant: 0x1acf }
```

| Type | Extra property | Wire representation |
|------|----------------|---------------------|
| `bits` | `width`, from 1 through 64 | Unsigned packed bit field. |
| `u8` | none | One octet. |
| `u16` | none | Two octets in `byte_order`. |
| `u32` | none | Four octets in `byte_order`. |
| `u64` | none | Eight octets in `byte_order`. |

`width` is required for `bits` and forbidden for scalar types. Values must fit
their declared widths. The compiler rejects a value rather than silently
discarding its high bits.

A constant is not present in the caller's value array. The encoder emits it
automatically. The decoder compares the received value with it and raises the
fixed-field fault when they differ. The calling application decides whether
that fault invalidates the record.

Names such as `reserved`, `spare`, `version`, and `marker` have no built-in
meaning.

## Bit and scalar layout

Fields consume the record from left to right in array order. No padding or
alignment is inserted.

`msb-first` places the first bit in bit 7 of the current octet and continues
toward bit 0. A field crossing an octet boundary continues at bit 7 of the
next octet.

`lsb-first` places the first bit in bit 0 and continues toward bit 7. A field
crossing an octet boundary continues at bit 0 of the next octet.

`byte_order` controls `u16`, `u32`, and `u64`. `bit_order` completely defines
`bits` fields.

The following rules prevent implicit host-layout assumptions:

- scalar fields must begin on an octet boundary;
- a message must end on an octet boundary;
- adjacent `bits` fields may cross octet boundaries; and
- an alignment error is reported instead of inserting padding.

## Test vectors

A vector has a name, a `values` object, and a `wire` octet array:

```json5
{
  name: 'nominal',
  values: {
    version: 1,
    mode: 2,
    'sample-count': 0x1234,
  },
  wire: [0x22, 0x12, 0x34],
}
```

Every caller-supplied field must occur exactly once in `values`. Constants must
not occur there. Value-property order has no meaning. Each `wire` element is an
unsigned octet and the array must not be empty.

For every vector, `wfc`:

1. Encodes `values` and compares every octet with `wire`.
2. Decodes `wire`, compares every returned field with `values`, and verifies
   every constant.

Vectors are compile-time source tests. They are not stored in the `.wi`. The
format permits a private definition to omit vectors, but committed protocol
definitions should have at least one independently derived vector per message.

## Compiler interface

The `wfc` implementations written in C and Rust provide the same interface:

```text
wfc check protocol.wf
wfc protocol.wf --output build/protocol
```

`check` stops after parsing, layout validation, and two-way execution of every
vector. Without `check`, compilation is implied.

Compilation creates `build/protocol.wi` and `build/protocol.h`. The `.wi` is
a position-independent database. The header names message ordinals, record
offsets, string sections, string slots, caller-field ordinals, and fixed wire
sizes. It contains no generated functions or storage policy.

The output is deterministic. Both implementations must produce byte-for-byte
identical files for the same source. The conformance test enforces that rule.
A failed compilation does not leave a mixture of old and new outputs.

The compiled format accepts at most 16 caller fields in one message, matching
`WI_MAX_PARAMS`. A caller-supplied `bits` field may be at most 63 bits because
bit-field arithmetic uses the signed interpreter stack. An octet-aligned
64-bit caller value uses `u64`. Constants may use all 64 bits because they are
compiled a fragment at a time.

## Diagnostics and checks

`wfc` rejects:

- malformed JSON5 or an unsupported source version;
- unknown, misspelled, duplicate, or missing properties;
- invalid or duplicate identifiers;
- empty required text, protocols, messages, or wire arrays;
- invalid field types or widths;
- values which do not fit their fields;
- unaligned scalar fields or non-octet message sizes;
- incomplete or incorrect vectors; and
- messages exceeding a compiled-format resource limit.

No field, message, or vector is silently discarded.

## What version 1 does not contain

Version 1 has no:

- prescribed run-time loader or storage mechanism;
- includes, macros, or conditional compilation;
- variable-length byte fields or arrays;
- calculated length fields;
- optional or conditional fields;
- checksums or error-correction calculations;
- floating-point or signed wire fields;
- host-language structure overlays;
- application state machines, timeouts, or retry rules; or
- generated per-message behavioral code.

These omissions keep the first format small and explicit. An incompatible
source extension requires a new `wire_format` version so an older compiler
rejects it instead of compiling a different record.

## Named-record source extension

The C `wfc` also accepts `wire_format: 4` `.wf` definitions with expressions,
conditions and named nested records. These compile to runtime format strings
in `.wi`, documented in [the version 4 guide](WIRE_INFO_V4.md).
