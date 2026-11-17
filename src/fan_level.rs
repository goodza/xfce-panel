use std::{fs, path::Path};

const FAN_PATH: &str = "/proc/acpi/ibm/fan";

pub(crate) struct FanLevelStatus {
    pub text: String,
    pub tooltip: String,
    pub failed: bool,
}

pub(crate) fn level(text: &str) -> Result<&str, String> {
    let mut levels = text.lines().filter_map(|line| line.strip_prefix("level:"));
    let value = levels.next().ok_or("ThinkPad fan level missing")?.trim();
    if levels.next().is_some() {
        return Err("ThinkPad fan level ambiguous".into());
    }
    match value {
        "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "auto" => Ok(value),
        // thinkpad_acpi documents these states, but neither is a numeric level or auto.
        "disengaged" | "full-speed" => Err(format!(
            "ThinkPad fan level {value} is not displayable as a number or auto"
        )),
        _ => Err("ThinkPad fan level invalid".into()),
    }
}

fn unavailable(error: String) -> FanLevelStatus {
    FanLevelStatus {
        text: "?".into(),
        tooltip: format!("Fan level unavailable: {error}"),
        failed: true,
    }
}

fn read_from(path: &Path) -> FanLevelStatus {
    match fs::read_to_string(path) {
        Ok(text) => match level(&text) {
            Ok(value) => FanLevelStatus {
                text: value.into(),
                tooltip: format!("Current fan level: {value}"),
                failed: false,
            },
            Err(error) => unavailable(error),
        },
        Err(error) => unavailable(format!("{}: {error}", path.display())),
    }
}

pub(crate) fn read() -> FanLevelStatus {
    read_from(Path::new(FAN_PATH))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tests::Fixture;

    #[test]
    fn documented_displayable_levels_are_accepted() {
        for value in ["0", "1", "2", "3", "4", "5", "6", "7", "auto"] {
            assert_eq!(
                level(&format!(
                    "status:\t\tenabled\nspeed:\t\t2400\nlevel:\t\t{value}\n"
                ))
                .unwrap(),
                value
            );
        }
        assert_eq!(level("level: 7").unwrap(), "7");
        assert_eq!(level("level:\tauto").unwrap(), "auto");
    }

    #[test]
    fn malformed_missing_and_ambiguous_levels_are_rejected() {
        for text in [
            "",
            "status:\t\tenabled\nspeed:\t\t2400\n",
            "level:",
            "level: 8",
            "level: -1",
            "level: +1",
            "level: 00",
            "level: 0x1",
            "level: 0 1",
            "level: automatic",
            "level: 0\nlevel: 0",
            "level: auto\nlevel: 7",
        ] {
            assert!(level(text).is_err(), "{text:?}");
        }
        for text in ["level: disengaged", "level: full-speed"] {
            let error = level(text).unwrap_err();
            assert!(error.contains("not displayable"), "{error}");
        }
    }

    #[test]
    fn fixture_reader_reports_values_and_unavailable_diagnostics() {
        let f = Fixture::new();
        let fan = f.write("fan", "status:\t\tenabled\nspeed:\t\t2400\nlevel:\t\t3\n");
        let status = read_from(&fan);
        assert!(!status.failed);
        assert_eq!(status.text, "3");
        assert_eq!(status.tooltip, "Current fan level: 3");

        f.write("fan", "level: auto\n");
        let status = read_from(&fan);
        assert!(!status.failed);
        assert_eq!(status.text, "auto");

        for (contents, diagnostic) in [
            ("", "missing"),
            ("level: 8\n", "invalid"),
            ("level: 0\nlevel: 0\n", "ambiguous"),
            (
                "level: disengaged\n",
                "ThinkPad fan level disengaged is not displayable",
            ),
            (
                "level: full-speed\n",
                "ThinkPad fan level full-speed is not displayable",
            ),
        ] {
            f.write("fan", contents);
            let status = read_from(&fan);
            assert!(status.failed);
            assert_eq!(status.text, "?");
            assert!(status.tooltip.contains(diagnostic), "{}", status.tooltip);
        }

        let status = read_from(&f.0.join("missing"));
        assert!(status.failed);
        assert_eq!(status.text, "?");
        assert!(status.tooltip.contains("Fan level unavailable"));
        assert!(status.tooltip.contains("missing"));

        let status = read_from(&f.0);
        assert!(status.failed);
        assert_eq!(status.text, "?");
        assert!(status.tooltip.contains("Fan level unavailable"));
    }
}
