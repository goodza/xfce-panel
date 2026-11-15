#include <glib.h>

static gint calls;
static gint running;
static gint overlap;

/* Linked only into the isolated test module, never the installable plugin. */
char *
smoke_fan_step(guint32 off_celsius, guint32 auto_celsius)
{
    g_assert_cmpuint(off_celsius, >=, 30);
    g_assert_cmpuint(off_celsius, <, auto_celsius);
    g_assert_cmpuint(auto_celsius, <=, 90);
    if (g_atomic_int_add(&running, 1) != 0)
        g_atomic_int_set(&overlap, 1);
    gint call = g_atomic_int_add(&calls, 1) + 1;
    g_usleep(200000);
    g_atomic_int_add(&running, -1);
    return g_strdup(call == 1 ?
        "Smart fan control on: simulated permission failure; retrying" :
        "Smart fan control on: level auto (simulated)");
}

void
smoke_fan_status_free(char *status)
{
    g_free(status);
}

gint smoke_fan_calls(void) { return g_atomic_int_get(&calls); }
gint smoke_fan_running(void) { return g_atomic_int_get(&running); }
gint smoke_fan_overlap(void) { return g_atomic_int_get(&overlap); }
