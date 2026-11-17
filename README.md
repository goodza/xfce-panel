# Rust Monitor for XFCE4

Extensible Linux system metrics in the XFCE4 panel: CPU temperature, fan RPM, available RAM, and filesystem usage. Rust handles collection and scripts; a small C/GTK3 adapter integrates with XFCE.

```text
54.0 °C · 1240 RPM · 8.20 GiB · / 62%
```

Metrics appear inline without CPU, first-fan, or RAM labels; hover retains full labels and diagnostics. The second fan is tooltip-only (numbering follows sorted configured/discovered sensor paths). CPU temperature turns red strictly above 80°C and returns to normal at or below 80°C or when unavailable. The threshold uses the raw reading, so a value rounded to 80.0°C can still be red. Other metrics keep their normal color. Missing readings and failing extensions are omitted. Vertical/deskbar panels stack metrics. Right-click toggles full display ↔ RAM-only; Ctrl+right-click passes through to XFCE's plugin menu. Each instance starts full and remembers the selection only until it is removed or the panel restarts. Hover details and polling continue in either view. RAM-only mode never inherits the CPU warning color; returning full restores the current warning. Until RAM is available, minimized mode shows Monitor with diagnostics. There is no Applications menu or general settings dialog; left-clicking the readings opens the fan temperature controls. A separate **fan icon toggle** enables optional ThinkPad smart fan control and remains visible in RAM-only mode; see [Smart fan control](#smart-fan-control) before enabling it. Beside the icon, the actual hardware fan level appears as a bare `0`–`7` or `auto`, or `?` with its diagnostic in the level tooltip; see [Fan level display](#fan-level-display).

## Try without installing

Requires a current stable Rust toolchain and a C linker on Linux:

```sh
cargo run --bin rust-monitor
cargo run --bin rust-monitor -- --config config.example.toml
make test
make test-ffi
```

The CLI prints one snapshot followed by its tooltip. `make test-ffi` additionally builds the release static library and calls it from C; neither command requires GTK headers.

## Build the native plugin

On Debian/Ubuntu, install these dependencies yourself (not installed by this project):

```sh
sudo apt install build-essential pkg-config libxfce4panel-2.0-dev libxfce4ui-2-dev libgtk-3-dev libglib2.0-dev
make
```

Requires XFCE4 panel **4.19 or newer** (including 4.20), panel API 2.0, GTK3, GIO, Cargo, and a Linux ELF linker supporting `-z nodelete`. `make` builds a PIC Rust release library, CLI, and `target/native/panel/librust-monitor.so`. Custom `CARGO_TARGET_DIR` is supported. Cargo may download crates on the first build.

**Verification status:** Native compilation and shared-library loading pass on XFCE 4.20.7 / GTK 3.24.52. Rust checks, the C ABI smoke test, and an isolated GTK test with transparent native windows pass. The native test checks compact readings, the hidden second fan's retained tooltip, the bare hardware fan level beside the icon in full and RAM-only views, raw CPU warning boundaries and reset, real RAM/disk metrics, literal text, right-click toggles and modifier propagation, polling/reload while minimized, orientation callbacks, and removal during a running extension. A second test module uses simulated fan-control and read-only fan-level backends to verify nonblocking toggles, independent polling (including one-second level reads while metrics are stalled and control is off), failure/retry, `?` diagnostics without control fallback, rapid off/on transitions, disabling to `level 0` after any in-flight check, failed disable writes without automatic fallback, one observation in flight, RAM-only visibility, and teardown of all workers without sudo or hardware writes. Rust control fixtures also cover the exact auto-level/<2200-RPM transition, its strict boundary, ignored malformed level text, hot-CPU `auto` priority, and the hot auto/<2200-RPM kick (`level 0`, three-second delay, then `level auto`) including failure fallback. Popup tests cover slider bounds, per-instance reload, invalid settings, save failure/recovery, error-area show/clear on fan failure, recovery and disable, and teardown with default and disabled animations. A separate-process X11 GtkSocket/GtkPlug test verifies the slider popup is a native toplevel that is not clipped to panel height. Escape is tested by injecting GTK's key signal; physical keyboard and outside-click routing still need a live-panel check. Staged installation also passes. Real fan writes, sudo authorization, actual panel discovery, external-wrapper integration, and live panel orientation remain unverified; your panel configuration has not been changed.

