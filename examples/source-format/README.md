# Source-format example

`example-telemetry.wf` is a synthetic, protocol-neutral input used to explain
and test the human-readable source format. Both compiler implementations
consume it in their conformance tests.

```sh
make wfc
./wfc check examples/source-format/example-telemetry.wf
./wfc examples/source-format/example-telemetry.wf \
    --output target/examples/source-format/example_telemetry
```

The Rust compiler accepts the same source:

```sh
cargo run -p wfc -- check examples/source-format/example-telemetry.wf
```
