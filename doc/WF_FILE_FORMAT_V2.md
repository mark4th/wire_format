# The `.wf` source format, version 2

Version 2 is a compatible extension of version 1 for records with a
variable-length final byte field. All version-1 scalar, bit-field, constant,
layout, identifier, and vector rules continue to apply.

Select it with:

```json5
{ wire_format: 2, protocol: { /* ... */ } }
```

## Variable byte field

A message may end with one caller-supplied `bytes` field:

```json5
fields: [
  { name: 'kind', type: 'u8' },
  { name: 'data', type: 'bytes' },
]
```

The field must:

- be the final field;
- begin on an octet boundary;
- have no `width` or `constant`; and
- use an octet array in every test vector.

```json5
values: { kind: 2, data: [0xaa, 0xbb, 0xcc] }
```

Encoding attaches the byte field to its generated caller-field ordinal with
`wi_set_slice()` and executes the generated program. Decoding captures all
input remaining after the fixed prefix without copying it; the result is in
`wi_vars_t.slices[ordinal]`. Rust callers use `Param::Raw` or `set_slice()`
and retrieve the result with `slice()`.

The compiled message's `WIRE_SIZE` is the minimum/fixed-prefix size.
`VARIABLE_WIRE_SIZE` is one for a message ending in `bytes` and zero otherwise.

This initial version-2 rule is intentionally bounded: the compiler never
guesses a byte-field boundary. Protocol adapters validate inline lengths,
optional trailers, choices, checksums, and stateful behavior before presenting
the exact record extent to the generated decoder.
