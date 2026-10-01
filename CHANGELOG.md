# Release history

Release tags use `vMAJOR.MINOR.PATCH` and identify the library/tool release,
independently of `.wf`/`.wi` revisions. See the
[versioning guide](doc/VERSIONING.md) for compatibility policy and the feature
implementation history.

## 0.2.0 — 2026-10-01

- Added a mandatory CRC-32C to every generated `.wi`. The checksum occupies
  header offset 60, covers the complete file with that field treated as zero,
  and is advertised by header flag bit 31.
- `wire_info_open()` now rejects a damaged version-4 image with
  `WIRE_INFO_CHECKSUM` before exposing catalog contents.
- Version-4 generated headers now provide protocol-prefixed file identity,
  message ordinals, field ordinals/types/counts and child-message bindings.
- This is a deliberate pre-1.0 compiled-artifact compatibility break. Files
  produced by release 0.1.0 must be recompiled from their `.wf` sources.

## 0.1.0 — 2026-09-29

First annotated release tag: [`v0.1.0`](https://github.com/mark4th/wire_format/tree/v0.1.0).
This establishes a release baseline for the existing implementation; the
features below were developed before the tag was created.

- Shared format-string interpreter for encoding and decoding, with an RPN
  stack, calculations, bit fields, conditionals and 64-bit values.
- Shared subformat calls, nested repetition and scalar arrays.
- `wfc` implementations written in C and Rust for source revisions 1–3: fixed records,
  variable byte tails, SDNVs, bounded slices, counted records and choices.
- Native `wfc` support for source revision 4: named records, expressions, conditions, nested records,
  checksums and additional encodings, compiled into runtime format strings.
- Reusable static/shared `wire_info` libraries and a named-record adapter.
- DNS and telemetry examples, format specifications and compatibility policy.

Known support boundaries: Rust supports revisions 1–3; named-record revision
4 is C-only. `wire_info_open()` accepts revision-4 images; earlier formats use
their existing loading and caller-data interfaces. No documented features are
deprecated in this release.

Validation: `make -j4 test` and `cargo test --workspace`, including C/Rust
compiler conformance for revisions 1–3 and C named-record checks.
