#include <glib.h>
#include "rust-monitor.h"

static gint calls;
static gint disable_calls;
static gint running;
static gint overlap;
static gint last_off;
static gint last_auto;
static gint force_fail;
static gint force_disable_fail;

/* Linked only into the isolated test module, never the installable plugin.
 * monitor_fan_step / monitor_fan_disable / monitor_fan_result_free are
 * macro-renamed to these. */
MonitorFanResult *
smoke_fan_step(guint32 off_celsius, guint32 auto_celsius)
{
    g_assert_cmpuint(off_celsius, >=, 30);
    g_assert_cmpuint(off_celsius, <, auto_celsius);
    g_assert_cmpuint(auto_celsius, <=, 90);
    if (g_atomic_int_add(&running, 1) != 0)
        g_atomic_int_set(&overlap, 1);
    g_atomic_int_set(&last_off, (gint) off_celsius);
    g_atomic_int_set(&last_auto, (gint) auto_celsius);
    gint call = g_atomic_int_add(&calls, 1) + 1;
    g_usleep(200000);
    g_atomic_int_add(&running, -1);
    MonitorFanResult *res = g_new0(MonitorFanResult, 1);
    if (call == 1 || g_atomic_int_get(&force_fail) != 0) {
        res->status = g_strdup("Smart fan control on: simulated permission failure; retrying");
        res->failed = 1;
    } else {
        res->status = g_strdup("Smart fan control on: level auto (simulated)");
        res->failed = 0;
    }
    return res;
}

MonitorFanResult *
smoke_fan_disable(void)
{
    if (g_atomic_int_add(&running, 1) != 0)
        g_atomic_int_set(&overlap, 1);
    g_atomic_int_add(&disable_calls, 1);
    g_usleep(200000);
    g_atomic_int_add(&running, -1);
    MonitorFanResult *res = g_new0(MonitorFanResult, 1);
    if (g_atomic_int_get(&force_disable_fail) != 0) {
        res->status = g_strdup("Smart fan control off: setting level 0 failed: simulated denial");
        res->failed = 1;
    } else {
        res->status = g_strdup("Smart fan control off: level 0 set (simulated)");
        res->failed = 0;
    }
    return res;
}

void
smoke_fan_set_fail(gint value)
{
    g_atomic_int_set(&force_fail, value);
}

void
smoke_fan_set_disable_fail(gint value)
{
    g_atomic_int_set(&force_disable_fail, value);
}

void
smoke_fan_result_free(MonitorFanResult *res)
{
    if (res != NULL) {
        g_free(res->status);
        g_free(res);
    }
}

gint smoke_fan_calls(void) { return g_atomic_int_get(&calls); }
gint smoke_fan_disable_calls(void) { return g_atomic_int_get(&disable_calls); }
gint smoke_fan_running(void) { return g_atomic_int_get(&running); }
gint smoke_fan_overlap(void) { return g_atomic_int_get(&overlap); }
gint smoke_fan_off(void) { return g_atomic_int_get(&last_off); }
gint smoke_fan_auto(void) { return g_atomic_int_get(&last_auto); }
