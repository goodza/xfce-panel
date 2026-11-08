use std::process::Command;

#[test]
fn cli_one_shot_and_usage() {
    let result = Command::new(env!("CARGO_BIN_EXE_rust-monitor"))
        .args(["--config", "/nonexistent/rust-monitor-test.toml"])
        .output()
        .unwrap();
    assert!(result.status.success());
    let text = String::from_utf8(result.stdout).unwrap();
    assert!(text.lines().count() >= 2);
    assert!(text.contains("RAM"));
    for args in [
        vec!["--config"],
        vec!["--unknown"],
        vec!["--config", "x", "extra"],
    ] {
        assert_eq!(
            Command::new(env!("CARGO_BIN_EXE_rust-monitor"))
                .args(args)
                .output()
                .unwrap()
                .status
                .code(),
            Some(2)
        );
    }
}

#[test]
fn executable_example_returns_plain_text() {
    let result = Command::new(concat!(env!("CARGO_MANIFEST_DIR"), "/examples/metric.sh"))
        .output()
        .unwrap();
    assert!(result.status.success());
    assert_eq!(
        String::from_utf8(result.stdout).unwrap(),
        "OK\nExample trusted local extension\n"
    );
}
