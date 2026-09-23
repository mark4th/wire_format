#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$repo/target"
work=$(mktemp -d "$repo/target/wfc-conformance.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cd "$repo"
cargo build --quiet -p wfc

./wfc check examples/source-format/example-telemetry.wf >/dev/null
./wfc examples/source-format/example-telemetry.wf --output "$work/c/example_telemetry" >/dev/null
target/debug/wfc examples/source-format/example-telemetry.wf \
    --output "$work/rust/example_telemetry" >/dev/null

cmp "$work/c/example_telemetry.wi" "$work/rust/example_telemetry.wi"
cmp "$work/c/example_telemetry.h" "$work/rust/example_telemetry.h"

./wfc examples/source-format/example-telemetry.wf \
    --output "$work/c-repeat/example_telemetry" >/dev/null
cmp "$work/c/example_telemetry.wi" "$work/c-repeat/example_telemetry.wi"
cmp "$work/c/example_telemetry.h" "$work/c-repeat/example_telemetry.h"

./wfc tests/data/wfc-types.wf --output "$work/c/types" >/dev/null
target/debug/wfc tests/data/wfc-types.wf \
    --output "$work/rust/types" >/dev/null

cmp "$work/c/types.wi" "$work/rust/types.wi"
cmp "$work/c/types.h" "$work/rust/types.h"

./wfc tests/data/wfc-text.wf --output "$work/c/text" >/dev/null
target/debug/wfc tests/data/wfc-text.wf \
    --output "$work/rust/text" >/dev/null

cmp "$work/c/text.wi" "$work/rust/text.wi"
cmp "$work/c/text.h" "$work/rust/text.h"

./wfc examples/dns/dns.wf --output "$work/c/dns" >/dev/null
target/debug/wfc examples/dns/dns.wf --output "$work/rust/dns" >/dev/null

cmp "$work/c/dns.wi" "$work/rust/dns.wi"
cmp "$work/c/dns.h" "$work/rust/dns.h"

./wfc tests/data/wfc-v2-bytes.wf --output "$work/c/v2" >/dev/null
target/debug/wfc tests/data/wfc-v2-bytes.wf --output "$work/rust/v2" >/dev/null

cmp "$work/c/v2.wi" "$work/rust/v2.wi"
cmp "$work/c/v2.h" "$work/rust/v2.h"

sed 's/wire_format: 2/wire_format: 1/' \
    tests/data/wfc-v2-bytes.wf >"$work/v1-bytes.wf"
if ./wfc check "$work/v1-bytes.wf" >/dev/null 2>&1; then
    echo "C wfc accepted a version-2 byte field in version 1" >&2
    exit 1
fi
if target/debug/wfc check "$work/v1-bytes.wf" >/dev/null 2>&1; then
    echo "Rust wfc accepted a version-2 byte field in version 1" >&2
    exit 1
fi

sed 's/wire_format: 1,/wire_format: 1, surprise: 2,/' \
    examples/source-format/example-telemetry.wf >"$work/unknown-property.wf"
if ./wfc check "$work/unknown-property.wf" >/dev/null 2>&1; then
    echo "C wfc accepted an unknown property" >&2
    exit 1
fi
if target/debug/wfc check "$work/unknown-property.wf" >/dev/null 2>&1; then
    echo "Rust wfc accepted an unknown property" >&2
    exit 1
fi

sed 's/wire: \[0x22/wire: [0xe2/' \
    examples/source-format/example-telemetry.wf >"$work/bad-vector.wf"
if ./wfc check "$work/bad-vector.wf" >/dev/null 2>&1; then
    echo "C wfc accepted a bad vector" >&2
    exit 1
fi
if target/debug/wfc check "$work/bad-vector.wf" >/dev/null 2>&1; then
    echo "Rust wfc accepted a bad vector" >&2
    exit 1
fi

cc -std=c11 -Wall -Wextra -Werror \
    -I"$repo" -I"$work/c" \
    "$repo/tests/wfc_wi_harness.c" \
    "$repo/tests/wfc_wi_incbin.S" \
    "$repo/wire_format.c" \
    -o "$work/c/incbin-test"

(cd "$work/c" && ./incbin-test)

cc -std=c11 -Wall -Wextra -Werror \
    -I"$repo" -I"$work/c" \
    "$repo/tests/wfc_wi_v2_harness.c" \
    "$repo/tests/wfc_wi_v2_incbin.S" \
    "$repo/wire_format.c" \
    -o "$work/c/v2-incbin-test"

(cd "$work/c" && ./v2-incbin-test)
