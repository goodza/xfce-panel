use crate::{config, extension, sensors};
use std::{fs, path::Path};

const FAN_PATH: &str = "/proc/acpi/ibm/fan";

#[derive(Clone, Copy, Debug, PartialEq)]
enum Level {
    Auto,
    Off,
}

impl Level {
    fn input(self) -> &'static [u8] {
        match self {
            Self::Auto => b"level auto\n",
            Self::Off => b"level 0\n",
        }
    }
}

fn speed(text: &str) -> Result<u64, String> {
    let mut readings = text.lines().filter_map(|line| line.strip_prefix("speed:"));
    let value = readings
        .next()
        .ok_or("ThinkPad fan speed unavailable")?
        .trim()
        .parse()
        .map_err(|_| "ThinkPad fan speed invalid")?;
    if readings.next().is_some() {
        return Err("ThinkPad fan speed ambiguous".into());
    }
    Ok(value)
}

fn requested_level(temp: i64, rpm: u64) -> Option<Level> {
    if temp > 80_000 {
        Some(Level::Auto)
    } else if temp < 58_000 && rpm < 2000 {
        Some(Level::Off)
    } else {
        None
    }
}

fn write_with_command(level: Level, command: Vec<String>) -> Result<(), String> {
    extension::run_with_input(
        &config::Extension {
            name: "Smart fan control".into(),
            command,
            timeout_ms: 1000,
        },
        Some(level.input()),
    )
    .map(|_| ())
}

fn write_level(level: Level) -> Result<(), String> {
    write_with_command(
        level,
        ["/usr/bin/sudo", "-n", "/usr/bin/tee", FAN_PATH]
            .map(str::to_owned)
            .to_vec(),
    )
}

fn step_from(
    config_path: Option<&Path>,
    hwmon: &Path,
    fan_path: &Path,
    mut write: impl FnMut(Level) -> Result<(), String>,
) -> String {
    let result = (|| {
        let (config, diagnostic) = config::load(config_path);
        if let Some(error) = diagnostic {
            return Err(error);
        }
        let temps = config
            .cpu_sensor
            .map(|path| vec![path])
            .unwrap_or_else(|| sensors::discover(hwmon).0);
        let (temp, errors) = sensors::cpu_temperature(temps);
        if !errors.is_empty() {
            return Err(format!("CPU sensor error: {}", errors.join("; ")));
        }
        let temp = temp
            .filter(|temp| *temp >= 0)
            .ok_or("CPU temperature unavailable or invalid")?;
        let rpm = speed(
            &fs::read_to_string(fan_path).map_err(|e| format!("{}: {e}", fan_path.display()))?,
        )?;
        match requested_level(temp, rpm) {
            Some(level) => {
                write(level)?;
                Ok(match level {
                    Level::Auto => "Smart fan control on: level auto".to_owned(),
                    Level::Off => "Smart fan control on: level 0".to_owned(),
                })
            }
            None => Ok("Smart fan control on: fan level unchanged".to_owned()),
        }
    })();
    match result {
        Ok(status) => status,
        Err(error) => match write(Level::Auto) {
            Ok(()) => {
                format!("Smart fan control on: {error}\nFallback level auto applied; retrying")
            }
            Err(fallback) => format!(
                "Smart fan control on: {error}\nFallback level auto failed: {fallback}\nRetrying"
            ),
        },
    }
}

