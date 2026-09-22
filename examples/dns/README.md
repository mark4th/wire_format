# DNS example

This example builds DNS queries and decodes DNS responses with format programs
compiled from `dns.wf.json5`.

From the repository root:

```sh
make dns_demo
./target/examples/dns/dns_demo example.com
```

The build performs these operations:

1. Builds the C `wfc` compiler.
2. Checks and compiles `dns.wf.json5`.
3. Writes `dns_protocol.wfb` and `dns_protocol.h` under `target/examples/dns/`.
4. Embeds the database with `dns_protocol.S`.
5. Builds the demo against the generated header and embedded database.

The JSON5 source describes the fixed query header, question tail, response
header, and resource-record header. DNS names and resource data are
variable-length, so the version 1 fixed-record compiler does not pretend to
describe them. Small bounded C helpers advance over those portions.

`make test` also constructs a deterministic `example.com` query and compares
all 29 wire octets with an independently written expected image. It does not
need network access.
