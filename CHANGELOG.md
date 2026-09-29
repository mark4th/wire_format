# Release history

Release tags use `vMAJOR.MINOR.PATCH` and identify the library/tool release,
independently of `.wf`/`.wi` revisions. See the
[versioning guide](doc/VERSIONING.md) for compatibility policy and the feature
implementation history.

## 0.1.0 — 2026-09-29

First annotated release tag: [`v0.1.0`](https://github.com/mark4th/wire_format/tree/v0.1.0).
This establishes a release baseline for the existing implementation; the
features below were developed before the tag was created.

- Shared format-string interpreter for encoding and decoding, with an RPN
  stack, calculations, bit fields, conditionals and 64-bit values.
- Shared subformat calls, nested repetition and scalar arrays.
- C and Rust `.wf` compilers for source revisions 1–3: fixed records,
  variable byte tails, SDNVs, bounded slices, counted records and choices.
- C source revision 4: named records, expressions, conditions, nested records,
  checksums and additional encodings, compiled into runtime format strings.
- Reusable static/shared `wire_info` libraries and a named-record adapter.
- DNS and telemetry examples, format specifications and compatibility policy.

Known support boundaries: Rust supports revisions 1–3; named-record revision
4 is C-only. `wire_info_open()` accepts revision-4 images; earlier formats use
their existing loading and caller-data interfaces. No documented features are
deprecated in this release.

Validation: `make -j4 test` and `cargo test --workspace`, including C/Rust
compiler conformance for revisions 1–3 and C named-record checks.
