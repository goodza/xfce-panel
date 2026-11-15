---
id: xfce-monitor-architecture
type: architecture-design
status: active
title: Rust Monitor architecture and contracts
parent: xfce-monitor-goals
---

# Rust Monitor architecture

## Boundary

Rust owns Linux collection, TOML configuration, extension lifecycle, formatting, ThinkPad fan-control decisions/commands, and a diagnostic CLI. A thin GTK3 C adapter owns XFCE API 2.0 registration and widget lifecycle. The adapter preserves XFCE's registration macros instead of maintaining custom Rust bindings. The core is usable and testable without GTK development packages.

## Display and collection

The widget contains metrics and a smart-fan icon toggle. Horizontal panels show inline text beside the toggle; vertical and deskbar panels stack metrics and the toggle. CPU temperature, first fan, and available RAM display their values and units without CPU, Fan 1, RAM, or available labels. The second fan by sorted sensor-path index is tooltip-only, even if the first fan is unreadable; later fan entries retain their existing labels. Tooltips retain all metric labels and readable fans. Tooltip text is plain text, never markup. Missing readings and failed scripts are hidden from the panel and described in the tooltip; an empty snapshot displays Monitor so diagnostics remain accessible.

Ordinary right-button presses on metrics toggle full display and RAM-only immediately from cached snapshots, independently per plugin instance. Full is the initial state; no selection is persisted. A child event box consumes the ordinary press before parent panel menu handling; Ctrl+right-click and other buttons propagate normally. Additional non-Ctrl double/triple button-3 events are consumed without toggling, since XFCE's parent handler would otherwise open its menu during rapid toggles. Polling and full tooltip details continue when minimized. RAM-only text is supplied explicitly by Rust, not parsed from display strings; absent memory uses Monitor. Minimized text is never CPU-colored; returning full restores the current snapshot's warning and layout.

CPU discovery uses the hottest tempN_input from coretemp, k10temp, zenpower, or cpu_thermal drivers, avoiding unrelated storage temperatures. Explicit sensor paths override discovery. Fan discovery covers all fanN_input files; zero RPM is a real reading. RAM means MemAvailable. Filesystem occupancy uses used/(used+available), reflecting unprivileged capacity rather than reserved blocks; / is the default and paths are configurable.

## Smart fan control

The icon toggle is opt-in, starts off per instance, is not persisted, and remains visible in RAM-only mode. Disabling or removing the plugin leaves the last fan level unchanged, including level 0: this is an explicit user choice, not a safe restoration guarantee. An already-started check may finish after disabling. Multiple instances are independent and do not coordinate access to the shared hardware; only one controller should be enabled.

Control uses the configured CPU sensor or the hottest recognized CPU reading, and reads `speed:` from `/proc/acpi/ibm/fan`, not the first displayed hwmon fan. This matches the RPM condition to the fan receiving commands. Raw CPU >60000 millidegrees requests `level auto`; CPU <58000 AND RPM <2000 requests `level 0`; all other cases preserve the level. Zero RPM is valid. Missing/invalid readings (including any discovered CPU read failure or negative CPU temperature), invalid configuration, and command errors attempt `level auto`, report the error and fallback outcome in the toggle tooltip, and retry while enabled. Metric collection still uses its existing tolerant missing-reading/default-config behavior.

Commands use fixed `/usr/bin/sudo -n /usr/bin/tee /proc/acpi/ibm/fan` argv and at most eleven bytes of fixed stdin, without a shell. They reuse the extension subprocess bounds/cleanup with a one-second timeout per attempt, including fallback. Password prompting is forbidden; permissions and kernel support are administrator-managed. No helper, sudoers rule, or kernel configuration is installed automatically. Neither CLI collection nor ordinary metric collection performs control writes.

A separate one-at-a-time GTask checks immediately on enable and two seconds after each completion, independent of metric refresh and slow extensions. Generation tracking discards stale UI results after off/on transitions; disabling cancels the timer. Removal waits for both workers without requiring main-loop completion callbacks, then prevents future scheduling. Filesystem/device I/O remains unbounded; command failure plus fallback adds up to two seconds of subprocess timeout budget. The adapter draws a theme-colored fan glyph itself to avoid a missing icon on themes without a fan asset.

