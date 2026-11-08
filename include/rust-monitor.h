#ifndef RUST_MONITOR_H
#define RUST_MONITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MonitorSnapshot {
    char *text;
    char *tooltip;
    uint32_t interval_seconds;
} MonitorSnapshot;

/* Thread-safe; no GTK calls. config_path is a borrowed NUL-terminated Unix path,
 * valid for this call only; NULL selects the XDG default. Reloaded on every call.
 * Returns an owned snapshot, or NULL on a contained Rust panic. Text is UTF-8,
 * plain text (not markup); inline metrics are separated by " · ".
 * Do not modify/free fields. Call monitor_snapshot_free exactly once. */
MonitorSnapshot *monitor_collect(const char *config_path);

/* Frees the snapshot and both strings. NULL is allowed. No pointers into the
 * snapshot remain valid after this call. */
void monitor_snapshot_free(MonitorSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
#endif
