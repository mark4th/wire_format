#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$repo/target"
work=$(mktemp -d "$repo/target/wfc-conformance.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cd "$repo"
cargo build --quiet -p wfc

./wfc check examples/example-telemetry.wf.json5 >/dev/null
./wfc examples/example-telemetry.wf.json5 --output "$work/c/example_telemetry" >/dev/null
target/debug/wfc examples/example-telemetry.wf.json5 \
    --output "$work/rust/example_telemetry" >/dev/null

cmp "$work/c/example_telemetry.wfb" "$work/rust/example_telemetry.wfb"
cmp "$work/c/example_telemetry.h" "$work/rust/example_telemetry.h"

./wfc examples/example-telemetry.wf.json5 \
    --output "$work/c-repeat/example_telemetry" >/dev/null
cmp "$work/c/example_telemetry.wfb" "$work/c-repeat/example_telemetry.wfb"
cmp "$work/c/example_telemetry.h" "$work/c-repeat/example_telemetry.h"

./wfc tests/data/wfc-types.wf.json5 --output "$work/c/types" >/dev/null
target/debug/wfc tests/data/wfc-types.wf.json5 \
    --output "$work/rust/types" >/dev/null

cmp "$work/c/types.wfb" "$work/rust/types.wfb"
cmp "$work/c/types.h" "$work/rust/types.h"

./wfc tests/data/wfc-text.wf.json5 --output "$work/c/text" >/dev/null
target/debug/wfc tests/data/wfc-text.wf.json5 \
    --output "$work/rust/text" >/dev/null

cmp "$work/c/text.wfb" "$work/rust/text.wfb"
cmp "$work/c/text.h" "$work/rust/text.h"

sed 's/wire_format: 1,/wire_format: 1, surprise: 2,/' \
    examples/example-telemetry.wf.json5 >"$work/unknown-property.wf.json5"
if ./wfc check "$work/unknown-property.wf.json5" >/dev/null 2>&1; then
    echo "C wfc accepted an unknown property" >&2
    exit 1
fi
if target/debug/wfc check "$work/unknown-property.wf.json5" >/dev/null 2>&1; then
    echo "Rust wfc accepted an unknown property" >&2
    exit 1
fi

sed 's/wire: \[0x22/wire: [0xe2/' \
    examples/example-telemetry.wf.json5 >"$work/bad-vector.wf.json5"
if ./wfc check "$work/bad-vector.wf.json5" >/dev/null 2>&1; then
    echo "C wfc accepted a bad vector" >&2
    exit 1
fi
if target/debug/wfc check "$work/bad-vector.wf.json5" >/dev/null 2>&1; then
    echo "Rust wfc accepted a bad vector" >&2
    exit 1
fi

cc -std=c11 -Wall -Wextra -Werror \
    -I"$repo" -I"$work/c" \
    "$repo/tests/wfc_wfb_harness.c" \
    "$repo/tests/wfc_wfb_incbin.S" \
    "$repo/wire_format.c" \
    -o "$work/c/incbin-test"

(cd "$work/c" && ./incbin-test)
