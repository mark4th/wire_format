mod check;
mod output;
mod source;
mod wfb;

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

enum Action {
    Check { input: PathBuf },
    Compile { input: PathBuf, output: PathBuf },
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(code) => code,
    }
}

fn run() -> Result<(), ExitCode> {
    let mut arguments: Vec<String> = env::args().collect();
    let program = arguments
        .first()
        .cloned()
        .unwrap_or_else(|| "wfc".to_owned());
    if !arguments.is_empty() {
        arguments.remove(0);
    }

    if matches!(arguments.first().map(String::as_str), Some("-h" | "--help")) {
        print_usage(&program);
        return Ok(());
    }
    if matches!(
        arguments.first().map(String::as_str),
        Some("-V" | "--version")
    ) {
        println!("wfc {}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }

    let action = parse_action(&arguments).map_err(|message| {
        eprintln!("error: {message}");
        print_usage(&program);
        ExitCode::from(2)
    })?;

    match action {
        Action::Check { input } => check_file(&input),
        Action::Compile { input, output } => compile_file(&input, &output),
    }
}

fn load(input: &Path) -> Result<(source::Protocol, check::Summary), ExitCode> {
    let text = fs::read_to_string(input).map_err(|error| {
        eprintln!("{}: error: {error}", input.display());
        ExitCode::FAILURE
    })?;
    let protocol = source::parse(&text).map_err(|error| {
        print_diagnostic(input, &error);
        ExitCode::FAILURE
    })?;
    let summary = check::check(&protocol).map_err(|error| {
        print_diagnostic(input, &error);
        ExitCode::FAILURE
    })?;
    Ok((protocol, summary))
}

fn check_file(input: &Path) -> Result<(), ExitCode> {
    let (protocol, summary) = load(input)?;

    println!(
        "{}: OK: protocol `{}`, {} message{}, {} vector{}, {} layout octet{}",
        input.display(),
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

fn compile_file(input: &Path, output: &Path) -> Result<(), ExitCode> {
    let (protocol, summary) = load(input)?;
    let paths = output::OutputPaths::new(output).map_err(|error| {
        eprintln!("{}: error: {error}", output.display());
        ExitCode::FAILURE
    })?;
    let generated = wfb::generate(&protocol).map_err(|error| {
        print_diagnostic(input, &error);
        ExitCode::FAILURE
    })?;
    output::write_pair(&paths, &generated.header, &generated.binary).map_err(|error| {
        eprintln!(
            "{}: error writing generated files: {error}",
            output.display()
        );
        ExitCode::FAILURE
    })?;

    println!(
        "{}: compiled protocol `{}` ({} message{}, {} vector{}) -> {}, {}",
        input.display(),
        protocol.name,
        summary.messages,
        plural(summary.messages),
        summary.vectors,
        plural(summary.vectors),
        paths.header.display(),
        paths.binary.display()
    );
    Ok(())
}

fn parse_action(arguments: &[String]) -> Result<Action, String> {
    let Some(first) = arguments.first() else {
        return Err("expected a .wf.json5 file or the `check` command".to_owned());
    };

    if first == "check" {
        if arguments.len() != 2 {
            return Err("`check` requires exactly one .wf.json5 file".to_owned());
        }
        return Ok(Action::Check {
            input: PathBuf::from(&arguments[1]),
        });
    }
    if first.starts_with('-') {
        return Err(format!("unknown option `{first}`"));
    }

    let input = PathBuf::from(first);
    let mut output = None;
    let mut position = 1;
    while position < arguments.len() {
        match arguments[position].as_str() {
            "-o" | "--output" => {
                position += 1;
                let Some(value) = arguments.get(position) else {
                    return Err("`--output` requires a path stem".to_owned());
                };
                if output.replace(PathBuf::from(value)).is_some() {
                    return Err("`--output` may be specified only once".to_owned());
                }
            }
            option => return Err(format!("unknown option `{option}`")),
        }
        position += 1;
    }

    let output = output.ok_or_else(|| "compilation requires `--output path-stem`".to_owned())?;
    Ok(Action::Compile { input, output })
}

fn print_diagnostic(path: &Path, diagnostic: &source::Diagnostic) {
    eprintln!(
        "{}:{}:{}: error: {}",
        path.display(),
        diagnostic.location.line,
        diagnostic.location.column,
        diagnostic.message
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
    eprintln!("usage: {program} check protocol.wf.json5");
    eprintln!("       {program} protocol.wf.json5 --output path/stem");
}
