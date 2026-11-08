---
id: xfce-monitor-architecture
type: architecture-design
status: active
title: Rust Monitor architecture and contracts
parent: xfce-monitor-goals
---

# Rust Monitor architecture

## Boundary

Rust owns Linux collection, TOML configuration, extension lifecycle, formatting, and a diagnostic CLI. A thin GTK3 C adapter owns XFCE API 2.0 registration and widget lifecycle. The adapter preserves XFCE's registration macros instead of maintaining custom Rust bindings. The core is usable and testable without GTK development packages.

## Display and collection

Only metrics belong to the widget. Horizontal panels show inline text; vertical and deskbar panels stack the same metric text. CPU temperature, first fan, and available RAM display their values and units without CPU, Fan 1, RAM, or available labels. The second fan by sorted sensor-path index is tooltip-only, even if the first fan is unreadable; later fan entries retain their existing labels. Tooltips retain all metric labels and readable fans. Tooltip text is plain text, never markup. Missing readings and failed scripts are hidden from the panel and described in the tooltip; an empty snapshot displays Monitor so diagnostics remain accessible.

Ordinary right-button presses toggle full display and RAM-only immediately from cached snapshots, independently per plugin instance. Full is the initial state; no selection is persisted. A child event box consumes the ordinary press before parent panel menu handling; Ctrl+right-click and other buttons propagate normally. Additional non-Ctrl double/triple button-3 events are consumed without toggling, since XFCE's parent handler would otherwise open its menu during rapid toggles. Polling and full tooltip details continue when minimized. RAM-only text is supplied explicitly by Rust, not parsed from display strings; absent memory uses Monitor. Minimized text is never CPU-colored; returning full restores the current snapshot's warning and layout.

CPU discovery uses the hottest tempN_input from coretemp, k10temp, zenpower, or cpu_thermal drivers, avoiding unrelated storage temperatures. Explicit sensor paths override discovery. Fan discovery covers all fanN_input files; zero RPM is a real reading. RAM means MemAvailable. Filesystem occupancy uses used/(used+available), reflecting unprivileged capacity rather than reserved blocks; / is the default and paths are configurable.

## Configuration contract

Instances share XDG_CONFIG_HOME/xfce4/panel/rust-monitor.toml, falling back to HOME/.config when XDG_CONFIG_HOME is missing or relative. Each collection reloads it, avoiding a settings dialog. Missing configuration uses defaults; invalid configuration rejects the whole file, uses defaults, and exposes a diagnostic. Poll delay defaults to two seconds and ranges from 1 to 3600 seconds.

## Extension contract

Trusted local executables run as argv arrays without an implicit shell. First stdout line is a nonempty value; optional second line is tooltip detail. Extra lines are ignored. Text must be UTF-8 without NUL/control characters except LF/tab. Combined stdout/stderr is capped at 4096 bytes; default timeout is 1000 ms, configurable from 10 to 10000 ms. At most 16 extensions run sequentially. Failures hide their metric and surface bounded diagnostics.

Nonblocking pipes prevent descendants holding pipes open from stalling collection. On success, failure, overflow, or timeout the process group is killed and the direct child reaped. This is cleanup, not sandboxing: deliberately detached descendants may escape it. Extensions must not daemonize.

## Native lifecycle and ownership

monitor_collect returns an owned C-compatible snapshot with full text, tooltip, and ram_text strings; monitor_snapshot_free releases it and all three strings exactly once. NULL config selects the shared default file; panics are contained at the ABI and reported by NULL. The snapshot's cpu_hot flag is 1 only for an available raw CPU temperature strictly above 80000 millidegrees Celsius, independent of display rounding; when set, CPU is the first metric. Rust and the native adapter must be built together against the current snapshot layout. C applies a red Pango foreground attribute only to that first metric's UTF-8 byte range in either layout. It replaces the attributes on every repaint, resetting color at or below 80°C, on missing CPU, or on collection failure. Other metrics retain normal color. This structured flag avoids reparsing rounded text; Pango attributes preserve plain-text label APIs, so scripts cannot inject markup.

The GTK main loop launches one GTask at a time and schedules the next one-shot timer after completion. Its interval is a post-collection delay, not a wall-clock guarantee. A task holds state and plugin references; removal marks state removed, removes the timer, and prevents later widget access. Teardown waits on a condition signaled by the worker after Rust collection returns, independent of GTK callback dispatch. Retaining objects or mapping the module does not keep XFCE's external wrapper process alive: waiting is necessary to finish script cleanup before wrapper exit. Sequential timeout budgets may delay removal by up to 160 seconds plus collection overhead. Kernel filesystem/device I/O is not bounded.

Rust is linked PIC into the external module. Linux nodelete linking keeps worker/callback code and Rust TLS mapped if the module is closed while references remain. No system installation or panel restart is performed during development automatically.

## Verification boundary

Rust fixtures, subprocess failure tests, live CLI collection, release static-library builds, and a linked C ABI smoke test exercise the core. Native compilation and dynamic loading are verified on XFCE 4.20.7 / GTK 3.24.52. An isolated GTK offscreen harness invokes the exported plugin factory and tests asynchronous readings, literal text, config reload, injected mode/orientation callbacks, and teardown during an extension timeout; its descendant must not write a delayed marker after removal. The harness isolates XDG configuration before GTK initialization and uses deadlines plus a global watchdog. Staged installation is verified. Actual panel discovery, the external wrapper process, and live-panel orientation remain unverified: offscreen callback injection does not exercise those integration paths. README.md contains the remaining manual native verification checklist.
