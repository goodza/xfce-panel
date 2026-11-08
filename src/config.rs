use serde::Deserialize;
use std::{
    env, fs,
    path::{Path, PathBuf},
};

#[derive(Debug, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    pub interval_seconds: u32,
    pub disks: Vec<PathBuf>,
    pub cpu_sensor: Option<PathBuf>,
    pub fan_sensors: Option<Vec<PathBuf>>,
    pub extensions: Vec<Extension>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            interval_seconds: 2,
            disks: vec![PathBuf::from("/")],
            cpu_sensor: None,
            fan_sensors: None,
            extensions: Vec::new(),
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Extension {
    pub name: String,
    pub command: Vec<String>,
    #[serde(default = "default_timeout")]
    pub timeout_ms: u64,
}

fn default_timeout() -> u64 {
    1000
}

pub fn default_path() -> Option<PathBuf> {
    env::var_os("XDG_CONFIG_HOME")
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .or_else(|| {
            env::var_os("HOME")
                .filter(|s| !s.is_empty())
                .map(|p| PathBuf::from(p).join(".config"))
        })
        .map(|p| p.join("xfce4/panel/rust-monitor.toml"))
}

pub fn plain_text(s: &str) -> bool {
    !s.chars().any(|c| c.is_control() && c != '\n' && c != '\t')
}

impl Config {
    fn validate(&self) -> Result<(), String> {
        if !(1..=3600).contains(&self.interval_seconds) {
            return Err("interval_seconds must be 1..3600".into());
        }
        if self.extensions.len() > 16 {
            return Err("at most 16 extensions are allowed".into());
        }
        for e in &self.extensions {
            if e.name.trim().is_empty() || !plain_text(&e.name) || e.name.contains(['\n', '\t']) {
                return Err("extension name must be a nonempty single line".into());
            }
            if e.command.is_empty()
                || e.command[0].is_empty()
                || e.command.iter().any(|s| s.contains('\0'))
            {
                return Err(format!(
                    "{}: command must contain executable argv without NUL",
                    e.name
                ));
            }
            if !(10..=10000).contains(&e.timeout_ms) {
                return Err(format!("{}: timeout_ms must be 10..10000", e.name));
            }
        }
        Ok(())
    }
}

pub fn load(path: Option<&Path>) -> (Config, Option<String>) {
    let default = default_path();
    let Some(path) = path.or(default.as_deref()) else {
        return (
            Config::default(),
            Some("Cannot locate configuration: HOME and XDG_CONFIG_HOME unavailable".into()),
        );
    };
    let result = match fs::read_to_string(path) {
        Ok(text) => toml::from_str::<Config>(&text)
            .map_err(|e| e.to_string())
            .and_then(|c| {
                c.validate()?;
                Ok(c)
            }),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return (Config::default(), None),
        Err(e) => Err(e.to_string()),
    };
    match result {
        Ok(c) => (c, None),
        Err(e) => (
            Config::default(),
            Some(format!(
                "Configuration {}: {e}; using defaults",
                path.display()
            )),
        ),
    }
}