## Configuration contract

Instances share XDG_CONFIG_HOME/xfce4/panel/rust-monitor.toml, falling back to HOME/.config when XDG_CONFIG_HOME is missing or relative. Each collection reloads it, avoiding a settings dialog. Missing configuration uses defaults; invalid configuration rejects the whole file, uses defaults, and exposes a diagnostic. Poll delay defaults to two seconds and ranges from 1 to 3600 seconds.

## Extension contract

Trusted local executables run as argv arrays without an implicit shell. First stdout line is a nonempty value; optional second line is tooltip detail. Extra lines are ignored. Text must be UTF-8 without NUL/control characters except LF/tab. Combined stdout/stderr is capped at 4096 bytes; default timeout is 1000 ms, configurable from 10 to 10000 ms. At most 16 extensions run sequentially. Failures hide their metric and surface bounded diagnostics.

Nonblocking pipes prevent descendants holding pipes open from stalling collection. On success, failure, overflow, or timeout the process group is killed and the direct child reaped. This is cleanup, not sandboxing: deliberately detached descendants may escape it. Extensions must not daemonize.

## Native lifecycle and ownership

monitor_collect returns an owned C-compatible snapshot with full text, tooltip, and ram_text strings; monitor_snapshot_free releases it and all three strings exactly once. NULL config selects the shared default file; panics are contained at the ABI and reported by NULL. The snapshot's cpu_hot flag is 1 only for an available raw CPU temperature strictly above 80000 millidegrees Celsius, independent of display rounding; when set, CPU is the first metric. Rust and the native adapter must be built together against the current snapshot layout. C applies a red Pango foreground attribute only to that first metric's UTF-8 byte range in either layout. It replaces the attributes on every repaint, resetting color at or below 80°C, on missing CPU, or on collection failure. Other metrics retain normal color. This structured flag avoids reparsing rounded text; Pango attributes preserve plain-text label APIs, so scripts cannot inject markup.

monitor_fan_step returns a separately owned status string released exactly once with monitor_fan_status_free; NULL represents a contained panic and the adapter reports a failed check before retrying.

The GTK main loop launches one metric GTask at a time and schedules the next one-shot timer after completion. Its interval is a post-collection delay, not a wall-clock guarantee. A task holds state and plugin references; removal marks state removed, removes the timer, and prevents later widget access. Teardown waits on a condition signaled by the worker after Rust collection returns, independent of GTK callback dispatch. Retaining objects or mapping the module does not keep XFCE's external wrapper process alive: waiting is necessary to finish script cleanup before wrapper exit. Sequential timeout budgets may delay removal by up to 160 seconds plus collection overhead. Kernel filesystem/device I/O is not bounded.

Rust is linked PIC into the external module. Linux nodelete linking keeps worker/callback code and Rust TLS mapped if the module is closed while references remain. No system installation or panel restart is performed during development automatically.

## Verification boundary

Rust fixtures, subprocess failure tests, live CLI collection, release static-library builds, and a linked C ABI smoke test exercise the core. Native compilation and dynamic loading are verified on XFCE 4.20.7 / GTK 3.24.52. An isolated GTK offscreen harness invokes the exported plugin factory and tests asynchronous readings, literal text, config reload, injected mode/orientation callbacks, and teardown during an extension timeout; its descendant must not write a delayed marker after removal. The harness isolates XDG configuration before GTK initialization and uses deadlines plus a global watchdog. A separately linked test-only fan backend verifies toggle defaults/visibility, asynchronous enable, retries during slow metrics and long refresh delays, rapid off/on serialization, stale-result suppression, and worker teardown. It never invokes sudo or changes fan hardware; Rust fan tests use temporary files and injected command writers. Staged installation is verified. Real fan writes, sudo authorization, actual panel discovery, the external wrapper process, and live-panel orientation remain unverified: offscreen callback injection does not exercise those integration paths. README.md contains the remaining manual native verification checklist.