```sh
make test-native
make test-embedded  # X11-only, separate-process XEmbed regression
# For a headless environment with Xvfb installed:
# xvfb-run -a make test-native
```

The native test requires a working GTK display and uses transparent native windows, because XFCE popup positioning needs real window origins. `test-embedded` additionally requires X11 and runs its socket and plugin in separate processes. Both use temporary configuration, never your real config or fan-control writes; a missing display is a failure, not a skipped pass.

## Install into XFCE's plugin directories

Use the library directory of your installed panel, especially on multiarch systems. Do not assume `/usr/local/lib` is searched by your distribution's panel.

```sh
PANEL_LIBDIR="$(pkg-config --variable=libdir libxfce4panel-2.0)"
make PREFIX=/usr LIBDIR="$PANEL_LIBDIR"
sudo make install PREFIX=/usr LIBDIR="$PANEL_LIBDIR"
```

Then open **Panel → Add New Items → Rust Monitor**. If not listed, restart the panel yourself with `xfce4-panel -r` and reopen the item list. Installation does not add an item or restart your panel automatically. Multiple instances share the metric/sensor configuration file but save their fan temperature ranges separately.

For staging without touching system files:

```sh
make install PREFIX=/usr LIBDIR="$PANEL_LIBDIR" DESTDIR="$PWD/build/stage"
```

Remove the panel item before uninstalling, then use the same directory overrides:

```sh
sudo make uninstall PREFIX=/usr LIBDIR="$PANEL_LIBDIR"
```

`make clean` removes the native Cargo target directory (default `target/native`); use `cargo clean` separately for ordinary debug artifacts.

## Configuration

The plugin reloads `${XDG_CONFIG_HOME:-$HOME/.config}/xfce4/panel/rust-monitor.toml` on each refresh. Relative `XDG_CONFIG_HOME` is ignored. Missing file uses defaults; malformed or invalid configuration uses defaults and adds a tooltip diagnostic.

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/xfce4/panel"
cp config.example.toml "${XDG_CONFIG_HOME:-$HOME/.config}/xfce4/panel/rust-monitor.toml"
```

```toml
interval_seconds = 2
disks = ["/", "/home"]
# Optional explicit millidegree CPU sensor:
# cpu_sensor = "/sys/class/hwmon/hwmon6/temp1_input"
# Omit for automatic fan discovery; [] disables fan metrics:
# fan_sensors = ["/sys/class/hwmon/hwmon7/fan1_input"]

