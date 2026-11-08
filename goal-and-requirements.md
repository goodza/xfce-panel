---
id: xfce-monitor-goals
type: goal-and-requirements
status: active
title: Extensible XFCE4 Panel Monitor
---

# Extensible XFCE4 Panel Monitor

A lightweight personal Linux system monitor inside the XFCE4 panel.

## Overview

Show essential machine health and capacity at a glance, with script-based extensions that do not require rebuilding the plugin.

## Problem

Checking CPU temperature, fan speed, memory headroom, and disk capacity should not require opening separate monitoring tools.

## Capabilities

- Display CPU temperature and fan speed in RPM.
- Display available RAM.
- Display filesystem space usage, rather than disk I/O throughput.
- Add metrics through executable scripts returning metric values.
- Provide build, installation, and configuration instructions for personal use.
- Show metrics inline with hover details; omit missing readings rather than displaying N/A.
- Toggle between the full display and RAM-only by right-click, independently per running instance; Ctrl+right-click retains the panel menu.
- Reload a shared local configuration file for sensor selection, filesystem paths, script extensions, and refresh delay.

## Tech Notes

A Rust monitoring core integrates with XFCE4 through a thin C/GTK3 panel adapter. A diagnostic CLI uses the same core without requiring GTK development headers. See xfce-monitor-architecture for contracts and lifecycle decisions.

## Non-Goals

- Distribution-specific packaging.
