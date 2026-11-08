#include <gio/gio.h>
#include <libxfce4panel/libxfce4panel.h>

#include "rust-monitor.h"

typedef struct {
    gint refs;
    XfcePanelPlugin *plugin; /* borrowed; owned by the task while collecting */
    GtkWidget *label;
    gchar *text;
    guint timeout_id;
    gboolean in_flight;
    gboolean removed;
    GMutex worker_mutex;
    GCond worker_done;
    gboolean worker_running;
} MonitorState;

static void start_refresh(MonitorState *state);

static MonitorState *
state_ref(MonitorState *state)
{
    g_atomic_int_inc(&state->refs);
    return state;
}

static void
state_unref(gpointer data)
{
    MonitorState *state = data;

    if (g_atomic_int_dec_and_test(&state->refs)) {
        g_free(state->text);
        g_cond_clear(&state->worker_done);
        g_mutex_clear(&state->worker_mutex);
        g_free(state);
    }
}

static void
stop_refresh(MonitorState *state)
{
    state->removed = TRUE;
    state->label = NULL;
    if (state->timeout_id != 0) {
        g_source_remove(state->timeout_id);
        state->timeout_id = 0;
    }
    g_mutex_lock(&state->worker_mutex);
    while (state->worker_running)
        g_cond_wait(&state->worker_done, &state->worker_mutex);
    g_mutex_unlock(&state->worker_mutex);
}

static void
state_destroy(gpointer data)
{
    MonitorState *state = data;

    stop_refresh(state);
    state_unref(state);
}

static void
removed(XfcePanelPlugin *plugin, MonitorState *state)
{
    (void) plugin;
    stop_refresh(state);
}

static void
repaint(MonitorState *state, gboolean stacked)
{
    gchar **parts;
    gchar *text;

    if (state->removed)
        return;

    if (stacked) {
        parts = g_strsplit(state->text, " · ", -1);
        text = g_strjoinv("\n", parts);
        gtk_label_set_text(GTK_LABEL(state->label), text);
        g_free(text);
        g_strfreev(parts);
    } else {
        gtk_label_set_text(GTK_LABEL(state->label), state->text);
    }
}

static void
mode_changed(XfcePanelPlugin *plugin, XfcePanelPluginMode mode,
             MonitorState *state)
{
    if (state->removed)
        return;
    xfce_panel_plugin_set_small(plugin, mode == XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
    repaint(state, mode != XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
}

static void
orientation_changed(XfcePanelPlugin *plugin, GtkOrientation orientation,
                    MonitorState *state)
{
    repaint(state, orientation == GTK_ORIENTATION_VERTICAL ||
                   xfce_panel_plugin_get_mode(plugin) != XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
}

static void
snapshot_free(gpointer data)
{
    monitor_snapshot_free(data);
}

static void
collect_worker(GTask *task, gpointer source_object, gpointer task_data,
               GCancellable *cancellable)
{
    MonitorState *state = task_data;
    MonitorSnapshot *snapshot;

    (void) source_object;
    (void) cancellable;
    snapshot = monitor_collect(NULL);
    g_mutex_lock(&state->worker_mutex);
    state->worker_running = FALSE;
    g_cond_broadcast(&state->worker_done);
    g_mutex_unlock(&state->worker_mutex);
    g_task_return_pointer(task, snapshot, snapshot_free);
}

static gboolean
refresh_timeout(gpointer data)
{
    MonitorState *state = data;

    state->timeout_id = 0;
    start_refresh(state);
    return G_SOURCE_REMOVE;
}

static void
collect_done(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    GTask *task = G_TASK(result);
    MonitorState *state = g_task_get_task_data(task);
    MonitorSnapshot *snapshot = g_task_propagate_pointer(task, NULL);
    guint interval = 2;

    (void) source_object;
    (void) user_data;
    state->in_flight = FALSE;
    if (!state->removed) {
        if (snapshot != NULL) {
            interval = MAX(1u, snapshot->interval_seconds);
            g_free(state->text);
            state->text = g_strdup(snapshot->text);
            gtk_widget_set_tooltip_text(state->label, snapshot->tooltip);
        } else {
            g_free(state->text);
            state->text = g_strdup("Monitor");
            gtk_widget_set_tooltip_text(state->label, "Monitor collection failed");
        }
        repaint(state, xfce_panel_plugin_get_mode(state->plugin) !=
                       XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
        state->timeout_id = g_timeout_add_seconds_full(
            G_PRIORITY_DEFAULT, interval, refresh_timeout,
            state_ref(state), state_unref);
    }
    monitor_snapshot_free(snapshot);
}

static void
start_refresh(MonitorState *state)
{
    GTask *task;

    if (state->removed || state->in_flight)
        return;
    state->in_flight = TRUE;
    g_mutex_lock(&state->worker_mutex);
    state->worker_running = TRUE;
    g_mutex_unlock(&state->worker_mutex);
    task = g_task_new(state->plugin, NULL, collect_done, NULL);
    g_task_set_task_data(task, state_ref(state), state_unref);
    g_task_run_in_thread(task, collect_worker);
    g_object_unref(task);
}

static void
monitor_construct(XfcePanelPlugin *plugin)
{
    MonitorState *state = g_new0(MonitorState, 1);

    state->refs = 1;
    g_mutex_init(&state->worker_mutex);
    g_cond_init(&state->worker_done);
    state->plugin = plugin;
    state->text = g_strdup("Monitor…");
    state->label = gtk_label_new(state->text);
    gtk_container_add(GTK_CONTAINER(plugin), state->label);
    g_object_set_data_full(G_OBJECT(plugin), "rust-monitor-state", state,
                           state_destroy);
    g_signal_connect(plugin, "free-data", G_CALLBACK(removed), state);
    g_signal_connect(plugin, "destroy", G_CALLBACK(removed), state);
    g_signal_connect(plugin, "removed", G_CALLBACK(removed), state);
    g_signal_connect(plugin, "mode-changed", G_CALLBACK(mode_changed), state);
    g_signal_connect(plugin, "orientation-changed",
                     G_CALLBACK(orientation_changed), state);
    xfce_panel_plugin_set_expand(plugin, FALSE);
    mode_changed(plugin, xfce_panel_plugin_get_mode(plugin), state);
    gtk_widget_show(state->label);
    start_refresh(state);
}

XFCE_PANEL_PLUGIN_REGISTER(monitor_construct)
