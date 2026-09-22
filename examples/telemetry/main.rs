use std::fmt::Write;

use wire_format::{Param, WireFormat};

// This example is deliberately over-commented because its job is to explain
// the format language, not merely exercise it.
//
// Format-string mental model:
//
//   - The parser has a small integer/raw stack.
//   - The rightmost value in these comments is TOS, the top of stack.
//   - %pN pushes caller parameter N.
//   - %{n} pushes literal n.
//   - %b, %w, and %W pop one value and emit 1, 2, or 4 bytes.
//   - %B, %S, and %L read 1, 2, or 4 bytes and push the decoded value.
//   - %Pa pops a value and stores it in variable a.
//   - %x pops pos, width, value and packs value into the bit accumulator.
//   - %X pops pos, width and pushes bits extracted from the bit accumulator.
//   - %f flushes or releases the bit accumulator byte.
//   - %r pops length, then a raw-buffer parameter, and emits that many bytes.
//
// For bit-field encoding, the common idiom is:
//
//   %pN%{width}%{pos}%x
//   [] -> [value,width,pos] -> []
//
// For bit-field decoding, the common idiom is:
//
//   %{width}%{pos}%X%Pa
//   [] -> [width,pos] -> [value] -> [] and variable a receives value.

// TELEMETRY_FRAME encodes this frame:
//
//   byte 0      +-------------------------------+
//               | sync = 0xA5                   |
//               +-------------------------------+
//
//   byte 1        bit 7..4          bit 3..0
//               +-----------------+-----------------+
//               | version         | message type    |
//               +-----------------+-----------------+
//
//   bytes 2..3  +-----------------+-----------------+
//               | sequence[15:8]  | sequence[7:0]   |
//               +-----------------+-----------------+
//
//   bytes 4..7  +--------------+--------------+--------------+--------------+
//               | uptime[31:24]| uptime[23:16]| uptime[15:8] | uptime[7:0]  |
//               +--------------+--------------+--------------+--------------+
//
//   bytes 8..   +-----------------------------------+
//               | payload bytes                     |
//               +-----------------------------------+
//
//   last 4      +--------------+--------------+--------------+--------------+
//               | crc[31:24]   | crc[23:16]   | crc[15:8]    | crc[7:0]     |
//               +--------------+--------------+--------------+--------------+
//
// Parameter map:
//
//   p1 protocol version
//   p2 message type
//   p3 sequence number
//   p4 uptime
//   p5 raw payload buffer
//   p6 raw payload length
//   p7 frame CRC placeholder
//
// Parser walk:
//
//   Fragment                  Stack / parser state                         Emits
//   ------------------------  -------------------------------------------  -------------
//   %{165} %b                 [] -> [165] -> []                            sync byte
//   %p1 %{4} %{4} %x          [] -> [version,4,4] -> []                    byte 1 high nibble
//   %p2 %{4} %{0} %x          [] -> [msg_type,4,0] -> []                   byte 1 low nibble
//   %f                        flush bit accumulator                        byte 1
//   %p3 %w                    [] -> [sequence] -> []                       bytes 2..3
//   %p4 %W                    [] -> [uptime] -> []                         bytes 4..7
//   %p5 %p6 %r                [] -> [raw_payload,len] -> []                payload bytes
//   %p7 %W                    [] -> [crc] -> []                            final 4 bytes

const TELEMETRY_FRAME: &str = concat!(
    "%{165}%b",      // sync byte
    "%p1%{4}%{4}%x", // version in high nibble
    "%p2%{4}%{0}%x", // message type in low nibble
    "%f",            // flush packed version/type byte
    "%p3%w",         // sequence number, uint16 big-endian
    "%p4%W",         // uptime, uint32 big-endian
    "%p5%p6%r",      // payload bytes by raw parameter + length
    "%p7%W",         // frame CRC placeholder, uint32 big-endian
);

