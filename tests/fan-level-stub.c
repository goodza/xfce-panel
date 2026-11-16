#include <glib.h>
#include "rust-monitor.h"

/* Values accepted by smoke_fan_level_set. */
#define SMOKE_LEVEL_AUTO 100
#define SMOKE_LEVEL_UNAVAILABLE 99

static gint calls;
static gint running;
static gint overlap;
static gint value;

/* Linked only into the isolated test module, never the installable plugin.
 * monitor_fan_level_read / monitor_fan_level_free are macro-renamed to these. */
MonitorFanLevel *
smoke_fan_level_read(void)
{
    if (g_atomic_int_add(&running, 1) != 0)
        g_atomic_int_set(&overlap, 1);
    g_atomic_int_add(&calls, 1);
    g_usleep(300000);
    gint mode = g_atomic_int_get(&value);
    MonitorFanLevel *result = g_new0(MonitorFanLevel, 1);
    if (mode == SMOKE_LEVEL_UNAVAILABLE) {
        result->text = g_strdup("?");
        result->tooltip = g_strdup("Fan level unavailable: simulated unreadable /proc/acpi/ibm/fan");
        result->failed = 1;
    } else if (mode == SMOKE_LEVEL_AUTO) {
        result->text = g_strdup("auto");
        result->tooltip = g_strdup("Current fan level: auto");
    } else {
        result->text = g_strdup_printf("%d", mode);
        result->tooltip = g_strdup_printf("Current fan level: %d", mode);
    }
    g_atomic_int_add(&running, -1);
    return result;
}

void
smoke_fan_level_set(gint new_value)
{
    g_atomic_int_set(&value, new_value);
}

void
smoke_fan_level_free(MonitorFanLevel *result)
{
    if (result != NULL) {
        g_free(result->text);
        g_free(result->tooltip);
        g_free(result);
    }
}

gint smoke_fan_level_calls(void) { return g_atomic_int_get(&calls); }
gint smoke_fan_level_running(void) { return g_atomic_int_get(&running); }
gint smoke_fan_level_overlap(void) { return g_atomic_int_get(&overlap); }