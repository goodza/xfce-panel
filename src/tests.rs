use super::*;
use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};

static NEXT: AtomicU64 = AtomicU64::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let p = std::env::temp_dir().join(format!(
            "rust-monitor-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&p).unwrap();
        Self(p)
    }
    fn write(&self, path: &str, text: &str) -> PathBuf {
        let path = self.0.join(path);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, text).unwrap();
        path
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn script(text: &str, timeout_ms: u64) -> config::Extension {
    config::Extension {
        name: "test".into(),
        command: vec!["/bin/sh".into(), "-c".into(), text.into()],
        timeout_ms,
    }
}

#[test]
fn config_defaults_missing_and_invalid() {
    let f = Fixture::new();
    let (c, e) = config::load(Some(&f.0.join("missing")));
    assert!(e.is_none());
    assert_eq!(c.interval_seconds, 2);
    assert_eq!(c.disks, vec![PathBuf::from("/")]);
    for text in [
        "invalid = [",
        "interval_seconds = 0",
        "interval_seconds = 3601",
        "unknown = true",
        "[[extensions]]\nname='bad'\ncommand=[]",
        "[[extensions]]\nname='bad'\ncommand=['true']\ntimeout_ms=9",
        "[[extensions]]\nname='bad'\ncommand=['true']\ntimeout_ms=10001",
    ] {
        let p = f.write("config", text);
        let (c, e) = config::load(Some(&p));
        assert_eq!(c.interval_seconds, 2);
        assert!(e.unwrap().contains("using defaults"));
    }
    let p = f.write(
        "config",
        &"[[extensions]]\nname='x'\ncommand=['true']\n".repeat(17),
    );
    assert!(config::load(Some(&p)).1.unwrap().contains("16"));
}

#[test]
fn config_optional_sensors_and_limits() {
    let f = Fixture::new();
    let p = f.write("config", "interval_seconds=3600\ndisks=[]\nfan_sensors=[]\n[[extensions]]\nname='x'\ncommand=['true']");
    let (c, e) = config::load(Some(&p));
    assert!(e.is_none());
    assert_eq!(c.interval_seconds, 3600);
    assert_eq!(c.fan_sensors, Some(vec![]));
    assert_eq!(c.extensions[0].timeout_ms, 1000);
    let p = f.write(
        "config",
        "interval_seconds=1\n[[extensions]]\nname='x'\ncommand=['true']\ntimeout_ms=10",
    );
    assert!(config::load(Some(&p)).1.is_none());
}

#[test]
fn meminfo_parsing() {
    assert_eq!(
        sensors::mem_available("MemTotal: 999 kB\nMemAvailable: 1048576 kB\n").unwrap(),
        1073741824
    );
    for text in [
        "",
        "MemAvailable: -1 kB",
        "MemAvailable: 1 MB",
        "MemAvailable: 18446744073709551615 kB",
    ] {
        assert!(sensors::mem_available(text).is_err());
    }
}

#[test]
fn hwmon_discovery_is_cpu_specific_and_sorted() {
    let f = Fixture::new();
    f.write("hwmon/hwmon9/name", "coretemp\n");
    let hot = f.write("hwmon/hwmon9/temp2_input", "70000\n");
    let cool = f.write("hwmon/hwmon9/temp1_input", "50000\n");
    f.write("hwmon/hwmon0/name", "nvme\n");
    f.write("hwmon/hwmon0/temp1_input", "99000\n");
    let fan = f.write("hwmon/hwmon0/fan1_input", "1200\n");
    f.write("hwmon/hwmon9/tempbad_input", "1");
    let (temps, fans) = sensors::discover(&f.0.join("hwmon"));
    assert_eq!(temps, vec![cool, hot]);
    assert_eq!(fans, vec![fan]);
    let cfg = f.write("config", "disks=[]");
    let mem = f.write("meminfo", "MemAvailable: 1048576 kB\n");
    let s = collect_from(Some(&cfg), &f.0.join("hwmon"), &mem);
    assert!(s.text.contains("70.0 °C"));
    assert!(!s.text.contains("CPU"));
    assert!(!s.text.contains("Fan 1"));
    assert!(!s.text.contains("RAM"));
    assert!(!s.text.contains("available"));
    assert!(s.tooltip.contains("CPU 70.0 °C"));
    assert!(s.tooltip.contains("RAM 1.00 GiB available"));
    assert!(!s.text.contains("99.0"));
    assert!(s.text.contains("1200 RPM"));
    assert!(s.text.contains("1.00 GiB"));
}

#[test]
fn compact_panel_hides_second_fan_but_preserves_tooltip() {
    let f = Fixture::new();
    f.write("hwmon/hwmon0/name", "coretemp\n");
    f.write("hwmon/hwmon0/temp1_input", "54000\n");
    f.write("hwmon/hwmon0/fan1_input", "1240\n");
    f.write("hwmon/hwmon0/fan2_input", "9876\n");
    let cfg = f.write("config", "disks=[]");
    let mem = f.write("meminfo", "MemAvailable: 1048576 kB\n");
    let s = collect_from(Some(&cfg), &f.0.join("hwmon"), &mem);
    assert_eq!(s.text, "54.0 °C · 1240 RPM · 1.00 GiB");
    assert!(s.tooltip.contains("CPU 54.0 °C"));
    assert!(s.tooltip.contains("Fan 1 ("));
    assert!(s.tooltip.contains("Fan 2 ("));
    assert!(s.tooltip.contains("9876 RPM"));
    assert!(s.tooltip.contains("RAM 1.00 GiB available"));

    f.write("hwmon/hwmon0/fan1_input", "invalid");
    let s = collect_from(Some(&cfg), &f.0.join("hwmon"), &mem);
    assert!(!s.text.contains("RPM"));
    assert!(s.tooltip.contains("9876 RPM"));

    f.write("hwmon/hwmon0/fan1_input", "0");
    f.write("hwmon/hwmon0/fan2_input", "invalid");
    f.write("hwmon/hwmon0/fan3_input", "2222");
    let s = collect_from(Some(&cfg), &f.0.join("hwmon"), &mem);
    assert_eq!(s.text, "54.0 °C · 0 RPM · Fan 3 2222 RPM · 1.00 GiB");
}

#[test]
fn cpu_warning_uses_raw_threshold_and_resets_when_unavailable() {
    let f = Fixture::new();
    let sensor = f.write("sensor", "80000");
    let cfg = f.write(
        "config",
        &format!("disks=[]\nfan_sensors=[]\ncpu_sensor={:?}", sensor),
    );
    let cpath = CString::new(cfg.to_str().unwrap()).unwrap();
    for (reading, hot) in [
        (79_999, false),
        (80_000, false),
        (80_001, true),
        (81_000, true),
        (70_000, false),
    ] {
        f.write("sensor", &reading.to_string());
        assert_eq!(collect(Some(&cfg)).cpu_hot, hot, "{reading}");
        // SAFETY: cpath is live; each returned snapshot is released unchanged once.
        unsafe {
            let snapshot = monitor_collect(cpath.as_ptr());
            assert!(!snapshot.is_null());
            assert_eq!((*snapshot).cpu_hot, u32::from(hot));
            monitor_snapshot_free(snapshot);
        }
    }
    f.write("sensor", "invalid");
    assert!(!collect(Some(&cfg)).cpu_hot);
}

#[test]
fn recognized_drivers_and_explicit_overrides() {
    let f = Fixture::new();
    for (i, name) in ["coretemp", "k10temp", "zenpower", "cpu_thermal", "nvme"]
        .iter()
        .enumerate()
    {
        f.write(&format!("hwmon/hwmon{i}/name"), name);
        f.write(&format!("hwmon/hwmon{i}/temp1_input"), "40000");
    }
    assert_eq!(sensors::discover(&f.0.join("hwmon")).0.len(), 4);
    let sensor = f.write("explicit", "88000");
    let cfg = f.write(
        "config",
        &format!(
            "disks=[]\nfan_sensors=[]\ncpu_sensor={:?}",
            sensor.to_str().unwrap()
        ),
    );
    let s = collect_from(Some(&cfg), &f.0.join("hwmon"), &f.0.join("missing"));
    assert_eq!(s.text, "88.0 °C");
}

#[test]
fn no_metrics_fallback_diagnostics_and_reload() {
    let f = Fixture::new();
    let cfg = f.write("config", "interval_seconds=3\ndisks=[]\nfan_sensors=[]");
    let s = collect_from(Some(&cfg), &f.0.join("absent"), &f.0.join("absent"));
    assert_eq!(s.text, "Monitor");
    assert_eq!(s.interval_seconds, 3);
    assert!(s.tooltip.contains("CPU temperature unavailable"));
    assert!(s.tooltip.contains("Memory unavailable"));
    f.write("config", "interval_seconds=4\ndisks=[]");
    assert_eq!(collect(Some(&cfg)).interval_seconds, 4);
    f.write("config", "broken[");
    let s = collect(Some(&cfg));
    assert_eq!(s.interval_seconds, 2);
    assert!(s.tooltip.contains("using defaults"));
}

#[test]
fn disk_errors_and_live_root() {
    let f = Fixture::new();
    assert!(sensors::disk(&f.0.join("missing"))
        .err()
        .unwrap()
        .contains("missing"));
    let root = sensors::disk(Path::new("/")).unwrap();
    assert!((0.0..=100.0).contains(&root.used_percent));
    assert!(root.available_bytes >= 0.0);
    use std::os::unix::ffi::OsStrExt;
    assert!(sensors::disk(Path::new(std::ffi::OsStr::from_bytes(b"a\0b"))).is_err());
}

#[test]
fn extension_success_and_arguments_without_shell() {
    let (value, detail) =
        extension::run(&script("printf '42%%\ndetails\nignored\n'", 1000)).unwrap();
    assert_eq!(value, "42%");
    assert_eq!(detail.as_deref(), Some("details"));
    let ext = config::Extension {
        name: "x".into(),
        command: vec![
            "/usr/bin/printf".into(),
            "%s\\n".into(),
            "$(false); hi".into(),
        ],
        timeout_ms: 1000,
    };
    assert_eq!(extension::run(&ext).unwrap().0, "$(false); hi");
}

#[test]
fn extension_failures_and_invalid_text() {
    let err = extension::run(&script("printf 'specific error' >&2; exit 7", 1000)).unwrap_err();
    assert!(err.contains("7"));
    assert!(err.contains("specific error"));
    for command in [
        "true",
        "printf '\\000'",
        "printf '\\001'",
        "printf '\\377'",
        "printf 'ok\\r\\n'",
        "printf ok; printf '\\001' >&2",
    ] {
        assert!(extension::run(&script(command, 1000)).is_err(), "{command}");
    }
    assert!(extension::run(&config::Extension {
        name: "x".into(),
        command: vec!["/no/such/executable".into()],
        timeout_ms: 1000
    })
    .unwrap_err()
    .contains("cannot start"));
}

#[test]
fn extension_timeout_and_output_limits() {
    let start = Instant::now();
    assert!(extension::run(&script("sleep 30", 40))
        .unwrap_err()
        .contains("timed out"));
    assert!(start.elapsed() < Duration::from_secs(2));
    for command in [
        "yes x",
        "yes x >&2",
        "head -c 4097 /dev/zero",
        "head -c 3000 /dev/zero; head -c 2000 /dev/zero >&2",
    ] {
        let start = Instant::now();
        assert!(extension::run(&script(command, 1000))
            .unwrap_err()
            .contains("4096"));
        assert!(start.elapsed() < Duration::from_secs(2));
    }
    let exact = extension::run(&script("printf '%04096d' 1", 1000)).unwrap();
    assert_eq!(exact.0.len(), 4096);
}

#[test]
fn background_pipe_holder_is_killed_without_waiting_for_eof() {
    let start = Instant::now();
    let result = extension::run(&script("sleep 30 & printf 'ready\\n'", 1000)).unwrap();
    assert_eq!(result.0, "ready");
    assert!(start.elapsed() < Duration::from_secs(2));
}

#[test]
fn process_groups_are_terminated_after_exit_and_timeout() {
    let f = Fixture::new();
    for tail in ["exit 0", "sleep 30"] {
        let marker = f.0.join("survived");
        let cmd = format!(
            "(sleep 0.3; touch '{}') & printf 'ok\\n'; {tail}",
            marker.display()
        );
        let _ = extension::run(&script(&cmd, 60));
        std::thread::sleep(Duration::from_millis(400));
        assert!(!marker.exists(), "descendant survived {tail}");
    }
}

#[test]
fn ffi_ownership_null_and_parallel_collection() {
    let f = Fixture::new();
    let cfg = f.write("config", "interval_seconds=7\ndisks=[]\nfan_sensors=[]");
    let cpath = CString::new(cfg.to_str().unwrap()).unwrap();
    // SAFETY: cpath is live and terminated; every snapshot is released once unchanged.
    unsafe {
        monitor_snapshot_free(std::ptr::null_mut());
        let ptr = monitor_collect(cpath.as_ptr());
        assert!(!ptr.is_null());
        assert_eq!((*ptr).interval_seconds, 7);
        assert!(!CStr::from_ptr((*ptr).text).to_bytes().is_empty());
        assert!(CStr::from_ptr((*ptr).tooltip).to_str().is_ok());
        monitor_snapshot_free(ptr);
    }
    assert_eq!(c_string("a\0b".into()).to_str().unwrap(), "a�b");
    let threads: Vec<_> = (0..4)
        .map(|_| {
            let cfg = cfg.clone();
            std::thread::spawn(move || collect(Some(&cfg)).interval_seconds)
        })
        .collect();
    for t in threads {
        assert_eq!(t.join().unwrap(), 7);
    }
}

#[test]
fn failed_metrics_are_hidden_with_diagnostics() {
    let f = Fixture::new();
    let bad_sensor = f.write("sensor", "not a number");
    let cfg = f.write(
        "config",
        &format!(
            "cpu_sensor={:?}\nfan_sensors=[{:?}]\ndisks=[{:?}]\n\
         [[extensions]]\nname='BadMetric'\ncommand=['/bin/sh','-c','echo problem >&2; exit 3']\n\
         [[extensions]]\nname='GoodMetric'\ncommand=['/bin/sh','-c','printf \"42\\ndetail\\n\"']\n",
            bad_sensor.to_str().unwrap(),
            bad_sensor.to_str().unwrap(),
            f.0.join("missing-disk").to_str().unwrap()
        ),
    );
    let s = collect_from(Some(&cfg), &f.0.join("hwmon"), &f.0.join("missing-meminfo"));
    assert_eq!(s.text, "GoodMetric: 42");
    assert!(s.tooltip.contains("invalid sensor reading"));
    assert!(s.tooltip.contains("Filesystem unavailable"));
    assert!(s.tooltip.contains("Extension BadMetric:"));
    assert!(s.tooltip.contains("problem"));
    assert!(s.tooltip.contains("detail"));
}
