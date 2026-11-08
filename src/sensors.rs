use std::{
    ffi::CString,
    fs,
    path::{Path, PathBuf},
};

fn input_paths(dir: &Path, prefix: &str) -> Vec<PathBuf> {
    let mut paths: Vec<_> = fs::read_dir(dir)
        .into_iter()
        .flatten()
        .filter_map(Result::ok)
        .filter_map(|e| {
            let name = e.file_name();
            let name = name.to_str()?;
            let number = name.strip_prefix(prefix)?.strip_suffix("_input")?;
            (!number.is_empty() && number.bytes().all(|b| b.is_ascii_digit())).then(|| e.path())
        })
        .collect();
    paths.sort();
    paths
}

pub fn discover(root: &Path) -> (Vec<PathBuf>, Vec<PathBuf>) {
    let mut dirs: Vec<_> = fs::read_dir(root)
        .into_iter()
        .flatten()
        .filter_map(Result::ok)
        .map(|e| e.path())
        .collect();
    dirs.sort();
    let mut temps = Vec::new();
    let mut fans = Vec::new();
    for dir in dirs {
        if let Ok(name) = fs::read_to_string(dir.join("name")) {
            if matches!(
                name.trim(),
                "coretemp" | "k10temp" | "zenpower" | "cpu_thermal"
            ) {
                temps.extend(input_paths(&dir, "temp"));
            }
        }
        fans.extend(input_paths(&dir, "fan"));
    }
    (temps, fans)
}

pub fn reading(path: &Path) -> Result<i64, String> {
    fs::read_to_string(path)
        .map_err(|e| format!("{}: {e}", path.display()))?
        .trim()
        .parse()
        .map_err(|e| format!("{}: invalid sensor reading: {e}", path.display()))
}

pub fn mem_available(text: &str) -> Result<u64, String> {
    let line = text
        .lines()
        .find_map(|line| line.strip_prefix("MemAvailable:"))
        .ok_or("MemAvailable missing from meminfo")?;
    let mut parts = line.split_whitespace();
    let kb: u64 = parts
        .next()
        .ok_or("MemAvailable value missing")?
        .parse()
        .map_err(|_| "Invalid MemAvailable value")?;
    if parts.next() != Some("kB") || parts.next().is_some() {
        return Err("Invalid MemAvailable unit".into());
    }
    kb.checked_mul(1024)
        .ok_or_else(|| "MemAvailable overflow".into())
}

pub struct Disk {
    pub used_percent: f64,
    pub available_bytes: f64,
}

pub fn disk(path: &Path) -> Result<Disk, String> {
    use std::os::unix::ffi::OsStrExt;
    let cpath = CString::new(path.as_os_str().as_bytes())
        .map_err(|_| "Disk path contains NUL".to_string())?;
    let mut stats = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    // SAFETY: cpath is terminated and stats points to writable storage.
    if unsafe { libc::statvfs(cpath.as_ptr(), stats.as_mut_ptr()) } != 0 {
        return Err(format!(
            "{}: {}",
            path.display(),
            std::io::Error::last_os_error()
        ));
    }
    // SAFETY: successful statvfs initialized stats.
    let stats = unsafe { stats.assume_init() };
    let used = stats.f_blocks.saturating_sub(stats.f_bfree) as f64;
    let available = stats.f_bavail as f64;
    let denominator = used + available;
    if denominator == 0.0 {
        return Err(format!(
            "{}: filesystem has no usable blocks",
            path.display()
        ));
    }
    Ok(Disk {
        used_percent: used / denominator * 100.0,
        available_bytes: available * stats.f_frsize as f64,
    })
}
