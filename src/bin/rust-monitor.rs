use std::{env, path::PathBuf, process::ExitCode};

fn main() -> ExitCode {
    let mut args = env::args_os().skip(1);
    let path = match args.next() {
        None => None,
        Some(flag) if flag == "--config" => match args.next() {
            Some(path) => Some(PathBuf::from(path)),
            None => return usage(),
        },
        _ => return usage(),
    };
    if args.next().is_some() {
        return usage();
    }
    let snapshot = xfce_rust_monitor::collect(path.as_deref());
    println!("{}\n{}", snapshot.text, snapshot.tooltip);
    ExitCode::SUCCESS
}

fn usage() -> ExitCode {
    eprintln!("Usage: rust-monitor [--config PATH]");
    ExitCode::from(2)
}