// TELEMETRY_PREFIX_DECODE decodes only the fixed prefix of TELEMETRY_FRAME.
// It intentionally stops before payload and CRC.
//
// Variable map after parsing:
//
//   a sync byte
//   b protocol version
//   c message type
//   d sequence number
//   e uptime
//
// Parser walk:
//
//   Fragment                  Stack / variable effect                      Reads
//   ------------------------  -------------------------------------------  -------------
//   %B %Pa                    [] -> [sync] -> var a                        byte 0
//   %{4} %{4} %X %Pb          [] -> [4,4] -> [version] -> var b            byte 1 high nibble
//   %{4} %{0} %X %Pc          [] -> [4,0] -> [msg_type] -> var c           byte 1 low nibble
//   %f                        release bit accumulator byte                 byte 1 complete
//   %S %Pd                    [] -> [sequence] -> var d                    bytes 2..3
//   %L %Pe                    [] -> [uptime] -> var e                      bytes 4..7

const TELEMETRY_PREFIX_DECODE: &str = concat!(
    "%B%Pa",         // sync byte -> a
    "%{4}%{4}%X%Pb", // version -> b
    "%{4}%{0}%X%Pc", // message type -> c
    "%f",
    "%S%Pd", // sequence -> d
    "%L%Pe", // uptime -> e
);

// STATUS_REPLY_DECODE shows a second, simpler receive-only message shape:
//
//   byte 0      +-------------------------------+
//               | status code                   |
//               +-------------------------------+
//
//   bytes 1..2  +-----------------+-----------------+
//               | seq[15:8]       | seq[7:0]        |
//               +-----------------+-----------------+
//
//   bytes 3..6  +--------------+--------------+--------------+--------------+
//               | time[31:24]  | time[23:16]  | time[15:8]   | time[7:0]    |
//               +--------------+--------------+--------------+--------------+
//
// Variable map after parsing:
//
//   a status code
//   b accepted sequence
//   c device timestamp
//
// Parser walk:
//
//   Fragment                  Stack / variable effect                      Reads
//   ------------------------  -------------------------------------------  -------------
//   %B %Pa                    [] -> [status] -> var a                      byte 0
//   %S %Pb                    [] -> [accepted_seq] -> var b                bytes 1..2
//   %L %Pc                    [] -> [timestamp] -> var c                   bytes 3..6

const STATUS_REPLY_DECODE: &str = concat!(
    "%B%Pa", // status code -> a
    "%S%Pb", // accepted sequence -> b
    "%L%Pc", // device timestamp -> c
);

fn hex(bytes: &[u8]) -> String {
    let mut out = String::new();
    for (index, byte) in bytes.iter().enumerate() {
        if index != 0 {
            out.push(' ');
        }
        write!(&mut out, "{byte:02X}").unwrap();
    }
    out
}

fn main() -> Result<(), wire_format::Error> {
    let payload = *b"rust-port-demo";
    let params = [
        Param::from(1u8),            // protocol version
        Param::from(2u8),            // message type
        Param::from(0x1234u16),      // sequence
        Param::from(0x0102_0304u32), // uptime
        Param::Raw(&payload),        // raw payload handle
        Param::from(payload.len()),  // raw payload length
        Param::from(0xDEAD_BEEFu32), // placeholder CRC
    ];

    let mut frame = [0u8; 64];
    let mut enc = WireFormat::new_encode(&mut frame, &params);
    enc.parse(TELEMETRY_FRAME)?;

    println!("telemetry frame format:");
    println!("  {TELEMETRY_FRAME}");
    println!("encoded frame ({} bytes):", enc.output_len());
    println!("  {}", hex(enc.output()));

    let mut prefix = WireFormat::new_decode(enc.output(), &[]);
    prefix.parse(TELEMETRY_PREFIX_DECODE)?;
    println!("decoded prefix:");
    println!(
        "  sync=0x{:02X} version={} type={} seq=0x{:04X} uptime=0x{:08X}",
        prefix.int_var(b'a').unwrap(),
        prefix.int_var(b'b').unwrap(),
        prefix.int_var(b'c').unwrap(),
        prefix.int_var(b'd').unwrap(),
        prefix.int_var(b'e').unwrap(),
    );

    let status_reply = [0x00, 0x12, 0x34, 0xCA, 0xFE, 0xBA, 0xBE];
    let mut dec = WireFormat::new_decode(&status_reply, &[]);
    dec.parse(STATUS_REPLY_DECODE)?;
    println!("decoded status reply:");
    println!(
        "  status={} accepted_seq=0x{:04X} timestamp=0x{:08X}",
        dec.int_var(b'a').unwrap(),
        dec.int_var(b'b').unwrap(),
        dec.int_var(b'c').unwrap(),
    );

    Ok(())
}
