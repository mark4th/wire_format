mod check;
mod lexer;
mod source;

use std::env;
use std::fs;
use std::process::ExitCode;

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(code) => code,
    }
}

fn run() -> Result<(), ExitCode> {
    let mut arguments = env::args();
    let program = arguments.next().unwrap_or_else(|| "wfc".to_owned());
    let command = arguments.next();

    if matches!(command.as_deref(), Some("-h" | "--help")) {
        print_usage(&program);
        return Ok(());
    }
    if matches!(command.as_deref(), Some("-V" | "--version")) {
        println!("wfc {}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }
    if command.as_deref() != Some("check") {
        eprintln!("error: expected the `check` command");
        print_usage(&program);
        return Err(ExitCode::from(2));
    }

    let Some(path) = arguments.next() else {
        eprintln!("error: `check` requires one .wf file");
        print_usage(&program);
        return Err(ExitCode::from(2));
    };
    if arguments.next().is_some() {
        eprintln!("error: `check` accepts exactly one .wf file");
        print_usage(&program);
        return Err(ExitCode::from(2));
    }

    let text = fs::read_to_string(&path).map_err(|error| {
        eprintln!("{path}: error: {error}");
        ExitCode::FAILURE
    })?;
    let protocol = source::parse(&text).map_err(|error| {
        print_diagnostic(&path, &error);
        ExitCode::FAILURE
    })?;
    let summary = check::check(&protocol).map_err(|error| {
        print_diagnostic(&path, &error);
        ExitCode::FAILURE
    })?;

    println!(
        "{path}: OK: protocol `{}`, {} message{}, {} vector{}, {} layout octet{}",
        protocol.name,
        summary.messages,
        plural(summary.messages),
        summary.vectors,
        plural(summary.vectors),
        summary.layout_octets,
        plural(summary.layout_octets)
    );
    Ok(())
}

fn print_diagnostic(path: &str, diagnostic: &source::Diagnostic) {
    eprintln!(
        "{path}:{}:{}: error: {}",
        diagnostic.location.line, diagnostic.location.column, diagnostic.message
    );
}

fn plural(count: usize) -> &'static str {
    if count == 1 {
        ""
    } else {
        "s"
    }
}

fn print_usage(program: &str) {
    eprintln!("usage: {program} check protocol.wf");
}
