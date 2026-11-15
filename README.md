# Rust Monitor for XFCE4

Extensible Linux system metrics in the XFCE4 panel: CPU temperature, fan RPM, available RAM, and filesystem usage. Rust handles collection and scripts; a small C/GTK3 adapter integrates with XFCE.

```text
54.0 °C · 1240 RPM · 8.20 GiB · / 62%
```

Metrics appear inline without CPU, first-fan, or RAM labels; hover retains full labels and diagnostics. The second fan is tooltip-only (numbering follows sorted configured/discovered sensor paths). CPU temperature turns red strictly above 80°C and returns to normal at or below 80°C or when unavailable. The threshold uses the raw reading, so a value rounded to 80.0°C can still be red. Other metrics keep their normal color. Missing readings and failing extensions are omitted. Vertical/deskbar panels stack metrics. Right-click toggles full display ↔ RAM-only; Ctrl+right-click passes through to XFCE's plugin menu. Each instance starts full and remembers the selection only until it is removed or the panel restarts. Hover details and polling continue in either view. RAM-only mode never inherits the CPU warning color; returning full restores the current warning. Until RAM is available, minimized mode shows Monitor with diagnostics. There is no Applications menu or settings dialog. A separate **fan icon toggle** enables optional ThinkPad smart fan control and remains visible in RAM-only mode; see [Smart fan control](#smart-fan-control) before enabling it.

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

Requires XFCE4 panel API 2.0, GTK3, GIO, Cargo, and a Linux ELF linker supporting `-z nodelete`. `make` builds a PIC Rust release library, CLI, and `target/native/panel/librust-monitor.so`. Custom `CARGO_TARGET_DIR` is supported. Cargo may download crates on the first build.

**Verification status:** Native compilation and shared-library loading pass on XFCE 4.20.7 / GTK 3.24.52. Rust checks, the C ABI smoke test, and an isolated GTK offscreen test pass. The native test checks compact readings, the hidden second fan's retained tooltip, raw CPU warning boundaries and reset, real RAM/disk metrics, literal text, right-click toggles and modifier propagation, polling/reload while minimized, orientation callbacks, and removal during a running extension. A second test module uses a simulated fan backend to verify nonblocking toggles, independent polling, failure/retry, rapid off/on transitions, and teardown without sudo or hardware writes. Staged installation also passes. Real fan writes, sudo authorization, actual panel discovery, external-wrapper integration, and live panel orientation remain unverified; your panel configuration has not been changed.

```sh
make test-native
# For a headless environment with Xvfb installed:
# xvfb-run -a make test-native
```

The native test requires a working GTK display even though it uses an offscreen window. It uses temporary configuration, never your real config; missing display is a failure, not a skipped pass.

## Install into XFCE's plugin directories

Use the library directory of your installed panel, especially on multiarch systems. Do not assume `/usr/local/lib` is searched by your distribution's panel.

```sh
PANEL_LIBDIR="$(pkg-config --variable=libdir libxfce4panel-2.0)"
make PREFIX=/usr LIBDIR="$PANEL_LIBDIR"
sudo make install PREFIX=/usr LIBDIR="$PANEL_LIBDIR"
```

Then open **Panel → Add New Items → Rust Monitor**. If not listed, restart the panel yourself with `xfce4-panel -r` and reopen the item list. Installation does not add an item or restart your panel automatically. Multiple instances share one configuration file.

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

## Smart fan control

Click the fan icon to enable or disable control. It starts **off** each time the plugin starts, without saving its state. The pressed icon indicates enabled control; hover over it for the latest action or error. Metric right-click behavior is unchanged.

While enabled, a separate background check runs immediately, then two seconds after each check completes, independently of metric scripts and `interval_seconds`:

- CPU **>80°C** → write `level auto`.
- CPU **<58°C** **and** ThinkPad fan speed **<2000 RPM** → write `level 0`.
- Otherwise leave the level unchanged. Bounds are strict: exactly 80°C, 58°C, or 2000 RPM does not trigger the corresponding transition.

CPU selection follows `cpu_sensor` or normal hottest-CPU discovery. Control RPM comes from `speed:` in `/proc/acpi/ibm/fan`, regardless of `fan_sensors` or the displayed fan metric. Missing/invalid readings, invalid configuration, or command failures attempt automatic control, report the failure/fallback result in the icon tooltip, and keep retrying while enabled. Each command has a one-second timeout.

**Important:** Switching off or removing the plugin **does not restore automatic control**; the last level, possibly `level 0`, is left unchanged. A check already started may finish after switching off. This plugin is not a fail-safe cooling service. Enable only one controller; do not run it alongside competing fan-management tools. To restore automatic control yourself:

```sh
printf 'level auto\n' | sudo /usr/bin/tee /proc/acpi/ibm/fan
```

### Required permissions and hardware

This feature requires a supported ThinkPad with a readable/writable `/proc/acpi/ibm/fan` and manual fan control enabled in the kernel driver. No modules or kernel settings are changed by the plugin. Confirm support for your model before using fan-off mode.

The plugin executes `/usr/bin/sudo -n /usr/bin/tee /proc/acpi/ibm/fan` with fixed stdin (`level auto\n` or `level 0\n`), not a shell. `-n` prevents password prompts. An administrator must configure permission separately. For example, use `sudo visudo -f /etc/sudoers.d/rust-monitor-fan` and replace `youruser` in this narrowly scoped rule:

```sudoers
youruser ALL=(root) NOPASSWD: /usr/bin/tee /proc/acpi/ibm/fan
```

This grants that user control over the fan interface; do not grant unrestricted `tee`, shell, or full sudo access for this feature. Fixed executable paths must exist. Without permission or driver support, the icon tooltip reports errors rather than prompting. Tests do not verify real sudo authorization or hardware writes. The CLI and ordinary metric collection never change fan levels.

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

1. Run `make test-native`, then install into the panel's actual directories.
2. Add Rust Monitor; check inline readings and plain-text hover details.
3. Edit the config and verify reload, invalid-config diagnostics, explicit sensors, and additional filesystems.
4. Add a successful script, failing script, and timed-out script; verify hidden failures and tooltip errors.
5. Check horizontal, vertical, and deskbar panels, then remove the item while a script runs and confirm no extension remains running.
6. On supported hardware only, configure the narrow sudo permission yourself, enable the fan icon, and inspect its tooltip and actual fan state. Confirm it stays accessible in RAM-only mode. After disabling/removing it, restore `level auto` manually; this checklist does not authorize automated hardware changes.

See `architecture.md` for the lifecycle and extension contracts.