[[extensions]]
name = "Custom"
command = ["/absolute/path/to/metric-script", "argument"]
timeout_ms = 1000
```

- `interval_seconds`: 1–3600, default 2; delay **after** the previous collection finishes, not a strict wall-clock cadence.
- `disks`: mount/directory paths, default `["/"]`; `[]` disables disk metrics. Each path reports its containing filesystem, so paths on the same filesystem repeat its usage.
- CPU discovery uses the hottest reading exposed by `coretemp`, `k10temp`, `zenpower`, or `cpu_thermal`; explicit paths support other drivers. Fan discovery uses all `fanN_input` files. Missing/unreadable sensors are hidden; a real 0 RPM remains visible. No sensor kernel modules are loaded automatically. hwmon indices can change across boots; update explicit paths if needed.
- RAM is Linux `MemAvailable`, not `MemFree`. GiB uses 1024³ bytes.
- Disk percentage is `used / (used + available)` from `statvfs`, excluding reserved blocks from available capacity. Tooltip space is available to an unprivileged user. No disk I/O throughput is measured.
- Unknown config keys and invalid extension definitions reject the whole config, with a diagnostic.

## Fan level display

The bare number or `auto` beside the fan icon is the actual level read from `level:` in `/proc/acpi/ibm/fan`, never inferred from the smart-control toggle or a previous command. A background observer rereads it one second after each read completes; it is independent of metric refresh, extensions, and whether smart control is on, and remains visible in full and RAM-only views. One observation runs at a time.

Only the documented displayable values `0`–`7` and `auto` are shown. `disengaged` and `full-speed` are documented hardware states, but neither is a number or `auto`, so they display `?`. Missing, malformed, duplicate/ambiguous, unsupported, or unreadable output also displays `?`; hover the level for the diagnostic. Observation only reads the file: it never writes a level, invokes sudo, or triggers smart control's fallback command. The level tooltip is separate from the fan icon's smart-control tooltip. On hardware without `/proc/acpi/ibm/fan`, the display stays `?` without affecting the other metrics.

## Smart fan control

Click the fan icon to enable or disable control. It starts **off** each time the plugin starts, without saving its state or issuing a level command. The pressed icon indicates enabled control; hover over it for the latest action or error. Disabling requests hardware `level 0` through the same bounded command; removing the plugin does not issue a level command. Metric right-click behavior is unchanged.

**Left-click the readings** (also in RAM-only mode) to open two temperature sliders:

- **Fan off below:** lower threshold, initially **58°C**.
- **Automatic above:** upper threshold, initially **60°C**.

Both sliders use 1°C steps from 30–90°C. They cannot cross: the lower threshold stays at least 1°C below the upper. Changes apply on the next fan check while dragging; an in-flight check may finish with its previous range. Adjusting the range never enables fan control. Click outside or press Escape to close the popup. It uses XFCE's native popup-window API so the sliders can appear outside the external plugin's panel-sized window. Below the sliders the popup shows **only current errors** — an active fan-control failure or a range load/save failure — and is otherwise empty; each clears when its source recovers, and switching smart control off clears its fan error. The red CPU display warning stays fixed at >80°C.

Each panel instance remembers its range in XFCE's per-instance rc file, separately from `rust-monitor.toml`; fan enablement still starts off. Missing saved settings use 58–60°C. Invalid saved settings show a warning and use those defaults. If saving fails, the selected range still works for this session and the popup shows a **Could not save range** error; the next adjustment retries and clears it on success. Existing malformed rc files are not overwritten. Manual rc edits take effect when the instance restarts.

While enabled, a separate background check runs immediately, then two seconds after each check completes, independently of metric scripts and `interval_seconds`:

- If the current hardware level is exactly `auto`, the raw CPU temperature is **>80°C**, and ThinkPad fan speed is **<2200 RPM** → write `level 0`, wait **3 seconds**, then write `level auto`. This kick takes precedence over the temperature transitions below.
- CPU **above the upper threshold** → write `level auto`, unless the kick rule above applied.
- Otherwise, if the current hardware level is exactly `auto` and ThinkPad fan speed is **<2200 RPM** → write `level 0`.
- Otherwise, CPU **below the lower threshold** **and** ThinkPad fan speed **<2000 RPM** → write `level 0`.
- Otherwise leave the level unchanged. Bounds are strict: exactly 80°C is not above, exactly the upper threshold is not above, exactly the lower threshold is not below, and exactly 2200 or 2000 RPM does not trigger its transition.

CPU selection follows `cpu_sensor` or normal hottest-CPU discovery. Current level comes from `level:` and control RPM from `speed:` in `/proc/acpi/ibm/fan`, regardless of `fan_sensors` or the displayed fan metric. Only an exact current `auto` activates the 2200-RPM and kick rules; missing, unsupported, or malformed level text is ignored for those rules and does not cause a control failure. Missing/invalid speed or CPU readings, invalid configuration, or command failures attempt automatic control, report the failure/fallback result in the icon tooltip and the slider popup's error area, and keep retrying while enabled. Each command has a one-second timeout; the kick adds a fixed three-second delay between its two commands. A persistent hot/low-RPM condition can trigger another kick on a later check.

**Important:** Switching smart control off requests `level 0`, which stops the fan; it never restores automatic control. If a check or kick is already running, the level-0 write waits for it to finish so the disabled state wins. A failed write leaves the previous level in place and reports the error in the icon tooltip and popup; it does not fall back to `level auto`. Removing the plugin does not issue a level command and leaves the last level unchanged, but removal waits for an active kick to finish its delayed `level auto`, so it cannot leave the temporary `level 0` behind. A check already started may finish after switching off, but the queued level-0 write runs after it. This plugin is not a fail-safe cooling service. Enable only one controller; do not run it alongside competing fan-management tools. To restore automatic control yourself:

```sh
printf 'level auto\n' | sudo /usr/bin/tee /proc/acpi/ibm/fan
```

### Required permissions and hardware

This feature requires a supported ThinkPad with a readable/writable `/proc/acpi/ibm/fan` and manual fan control enabled in the kernel driver. No modules or kernel settings are changed by the plugin. Confirm support for your model before using fan-off mode.

The plugin executes `/usr/bin/sudo -n /usr/bin/tee /proc/acpi/ibm/fan` with fixed stdin (`level auto\n` or `level 0\n`), not a shell. `-n` forbids password prompts, so a scoped **NOPASSWD** rule is required — without it the fan toggle shows `sudo: a password is required` / *interactive authentication required* and never writes a level. Grant it once with the setup script (validates the rule with `visudo -c` before installing, and re-validates the merged sudoers afterward, rolling back on failure):

```sh
scripts/install-fan-sudoers.sh            # install for the invoking user (one password prompt)
scripts/install-fan-sudoers.sh --dry-run  # show the rule without changing anything
scripts/install-fan-sudoers.sh --remove   # uninstall
```

It writes `/etc/sudoers.d/rust-monitor-fan` (mode 0440, root:root) containing exactly:

```sudoers
youruser ALL=(root) NOPASSWD: /usr/bin/tee /proc/acpi/ibm/fan
```

Prefer to do it by hand instead? `sudo visudo -f /etc/sudoers.d/rust-monitor-fan` and add that line for your user. Either way the rule is pinned to one command and one file argument — it grants that user control over the fan interface and nothing else (no shell, no other paths, no unrestricted `tee`). Fixed executable paths must exist. Without permission or driver support, the icon tooltip and the slider popup report the error rather than prompting. Tests do not verify real sudo authorization or hardware writes. The CLI and ordinary metric collection never change fan levels.

## Add a metric script

Make a local script executable and add its absolute path as `command`. See `examples/metric.sh`.

```sh
#!/bin/sh
printf '42%%\nExtra detail shown on hover\n'
```

`command` is an argv array; no shell expansion is performed. Use absolute paths: `~`, `$HOME`, and shell operators are not expanded. Executables run as your logged-in user with the panel's environment. Scripts are **trusted code, not sandboxed**.

Output contract:

- Exit 0; first stdout line is the value, optional second line is hover detail. Additional lines are ignored.
- UTF-8 plain text only; no markup. NUL and control characters except LF/tab are rejected (including CRLF).
- Combined stdout/stderr limit: 4096 bytes. `timeout_ms`: 10–10000, default 1000. Maximum 16 extensions, executed sequentially.
- Nonzero exit, timeout, invalid output, empty value, or overflow hides the metric and adds a tooltip diagnostic. stderr from failures is bounded and included.
- The extension process group is killed and the direct child reaped on completion/failure, including after a successful leader exits. Background jobs in that group are not supported. Deliberately detached descendants can escape this cleanup; do not use daemonizing scripts.

Collection normally runs off the UI thread with one job in flight. Removing the plugin waits for an outstanding collection to finish so wrapper exit cannot orphan scripts. Many slow extensions can delay removal (up to 160 seconds of configured script timeouts); blocked filesystem/device I/O can take longer. Avoid network mounts and blocking special files in configuration.

## Native verification checklist

For the remaining live-panel checks:

1. Run `make test-native` and, on X11, `make test-embedded`, then install into the panel's actual directories.
2. Add Rust Monitor; check inline readings and plain-text hover details.
3. Edit the config and verify reload, invalid-config diagnostics, explicit sensors, and additional filesystems.
4. Add a successful script, failing script, and timed-out script; verify hidden failures and tooltip errors.
5. Check horizontal, vertical, and deskbar panels, then remove the item while a script runs and confirm no extension remains running.
6. Left-click readings and adjust both sliders in full and RAM-only views. Check outside-click/Escape dismissal, popup placement in each panel orientation, saved ranges after restart, and independent ranges on two instances. Changing a range must not enable control.
7. On supported hardware only, configure the narrow sudo permission yourself, enable the fan icon, and inspect its tooltip and actual fan state. Confirm it stays accessible in RAM-only mode. Disabling must request `level 0`; removing the item leaves the last level unchanged. Restore `level auto` manually when finished; this checklist does not authorize automated hardware changes.
8. Compare the bare level beside the fan icon with `level:` in `/proc/acpi/ibm/fan` over time in both views. Confirm it updates while smart control is off, remains visible in RAM-only mode, and shows `?` with a level-tooltip diagnostic if the file becomes unreadable. This observation is read-only; do not change the level for this check.

See `architecture.md` for the lifecycle and extension contracts.
