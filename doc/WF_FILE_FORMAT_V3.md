# The `.wf` source format, version 3

This is **source-format revision 3**, not library release 3. Its C and Rust
support was added in development commit
[`92f5533`](https://github.com/mark4th/wire_format/commit/92f5533)
(2026-09-29). **First included in tagged library release 0.1.0 (C and Rust).**
It retains support for the earlier features; see
[versioning and feature history](VERSIONING.md).

Version 3 adds bounded variable fields and structured variable repetition to
the version-2 format. All earlier scalar, bit-field, constant, identifier,
layout, and vector rules remain compatible.

Select it with:

```json5
{ wire_format: 3, protocol: { /* ... */ } }
```

## SDNV integers

`sdnv` is an unsigned 64-bit Self-Delimiting Numeric Value. It occupies from
one through ten wire octets and is therefore a variable-size field:

```json5
{ name: 'session-number', type: 'sdnv' }
```

The generated encoder and decoder use `%d` and `%D`, respectively. Overflowing
or unterminated input is rejected after at most ten octets.

## Length-delimited byte slices

A scalar or SDNV length field and a later `bytes` field may form a reciprocal
relationship:

```json5
fields: [
  { name: 'length', type: 'sdnv', length_of: 'value' },
  { name: 'value', type: 'bytes', length_from: 'length' },
  { name: 'trailer', type: 'u8' },
]
```

The length is derived from the attached slice during encoding and is not a
caller field. Decoding captures exactly that many octets without copying, so a
bounded byte field may occur before later fields. Both names must reference
each other, and the length field must precede the byte field.

`%z` obtains an attached slice length, `%V` verifies and emits an exact-length
slice, and `%N` captures an exact-length slice.

## Counted record sequences

A scalar or bit field may derive the count of a later `records` field:

```json5
{ name: 'item-count', type: 'sdnv', count_of: 'items' },
{
  name: 'items',
  type: 'records',
  message: 'item',
  count_from: 'item-count',
}
```

The relationship is reciprocal and ordered like a byte length. The referenced
message must appear earlier in the protocol, which preserves the compiled
format table's acyclic, no-forward-reference rule.

Record storage is caller-owned and allocation-free. In C, attach a
`wi_record_list_t` with `wi_set_record_list()`. Each `wi_record_t` contains the
child message's scalar values, slices, and optional nested lists at the
generated caller-field ordinals. Rust uses `Record`, `RecordList`, and
`WireFormat::set_record_list()`.

`%k` pushes a list's count. `%J[n]` consumes count, list slot, and child field
count from the stack, then encodes or decodes message `n` once per record.
Decode rejects a count larger than the caller-provided capacity.

## Message alternatives

A `choice` selects one earlier message using a preceding scalar field:

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

The choice occupies one record-list caller field. Its list contains one record
for the selected child. Cases must have unique selector values and may only
reference earlier messages. Protocol-level rules may impose a closed selector
set; the structural decoder deliberately does not invent semantic policy for
an unmatched value.

## Test vectors

Vectors continue to cover scalar, SDNV, and byte fields. Messages containing
`records` or `choice` fields are exercised through the generated runtime
programs rather than inline vectors; their referenced leaf messages retain
ordinary source vectors. The C and
Rust `wfc` conformance suite executes generated counted-record and choice
programs in addition to comparing their `.wi` and header output byte for byte.
