use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

const HEADER_SIZE: usize = 64;
const MESSAGE_RECORD_SIZE: usize = 32;

fn manifest_path(relative: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("the Rust crate must be inside the repository")
        .join(relative)
}

fn assert_success(output: &std::process::Output, operation: &str) {
    assert!(
        output.status.success(),
        "{operation} failed\nstdout: {}\nstderr: {}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn temporary_directory(label: &str) -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    manifest_path(&format!(
        "target/wfc-{label}-{}-{nonce}",
        std::process::id()
    ))
}

fn compile_wfb(source: &str, stem_name: &str) -> (PathBuf, PathBuf, Vec<u8>, Vec<u8>) {
    let directory = temporary_directory(stem_name);
    let stem = directory.join(stem_name);

    let generated = Command::new(env!("CARGO_BIN_EXE_wfc"))
        .arg(manifest_path(source))
        .arg("--output")
        .arg(&stem)
        .output()
        .expect("wfc must run");
    assert_success(&generated, "wfc compilation");

    let first_header = fs::read(stem.with_extension("h")).unwrap();
    let first_binary = fs::read(stem.with_extension("wfb")).unwrap();
    let regenerated = Command::new(env!("CARGO_BIN_EXE_wfc"))
        .arg(manifest_path(source))
        .arg("--output")
        .arg(&stem)
        .output()
        .expect("wfc must run again");
    assert_success(&regenerated, "repeat wfc compilation");
    assert_eq!(fs::read(stem.with_extension("h")).unwrap(), first_header);
    assert_eq!(fs::read(stem.with_extension("wfb")).unwrap(), first_binary);

    (directory, stem, first_header, first_binary)
}

fn u16_at(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap())
}

fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn string_at(bytes: &[u8], section: u32, slot: u32) -> &str {
    let relative = u32_at(bytes, section as usize + slot as usize * 4);
    assert_ne!(relative, u32::MAX);
    let string_table = u32_at(bytes, 52) as usize;
    let start = string_table + relative as usize;
    let length = bytes[start..].iter().position(|byte| *byte == 0).unwrap();
    std::str::from_utf8(&bytes[start..start + length]).unwrap()
}

fn assert_wfb_header(binary: &[u8]) {
    assert_eq!(&binary[..4], b"WFB\0");
    assert_eq!(u16_at(binary, 4), 1);
    assert_eq!(u16_at(binary, 6), HEADER_SIZE as u16);
    assert_eq!(u32_at(binary, 8) as usize, binary.len());
    assert_eq!(u32_at(binary, 20), MESSAGE_RECORD_SIZE as u32);

    let strings = u32_at(binary, 44) as usize;
    let string_count = u32_at(binary, 48) as usize;
    let string_table = u32_at(binary, 52) as usize;
    let string_table_size = u32_at(binary, 56) as usize;
    assert!(strings >= HEADER_SIZE);
    assert_eq!(strings + string_count * 4, string_table);
    assert_eq!(string_table + string_table_size, binary.len());
}

#[test]
fn checks_the_example_source_from_the_command_line() {
    let source = manifest_path("examples/example-telemetry.wf");
    let output = Command::new(env!("CARGO_BIN_EXE_wfc"))
        .arg("check")
        .arg(source)
        .output()
        .expect("wfc must run");

    assert_success(&output, "wfc check");
    let stdout = String::from_utf8(output.stdout).expect("wfc output must be UTF-8");
    assert!(stdout.contains("protocol `example-telemetry`"));
    assert!(stdout.contains("1 message, 1 vector, 3 layout octets"));
}

#[test]
fn compilation_requires_an_output_stem() {
    let output = Command::new(env!("CARGO_BIN_EXE_wfc"))
        .arg("example.wf")
        .output()
        .expect("wfc must run");

    assert_eq!(output.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&output.stderr).contains("requires `--output path-stem`"));
}

#[test]
fn language_backends_are_not_part_of_the_interface() {
    let output = Command::new(env!("CARGO_BIN_EXE_wfc"))
        .args(["example.wf", "--output", "example", "--language", "c"])
        .output()
        .expect("wfc must run");

    assert_eq!(output.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&output.stderr).contains("unknown option `--language`"));
}

#[test]
fn compiles_loadable_and_incbin_compatible_wfb() {
    let (directory, _stem, header, binary) =
        compile_wfb("examples/example-telemetry.wf", "example_telemetry");
    assert_wfb_header(&binary);
    assert_eq!(u32_at(&binary, 16), 1);

    let protocol_strings = u32_at(&binary, 36);
    assert_eq!(u32_at(&binary, 40), 4);
    assert_eq!(string_at(&binary, protocol_strings, 0), "example-telemetry");

    let message_table = u32_at(&binary, 24) as usize;
    let message_strings = u32_at(&binary, message_table);
    assert_eq!(string_at(&binary, message_strings, 0), "status");
    assert_eq!(string_at(&binary, message_strings, 4), "version");
    assert!(string_at(&binary, message_strings, 2).contains("%p{1}"));
    assert!(String::from_utf8(header)
        .unwrap()
        .contains("EXAMPLE_TELEMETRY_STATUS_STRINGS_OFFSET"));

    let executable = directory.join("incbin_test");
    let compiler = std::env::var_os("CC").unwrap_or_else(|| "cc".into());
    let compiled = Command::new(compiler)
        .current_dir(&directory)
        .args(["-std=c11", "-Wall", "-Wextra", "-Werror"])
        .arg("-I")
        .arg(manifest_path("."))
        .arg("-I")
        .arg(&directory)
        .arg(manifest_path("tests/wfc_wfb_harness.c"))
        .arg(manifest_path("tests/wfc_wfb_incbin.S"))
        .arg(manifest_path("wire_format.c"))
        .arg("-o")
        .arg(&executable)
        .output()
        .expect("the C compiler must run");
    assert_success(&compiled, "compiling the incbin application");

    let ran = Command::new(&executable)
        .output()
        .expect("the incbin application must run");
    assert_success(&ran, "using an incbin WFB");
    fs::remove_dir_all(directory).unwrap();
}

#[test]
fn compiles_crossing_bits_little_endian_and_u64_metadata() {
    let (directory, _stem, _header, binary) =
        compile_wfb("tests/data/wfc-types.wf", "compiled_types");
    assert_wfb_header(&binary);
    assert_eq!(u32_at(&binary, 12), 3);
    assert_eq!(u32_at(&binary, 16), 3);
    let protocol_strings = u32_at(&binary, 36) as usize;
    assert_eq!(u32_at(&binary, protocol_strings + 8), u32::MAX);
    assert_eq!(u32_at(&binary, protocol_strings + 12), u32::MAX);

    let message_table = u32_at(&binary, 24) as usize;
    let crossing = message_table;
    assert_eq!(u32_at(&binary, crossing + 8), 14);
    assert_eq!(u32_at(&binary, crossing + 12), 4);
    let widths = u32_at(&binary, crossing + 16) as usize;
    let swaps = u32_at(&binary, crossing + 20) as usize;
    assert_eq!(&binary[widths..widths + 4], &[5, 9, 32, 64]);
    assert_eq!(&binary[swaps..swaps + 4], &[0, 0, 4, 8]);

    let strings = u32_at(&binary, crossing);
    assert_eq!(string_at(&binary, strings, 0), "crossing");
    assert_eq!(string_at(&binary, strings, 7), "count");
    fs::remove_dir_all(directory).unwrap();
}
