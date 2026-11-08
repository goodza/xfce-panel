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

## Non-Goals

- Distribution-specific packaging.
