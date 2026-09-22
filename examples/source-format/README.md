# Source-format example

`example-telemetry.wf.json5` is the small input used to explain and test the
human-readable source format.

```sh
make wfc
./wfc check examples/source-format/example-telemetry.wf.json5
./wfc examples/source-format/example-telemetry.wf.json5 \
    --output target/examples/source-format/example_telemetry
```

The Rust compiler accepts the same source:

```sh
cargo run -p wfc -- check examples/source-format/example-telemetry.wf.json5
```
