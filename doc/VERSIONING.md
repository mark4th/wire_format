# Versions, feature history and compatibility

## Which version is which?

| Version | Where it appears | What it identifies |
|---|---|---|
| Library/tool release | Package manifests and `wfc --version` | A particular release of the implementation |
| Source-format revision | `wire_format` in a `.wf` file | The accepted declaration vocabulary and caller-data model |
| Compiled-format revision | The u16 at offset 4 in a `.wi` header | The binary table layout and interpretation of its contents |
| Shared-library ABI | `.4` in `libwire_info.so.4` | The C binary interface used when linking applications |
| Protocol version | A field in the protocol being described, if it has one | The network protocol's own version, independent of all the above |

The current library/tool version is **0.2.0**. The native `wfc` and Rust package
versions agree. Release 0.1.0 remains identified by the annotated Git tag
[`v0.1.0`](https://github.com/mark4th/wire_format/tree/v0.1.0); use a release
tag or commit identifier when selecting an exact implementation.

Source format 4 does **not** mean library release 4.0, and the shared-library
SONAME is not a release number either. See the [release notes](../CHANGELOG.md)
for the contents and limitations of each release.

The current compilers emit a `.wi` revision matching the selected `.wf`
revision. These numbers describe the files; they do not require a matching
library release number. Handwritten format strings have no version header:
their minimum implementation requirement depends on the operations they use.

## Compatibility policy

New library versions must retain support for earlier documented features and
their behavior unless an explicit deprecation is documented. Adding a source
or compiled-format revision does not deprecate earlier revisions. There are
currently no deprecated features in the documented revisions 1–4.

Keep an existing `.wf` file's `wire_format` value when upgrading the library.
Change it only when adopting a newer source vocabulary. Revisions 2 and 3
extend the original declarations; revision 4 introduces a different named-field
model. Existing revision-1–3 definitions remain supported without conversion.

Support is specific to the implementation and interface:

- The native `wfc` implementation accepts source revisions 1–4. The C interpreter
  retains the earlier numbered-parameter operations alongside the named-record
  operations. Applications using revisions 1–3 keep their existing table
  loading and parameter/slice/record-list interfaces.
- `wire_info_open()` is the named-record adapter and accepts compiled revision
  4 only. It is not a universal loader for older images. This restriction does
  not remove the older interpreter interfaces or their features.
- The Rust `wfc` implementation and interpreter support revisions 1–3. Named-record
  revision 4 has not been implemented in Rust; this is an implementation gap,
  not deprecation of earlier support.
- Readers reject unsupported future compiled revisions rather than assuming
  their layouts or format operations are compatible.

Feature compatibility does not itself promise a stable C struct layout or
binary ABI across development snapshots. Rebuild applications when public
headers or the ABI change; the SONAME identifies shared-library ABI changes.

A deprecation notice must name the affected feature, the library release that
deprecates it, its replacement or migration path, and any planned removal
release. Deprecation alone does not remove support; removal must be announced
separately. Historical feature entries remain in this document after removal.

## What each source/compiled revision added

**First included in tagged library release 0.1.0:** every row below, for the
implementations listed in the support column. The implementation column
separately records when the code was first committed. No earlier release
numbers have been assigned retroactively to development snapshots.

| Revision | Additions | First implementation in repository history | Current support |
|---|---|---|---|
| [1](WF_FILE_FORMAT.md) | Fixed scalar and packed-bit fields, constants, layout checks, test vectors, generated ordinals, encode/decode strings | `wfc` implementations written in C and Rust: [ba74aaa](https://github.com/mark4th/wire_format/commit/ba74aaa), 2026-09-22. The current `.wi` name and `WI` magic replaced the experimental WFB container in [bf6fc14](https://github.com/mark4th/wire_format/commit/bf6fc14), 2026-09-23. | C and Rust |
| [2](WF_FILE_FORMAT_V2.md) | Revision 1 plus one variable-length final byte field, attached slices, `%v`/`%R`, and a variable-size message flag | [bf6fc14](https://github.com/mark4th/wire_format/commit/bf6fc14), 2026-09-23 | C and Rust |
| [3](WF_FILE_FORMAT_V3.md) | Revision 2 plus SDNV integers, length-delimited byte slices, derived lengths/counts, counted child records and selector-driven choices; `%d`/`%D`, `%z`, `%V`/`%N`, `%k` and numbered `%J[n]` | [92f5533](https://github.com/mark4th/wire_format/commit/92f5533), 2026-09-29 | C and Rust |
| [4](WIRE_INFO_V4.md) | Named fields and caller records, unsigned expressions, conditional fields, assertions, sequential bit fields, nested records, marks/checksums, CBOR values and arrays, decimal/BCD and terminated integers; named operands and record metadata, with the reusable `wire_info` loader/adapter | [92f5533](https://github.com/mark4th/wire_format/commit/92f5533), 2026-09-29 | C |

The [compiled-format reference](WI_FILE_FORMAT.md) details the corresponding
binary changes. Revision 4 still executes format strings through the existing
interpreter; it does not introduce a separate bytecode engine.

## Interpreter features that predate those revisions

Source revisions describe what the compiler can express. They are not the
chronology of every interpreter operation: shared subformats, repeated calls
and 64-bit operations already existed before the `.wf` compiler.

All features in this table were **first included in tagged library release
0.1.0**, in the implementations shown. Their original C implementation commits
remain useful when examining pre-release development history.

| Feature | First C implementation | Current support |
|---|---|---|
| Stack-based encoding, parameters, variables, calculations and conditionals | [5cc225a](https://github.com/mark4th/wire_format/commit/5cc225a), 2026-05-26 | C and Rust |
| Incoming-message decoding | [983eb5c](https://github.com/mark4th/wire_format/commit/983eb5c), 2026-05-26 | C and Rust |
| Shared subformat calls, `%[n]` | [63525d1](https://github.com/mark4th/wire_format/commit/63525d1), 2026-08-29 | C and Rust |
| Repeated calls, `%:`, including repetition within child calls | [ccd308a](https://github.com/mark4th/wire_format/commit/ccd308a), 2026-08-29 | C and Rust |
| Configurable call-depth and format-table limits | [7245429](https://github.com/mark4th/wire_format/commit/7245429), 2026-08-29 | C; Rust uses its own limits |
| Scalar-array emission, `%rN` | [a1ef00c](https://github.com/mark4th/wire_format/commit/a1ef00c), 2026-08-30 | C and Rust |
| 64-bit encode/decode operations | [89c2488](https://github.com/mark4th/wire_format/commit/89c2488), 2026-09-15 | C and Rust |

The Rust port first appeared in
[bf0315b](https://github.com/mark4th/wire_format/commit/bf0315b) on 2026-09-22,
with parity for the then-existing C operations completed in
[3550727](https://github.com/mark4th/wire_format/commit/3550727) that day.
Later source-revision additions are listed separately above.

## Release tags and future releases

Use annotated tags named `vMAJOR.MINOR.PATCH`. Published tags identify immutable
release snapshots: do not move or overwrite them. Commit release notes and
version updates before tagging, so a checkout of the tag is self-describing.

Use patch increments for compatible fixes and documentation corrections, and
minor increments for compatible feature additions. Record any incompatible
change and migration instructions explicitly; it must not be hidden in a
patch release. Before 1.0, any necessary breaking change requires a minor
increment; from 1.0 onward it requires a major increment. The compatibility
and deprecation policy above still applies during 0.x development.

For each feature, record both the source/compiled revision it needs, if any,
and the first library release that contains its implementation. Once a release
is established, use wording such as **“Added in library X.Y.Z (C)”**, with
separate C/Rust entries if their availability differs. Until then, label the
entry as development work and identify its commit; do not infer a release
number from a file-format revision.

When publishing a release:

1. Update the native `wfc` version in `compiler/main.c`, both Rust package
   versions in `rust/Cargo.toml` and `rust/wfc/Cargo.toml`, and their entries
   in `Cargo.lock`. Keep them consistent with the intended tag.
2. Update `CHANGELOG.md`, this feature history and any affected feature guides.
   Keep earlier feature and compatibility entries.
3. Run `make -j4 test` and `cargo test --workspace`; resolve failures before
   committing the release changes.
4. Create an annotated tag on that commit, then push the commit and that
   specific tag together. For example, for a future 0.1.1 release:

   ```sh
   git tag -a v0.1.1 -m 'wire_format 0.1.1: describe the release changes'
   git push --atomic origin master refs/tags/v0.1.1
   ```

File-format revisions and the shared-library SONAME change only when their
respective formats or ABI require it, not on every library release.

## Release 0.2.0

Newly generated `.wi` files contain a mandatory CRC-32C at header offset 60
and set header flag bit 31. This integrity field applies to source/compiled
revisions 1 through 4 and does not change their protocol-description features.
It is nevertheless a compiled-artifact compatibility break: release-0.1.0
`.wi` files must be recompiled, and release-0.1.0 readers reject the new
nonzero reserved flag and checksum fields. Release 0.2.0 therefore requires
applications to rebuild `.wi` files from their `.wf` sources.

Revision-4 generated headers in 0.2.0 also provide protocol-prefixed file
identity, message ordinals, field ordinals and types, field counts, and child
message bindings. Applications built with a specific protocol can use those
constants directly; runtime name lookup remains available for generic tools.
