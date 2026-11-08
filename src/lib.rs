mod config;
mod extension;
mod sensors;

use std::{
    ffi::{CStr, CString},
    os::raw::c_char,
    path::Path,
};

pub struct Snapshot {
    pub text: String,
    pub tooltip: String,
    pub interval_seconds: u32,
    pub cpu_hot: bool,
}

pub fn collect(config_path: Option<&Path>) -> Snapshot {
    collect_from(
        config_path,
        Path::new("/sys/class/hwmon"),
        Path::new("/proc/meminfo"),
    )
}

fn collect_from(config_path: Option<&Path>, hwmon: &Path, meminfo: &Path) -> Snapshot {
    let (config, diagnostic) = config::load(config_path);
    let mut metrics = Vec::new();
    let mut details: Vec<String> = diagnostic.into_iter().collect();
    let (temps, discovered_fans) = sensors::discover(hwmon);
    let temps = config.cpu_sensor.map(|p| vec![p]).unwrap_or(temps);
    let mut hottest = None;
    for path in temps {
        match sensors::reading(&path) {
            Ok(value) => {
                hottest = Some(hottest.map_or(value, |old: i64| old.max(value)));
            }
            Err(e) => details.push(e),
        }
    }
    if let Some(temp) = hottest {
        let value = format!("{:.1} °C", temp as f64 / 1000.0);
        details.push(format!("CPU {value}"));
        metrics.push(value);
    } else {
        details.push("CPU temperature unavailable: no readable recognized CPU sensor".into());
    }
    let mut fans = config.fan_sensors.unwrap_or(discovered_fans);
    fans.sort();
    if fans.is_empty() {
        details.push("Fan readings unavailable or disabled".into());
    }
    for (i, path) in fans.iter().enumerate() {
        match sensors::reading(path) {
            Ok(rpm) if rpm >= 0 => {
                match i {
                    0 => metrics.push(format!("{rpm} RPM")),
                    1 => {}
                    _ => metrics.push(format!("Fan {} {rpm} RPM", i + 1)),
                }
                details.push(format!("Fan {} ({}): {rpm} RPM", i + 1, path.display()));
            }
            Ok(_) => details.push(format!("{}: negative fan reading", path.display())),
            Err(e) => details.push(e),
        }
    }
    match std::fs::read_to_string(meminfo)
        .map_err(|e| e.to_string())
        .and_then(|s| sensors::mem_available(&s))
    {
        Ok(bytes) => {
            let value = format!("{:.2} GiB", bytes as f64 / 1073741824.0);
            details.push(format!("RAM {value} available"));
            metrics.push(value);
        }
        Err(e) => details.push(format!("Memory unavailable: {e}")),
    }
    for path in config.disks {
        match sensors::disk(&path) {
            Ok(disk) => {
                metrics.push(format!("{} {:.0}%", path.display(), disk.used_percent));
                details.push(format!(
                    "{}: {:.1}% used, {:.2} GiB free available",
                    path.display(),
                    disk.used_percent,
                    disk.available_bytes / 1073741824.0
                ));
            }
            Err(e) => details.push(format!("Filesystem unavailable: {e}")),
        }
    }
    for ext in config.extensions {
        match extension::run(&ext) {
            Ok((value, detail)) => {
                metrics.push(format!("{}: {value}", ext.name));
                details.push(format!(
                    "{}: {value}{}",
                    ext.name,
                    detail.map(|d| format!("\n{d}")).unwrap_or_default()
                ));
            }
            Err(e) => details.push(format!("Extension {}: {e}", ext.name)),
        }
    }
    Snapshot {
        text: if metrics.is_empty() {
            "Monitor".into()
        } else {
            metrics.join(" · ")
        },
        tooltip: details.join("\n"),
        interval_seconds: config.interval_seconds,
        cpu_hot: hottest.is_some_and(|temp| temp > 80_000),
    }
}

#[repr(C)]
pub struct MonitorSnapshot {
    pub text: *mut c_char,
    pub tooltip: *mut c_char,
    pub interval_seconds: u32,
    pub cpu_hot: u32,
}

fn c_string(s: String) -> CString {
    CString::new(s.replace('\0', "�")).expect("NUL replaced")
}

/// Collect a newly allocated snapshot; NULL selects the default configuration path.
///
/// # Safety
/// A non-NULL config_path must point to a valid NUL-terminated string for this call.
/// Release the returned pointer exactly once with monitor_snapshot_free.
#[no_mangle]
pub unsafe extern "C" fn monitor_collect(config_path: *const c_char) -> *mut MonitorSnapshot {
    std::panic::catch_unwind(|| {
        use std::os::unix::ffi::OsStrExt;
        let path = if config_path.is_null() {
            None
        } else {
            // SAFETY: the caller guarantees a valid terminated string.
            Some(Path::new(std::ffi::OsStr::from_bytes(
                unsafe { CStr::from_ptr(config_path) }.to_bytes(),
            )))
        };
        let snapshot = collect(path);
        Box::into_raw(Box::new(MonitorSnapshot {
            text: c_string(snapshot.text).into_raw(),
            tooltip: c_string(snapshot.tooltip).into_raw(),
            interval_seconds: snapshot.interval_seconds,
            cpu_hot: u32::from(snapshot.cpu_hot),
        }))
    })
    .unwrap_or(std::ptr::null_mut())
}

/// Release a snapshot returned by monitor_collect. NULL is a no-op.
///
/// # Safety
/// snapshot must be NULL or an unfreed pointer returned by monitor_collect.
/// Its fields must not have been modified or freed independently.
#[no_mangle]
pub unsafe extern "C" fn monitor_snapshot_free(snapshot: *mut MonitorSnapshot) {
    let _ = std::panic::catch_unwind(|| {
        if !snapshot.is_null() {
            // SAFETY: the caller transfers the original allocation and unchanged fields back.
            unsafe {
                let snapshot = Box::from_raw(snapshot);
                drop(CString::from_raw(snapshot.text));
                drop(CString::from_raw(snapshot.tooltip));
            }
        }
    });
}

#[cfg(test)]
mod tests;
