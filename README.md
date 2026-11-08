# Rust Monitor for XFCE4

Extensible Linux system metrics in the XFCE4 panel: CPU temperature, fan RPM, available RAM, and filesystem usage. Rust handles collection and scripts; a small C/GTK3 adapter integrates with XFCE.

```text
54.0 °C · 1240 RPM · 8.20 GiB · / 62%
```

Metrics appear inline without CPU, first-fan, or RAM labels; hover retains full labels and diagnostics. The second fan is tooltip-only (numbering follows sorted configured/discovered sensor paths). CPU temperature turns red strictly above 80°C and returns to normal at or below 80°C or when unavailable. The threshold uses the raw reading, so a value rounded to 80.0°C can still be red. Other metrics keep their normal color. Missing readings and failing extensions are omitted. Vertical/deskbar panels stack metrics. There is no Applications menu or settings dialog.

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

**Verification status:** Native compilation and shared-library loading pass on XFCE 4.20.7 / GTK 3.24.52. Rust checks, the C ABI smoke test, and an isolated GTK offscreen test pass. The native test checks compact readings, the hidden second fan's retained tooltip, raw CPU warning boundaries and reset, real RAM/disk metrics, literal text, config reload, orientation callbacks, and removal during a running extension. Staged installation also passes. Actual panel discovery, external-wrapper integration, and live panel orientation remain unverified; your panel configuration has not been changed.

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

See `architecture.md` for the lifecycle and extension contracts.
