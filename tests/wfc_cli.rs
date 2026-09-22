use std::path::Path;
use std::process::Command;

#[test]
fn checks_the_example_source_from_the_command_line() {
    let source = Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/example-telemetry.wf");
    let output = Command::new(env!("CARGO_BIN_EXE_wfc"))
        .arg("check")
        .arg(source)
        .output()
        .expect("wfc must run");

    assert!(
        output.status.success(),
        "wfc failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let stdout = String::from_utf8(output.stdout).expect("wfc output must be UTF-8");
    assert!(stdout.contains("protocol `example-telemetry`"));
    assert!(stdout.contains("1 message, 1 vector, 3 layout octets"));
}

#[test]
fn rejects_an_unknown_command() {
    let output = Command::new(env!("CARGO_BIN_EXE_wfc"))
        .arg("unknown")
        .output()
        .expect("wfc must run");

    assert_eq!(output.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&output.stderr).contains("expected the `check` command"));
}
