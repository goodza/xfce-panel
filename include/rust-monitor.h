#ifndef RUST_MONITOR_H
#define RUST_MONITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MonitorSnapshot {
    char *text;
    char *tooltip;
    /* Available RAM only, or Monitor when unavailable. Owned like text. */
    char *ram_text;
    uint32_t interval_seconds;
    /* 1 iff a CPU reading exists and exceeds 80 C; CPU is then the first metric. */
    uint32_t cpu_hot;
} MonitorSnapshot;

/* Thread-safe; no GTK calls. config_path is a borrowed NUL-terminated Unix path,
 * valid for this call only; NULL selects the XDG default. Reloaded on every call.
 * Returns an owned snapshot, or NULL on a contained Rust panic. Text is UTF-8,
 * plain text (not markup); inline metrics are separated by " · ".
 * Do not modify/free fields. Call monitor_snapshot_free exactly once. */
MonitorSnapshot *monitor_collect(const char *config_path);

/* Frees the snapshot and all three strings. NULL is allowed. No pointers into the
 * snapshot remain valid after this call. */
void monitor_snapshot_free(MonitorSnapshot *snapshot);

typedef struct MonitorFanResult {
    char *status;    /* owned plain-text status; never NULL */
    uint32_t failed; /* 1 if the check failed (error status), else 0 */
} MonitorFanResult;

/* One blocking smart-fan check; call off the GTK thread. Reloads shared CPU
 * configuration, reads ThinkPad RPM and may write through noninteractive sudo.
 * Integer thresholds require 30 <= off_celsius < auto_celsius <= 90.
 * Invalid ranges attempt automatic control and set failed=1.
 * Returns an owned result, or NULL on a contained panic.
 * Rebuild Rust and the adapter together when this signature changes. */
MonitorFanResult *monitor_fan_step(uint32_t off_celsius, uint32_t auto_celsius);

/* Release a result from monitor_fan_step and its status, exactly once. NULL allowed. */
void monitor_fan_result_free(MonitorFanResult *result);

typedef struct MonitorFanLevel {
    char *text;    /* owned display level: "0".."7", "auto", or "?" */
    char *tooltip; /* owned plain-text detail; describes unavailability */
    uint32_t failed; /* 1 when the display level is "?", else 0 */
} MonitorFanLevel;

/* One blocking, read-only observation of the actual ThinkPad fan level from
 * /proc/acpi/ibm/fan; call off the GTK thread. Never writes hardware, runs a
 * command, or consults smart-control state. Invalid, ambiguous, unsupported, or
 * unreadable output yields text "?" with a diagnostic tooltip and failed=1.
 * Returns an owned result, or NULL on a contained panic. Rebuild Rust and the
 * adapter together when this signature changes. */
MonitorFanLevel *monitor_fan_level_read(void);

/* Release a result from monitor_fan_level_read and both strings, exactly once.
 * NULL allowed. */
void monitor_fan_level_free(MonitorFanLevel *level);

#ifdef __cplusplus
}
#endif
#endif