pub fn step() -> String {
    step_from(
        None,
        Path::new("/sys/class/hwmon"),
        Path::new(FAN_PATH),
        write_level,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tests::Fixture;

    #[test]
    fn strict_temperature_and_rpm_boundaries() {
        for (temp, rpm, expected) in [
            (80_001, 5000, Some(Level::Auto)),
            (80_000, 0, None),
            (58_000, 0, None),
            (57_999, 1999, Some(Level::Off)),
            (57_999, 2000, None),
            (57_999, 2001, None),
            (57_999, 0, Some(Level::Off)),
            (70_000, 1000, None),
        ] {
            assert_eq!(requested_level(temp, rpm), expected, "{temp}, {rpm}");
        }
        assert_eq!(
            speed("status: enabled\nspeed: 0\nlevel: auto\n").unwrap(),
            0
        );
        assert_eq!(speed("speed:\t1999\n").unwrap(), 1999);
        for text in ["", "speed: -1", "speed: invalid", "speed: 2\nspeed: 3"] {
            assert!(speed(text).is_err());
        }
    }

    #[test]
    fn readings_reload_commands_and_fallback_retry() {
        let f = Fixture::new();
        let cpu = f.write("cpu", "57000");
        let config = f.write("config", &format!("cpu_sensor={cpu:?}\nfan_sensors=[]"));
        let fan = f.write("fan", "speed: 1800\n");
        let mut writes = Vec::new();
        let mut run = |fail: bool| {
            step_from(Some(&config), &f.0, &fan, |level| {
                writes.push(level);
                if fail {
                    Err("permission denied".into())
                } else {
                    Ok(())
                }
            })
        };
        assert!(run(false).contains("level 0"));
        f.write("cpu", "80001");
        assert!(run(false).contains("level auto"));
        f.write("cpu", "70000");
        assert!(run(false).contains("unchanged"));
        f.write("cpu", "57000");
        let error = run(true);
        assert!(error.contains("permission denied"));
        assert!(error.contains("Fallback level auto failed"));
        assert!(run(false).contains("level 0"));
        assert_eq!(
            writes,
            [Level::Off, Level::Auto, Level::Off, Level::Auto, Level::Off]
        );
        for (path, contents) in [
            ("cpu", "bad"),
            ("cpu", "-1"),
            ("fan", "speed: invalid"),
            ("config", "unknown=true"),
        ] {
            f.write("cpu", "57000");
            f.write("fan", "speed: 1800");
            f.write("config", &format!("cpu_sensor={cpu:?}\nfan_sensors=[]"));
            f.write(path, contents);
            let mut fallback = Vec::new();
            let status = step_from(Some(&config), &f.0, &fan, |level| {
                fallback.push(level);
                Ok(())
            });
            assert_eq!(fallback, [Level::Auto]);
            assert!(status.contains("Fallback level auto applied"));
        }
    }

    #[test]
    fn partial_or_missing_readings_never_stop_fan() {
        let f = Fixture::new();
        f.write("hwmon/hwmon0/name", "coretemp");
        f.write("hwmon/hwmon0/temp1_input", "57000");
        f.write("hwmon/hwmon0/temp2_input", "invalid");
        let config = f.write("config", "disks=[]");
        let fan = f.write("fan", "speed: 1000");
        for fan in [&fan, &f.0.join("missing")] {
            let status = step_from(Some(&config), &f.0.join("hwmon"), fan, |level| {
                assert_eq!(level, Level::Auto);
                Ok(())
            });
            assert!(status.contains("Fallback"));
        }
        f.write("hwmon/hwmon0/temp2_input", "81000");
        let status = step_from(Some(&config), &f.0.join("hwmon"), &fan, |level| {
            assert_eq!(level, Level::Auto);
            Ok(())
        });
        assert_eq!(status, "Smart fan control on: level auto");
        let status = step_from(
            Some(&config),
            &f.0.join("hwmon"),
            &f.0.join("missing"),
            |level| {
                assert_eq!(level, Level::Auto);
                Ok(())
            },
        );
        assert!(status.contains("Fallback level auto applied"));
    }

    #[test]
    fn command_input_failures_and_timeout_are_bounded() {
        let f = Fixture::new();
        let output = f.0.join("input");
        write_with_command(
            Level::Off,
            vec!["/usr/bin/tee".into(), output.to_str().unwrap().into()],
        )
        .unwrap();
        assert_eq!(fs::read(&output).unwrap(), b"level 0\n");
        write_with_command(
            Level::Auto,
            vec!["/usr/bin/tee".into(), output.to_str().unwrap().into()],
        )
        .unwrap();
        assert_eq!(fs::read(&output).unwrap(), b"level auto\n");
        let error = write_with_command(
            Level::Auto,
            vec![
                "/bin/sh".into(),
                "-c".into(),
                "echo denied >&2; exit 7".into(),
            ],
        )
        .unwrap_err();
        assert!(error.contains("denied") && error.contains('7'));
        assert!(write_with_command(Level::Auto, vec!["/missing".into()]).is_err());
        let start = std::time::Instant::now();
        assert!(write_with_command(
            Level::Off,
            vec!["/bin/sh".into(), "-c".into(), "sleep 30".into()]
        )
        .unwrap_err()
        .contains("timed out"));
        assert!(start.elapsed() < std::time::Duration::from_secs(3));
    }
}
