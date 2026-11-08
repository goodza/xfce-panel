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

Only metrics belong to the widget. Horizontal panels show inline text; vertical and deskbar panels stack the same metric text. Tooltip text is plain text, never markup. Missing readings and failed scripts are hidden from the panel and described in the tooltip; an empty snapshot displays Monitor so diagnostics remain accessible.

CPU discovery uses the hottest tempN_input from coretemp, k10temp, zenpower, or cpu_thermal drivers, avoiding unrelated storage temperatures. Explicit sensor paths override discovery. Fan discovery covers all fanN_input files; zero RPM is a real reading. RAM means MemAvailable. Filesystem occupancy uses used/(used+available), reflecting unprivileged capacity rather than reserved blocks; / is the default and paths are configurable.

## Configuration contract

Instances share XDG_CONFIG_HOME/xfce4/panel/rust-monitor.toml, falling back to HOME/.config when XDG_CONFIG_HOME is missing or relative. Each collection reloads it, avoiding a settings dialog. Missing configuration uses defaults; invalid configuration rejects the whole file, uses defaults, and exposes a diagnostic. Poll delay defaults to two seconds and ranges from 1 to 3600 seconds.

## Extension contract

Trusted local executables run as argv arrays without an implicit shell. First stdout line is a nonempty value; optional second line is tooltip detail. Extra lines are ignored. Text must be UTF-8 without NUL/control characters except LF/tab. Combined stdout/stderr is capped at 4096 bytes; default timeout is 1000 ms, configurable from 10 to 10000 ms. At most 16 extensions run sequentially. Failures hide their metric and surface bounded diagnostics.

Nonblocking pipes prevent descendants holding pipes open from stalling collection. On success, failure, overflow, or timeout the process group is killed and the direct child reaped. This is cleanup, not sandboxing: deliberately detached descendants may escape it. Extensions must not daemonize.

## Native lifecycle and ownership

monitor_collect returns an owned C-compatible snapshot; monitor_snapshot_free releases it exactly once. NULL config selects the shared default file; panics are contained at the ABI and reported by NULL. C uses plain text APIs, so script output cannot become GTK markup.

The GTK main loop launches one GTask at a time and schedules the next one-shot timer after completion. Its interval is a post-collection delay, not a wall-clock guarantee. A task holds state and plugin references; removal marks state removed, removes the timer, and prevents later widget access. Teardown waits on a condition signaled by the worker after Rust collection returns, independent of GTK callback dispatch. Retaining objects or mapping the module does not keep XFCE's external wrapper process alive: waiting is necessary to finish script cleanup before wrapper exit. Sequential timeout budgets may delay removal by up to 160 seconds plus collection overhead. Kernel filesystem/device I/O is not bounded.

Rust is linked PIC into the external module. Linux nodelete linking keeps worker/callback code and Rust TLS mapped if the module is closed while references remain. No system installation or panel restart is performed during development automatically.

## Verification boundary

Rust fixtures, subprocess failure tests, live CLI collection, release static-library builds, and a linked C ABI smoke test exercise the core. GTK/XFCE development headers are absent in the development environment: native compilation and live-panel rendering, discovery, and removal remain unverified. README.md contains the manual native verification checklist.
