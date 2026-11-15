#include <gio/gio.h>
#include <libxfce4panel/libxfce4panel.h>
#include <string.h>

#include "rust-monitor.h"

typedef struct {
    gint refs;
    XfcePanelPlugin *plugin; /* borrowed; owned by the task while collecting */
    GtkWidget *label;
    GtkWidget *content;
    GtkWidget *fan_button;
    gboolean fan_enabled;
    gboolean fan_in_flight;
    gboolean fan_worker_running;
    guint fan_timeout_id;
    guint fan_generation;
    gchar *text;
    gchar *ram_text;
    gboolean minimized;
    guint timeout_id;
    gboolean in_flight;
    gboolean removed;
    gboolean cpu_hot;
    GMutex worker_mutex;
    GCond worker_done;
    gboolean worker_running;
} MonitorState;

static void start_refresh(MonitorState *state);
static void start_fan_refresh(MonitorState *state);

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
        g_free(state->ram_text);
        g_cond_clear(&state->worker_done);
        g_mutex_clear(&state->worker_mutex);
        g_free(state);
    }
}

static void
button_state_unref(gpointer data, GClosure *closure)
{
    (void) closure;
    state_unref(data);
}

static void
stop_refresh(MonitorState *state)
{
    state->removed = TRUE;
    state->label = NULL;
    state->content = NULL;
    state->fan_button = NULL;
    state->fan_enabled = FALSE;
    if (state->fan_timeout_id != 0) {
        g_source_remove(state->fan_timeout_id);
        state->fan_timeout_id = 0;
    }
    if (state->timeout_id != 0) {
        g_source_remove(state->timeout_id);
        state->timeout_id = 0;
    }
    g_mutex_lock(&state->worker_mutex);
    while (state->worker_running || state->fan_worker_running)
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

    gtk_orientable_set_orientation(GTK_ORIENTABLE(state->content),
                                   stacked ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
    const gchar *selected = state->minimized ? state->ram_text : state->text;
    if (stacked) {
        parts = g_strsplit(selected, " · ", -1);
        text = g_strjoinv("\n", parts);
        gtk_label_set_text(GTK_LABEL(state->label), text);
        g_free(text);
        g_strfreev(parts);
    } else {
        gtk_label_set_text(GTK_LABEL(state->label), selected);
    }
    PangoAttrList *attributes = pango_attr_list_new();
    if (state->cpu_hot && !state->minimized) {
        const gchar *separator = strstr(state->text, " · ");
        PangoAttribute *color = pango_attr_foreground_new(65535, 0, 0);
        color->start_index = 0;
        color->end_index = separator != NULL ? (guint) (separator - state->text)
                                            : (guint) strlen(state->text);
        pango_attr_list_insert(attributes, color);
    }
    gtk_label_set_attributes(GTK_LABEL(state->label), attributes);
    pango_attr_list_unref(attributes);
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
            g_free(state->ram_text);
            state->ram_text = g_strdup(snapshot->ram_text);
            state->cpu_hot = snapshot->cpu_hot != 0;
            gtk_widget_set_tooltip_text(state->label, snapshot->tooltip);
        } else {
            g_free(state->text);
            state->text = g_strdup("Monitor");
            g_free(state->ram_text);
            state->ram_text = g_strdup("Monitor");
            state->cpu_hot = FALSE;
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

static gboolean
button_pressed(GtkWidget *widget, GdkEventButton *event, MonitorState *state)
{
    (void) widget;
    if (state->removed || event->button != 3 || (event->state & GDK_CONTROL_MASK) != 0)
        return FALSE;
    if (event->type == GDK_2BUTTON_PRESS || event->type == GDK_3BUTTON_PRESS)
        return TRUE;
    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    state->minimized = !state->minimized;
    repaint(state, xfce_panel_plugin_get_mode(state->plugin) !=
                   XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
    return TRUE;
}

typedef struct {
    MonitorState *state;
    guint generation;
} FanJob;

static void
fan_job_free(gpointer data)
{
    FanJob *job = data;
    state_unref(job->state);
    g_free(job);
}

static void
fan_status_free(gpointer data)
{
    monitor_fan_status_free(data);
}

static void
fan_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
    FanJob *job = task_data;
    MonitorState *state = job->state;
    (void) source;
    (void) cancel;
    gchar *status = monitor_fan_step(58, 60);
    g_mutex_lock(&state->worker_mutex);
    state->fan_worker_running = FALSE;
    g_cond_broadcast(&state->worker_done);
    g_mutex_unlock(&state->worker_mutex);
    g_task_return_pointer(task, status, fan_status_free);
}

static gboolean
fan_timeout(gpointer data)
{
    MonitorState *state = data;
    state->fan_timeout_id = 0;
    start_fan_refresh(state);
    return G_SOURCE_REMOVE;
}

static void
fan_done(GObject *source, GAsyncResult *result, gpointer data)
{
    GTask *task = G_TASK(result);
    FanJob *job = g_task_get_task_data(task);
    MonitorState *state = job->state;
    gchar *status = g_task_propagate_pointer(task, NULL);
    (void) source;
    (void) data;
    state->fan_in_flight = FALSE;
    if (!state->removed && state->fan_enabled) {
        if (job->generation == state->fan_generation) {
            gtk_widget_set_tooltip_text(state->fan_button, status != NULL ? status :
                                        "Smart fan control on: check failed; retrying");
            state->fan_timeout_id = g_timeout_add_seconds_full(
                G_PRIORITY_DEFAULT, 2, fan_timeout, state_ref(state), state_unref);
        } else {
            start_fan_refresh(state);
        }
    }
    monitor_fan_status_free(status);
}

static void
start_fan_refresh(MonitorState *state)
{
    if (state->removed || !state->fan_enabled || state->fan_in_flight)
        return;
    state->fan_in_flight = TRUE;
    g_mutex_lock(&state->worker_mutex);
    state->fan_worker_running = TRUE;
    g_mutex_unlock(&state->worker_mutex);
    FanJob *job = g_new0(FanJob, 1);
    job->state = state_ref(state);
    job->generation = state->fan_generation;
    GTask *task = g_task_new(state->plugin, NULL, fan_done, NULL);
    g_task_set_task_data(task, job, fan_job_free);
    g_task_run_in_thread(task, fan_worker);
    g_object_unref(task);
}

static void
fan_toggled(GtkToggleButton *button, MonitorState *state)
{
    if (state->removed)
        return;
    state->fan_enabled = gtk_toggle_button_get_active(button);
    state->fan_generation++;
    if (state->fan_timeout_id != 0) {
        g_source_remove(state->fan_timeout_id);
        state->fan_timeout_id = 0;
    }
    gtk_widget_set_tooltip_text(GTK_WIDGET(button), state->fan_enabled ?
        "Smart fan control on: checking…" :
        "Smart fan control off; last fan level left unchanged");
    start_fan_refresh(state);
}

static gboolean
fan_icon_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    GdkRGBA color;
    (void) data;
    gtk_style_context_get_color(gtk_widget_get_style_context(widget),
                               gtk_widget_get_state_flags(widget), &color);
    cairo_save(cr);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_translate(cr, gtk_widget_get_allocated_width(widget) / 2.0,
                    gtk_widget_get_allocated_height(widget) / 2.0);
    for (guint i = 0; i < 3; i++) {
        cairo_move_to(cr, 0, -2);
        cairo_curve_to(cr, -6, -9, 6, -9, 5, -3);
        cairo_curve_to(cr, 4, 0, 2, 1, 0, -2);
        cairo_fill(cr);
        cairo_rotate(cr, 2.0 * G_PI / 3.0);
    }
    cairo_arc(cr, 0, 0, 1.8, 0, 2.0 * G_PI);
    cairo_fill(cr);
    cairo_restore(cr);
    return FALSE;
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
    state->ram_text = g_strdup("Monitor");
    state->label = gtk_label_new(state->text);
    GtkWidget *event_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(event_box), FALSE);
    gtk_widget_add_events(event_box, GDK_BUTTON_PRESS_MASK);
    g_signal_connect_data(event_box, "button-press-event", G_CALLBACK(button_pressed),
                          state_ref(state), button_state_unref, 0);
    gtk_container_add(GTK_CONTAINER(event_box), state->label);
    state->content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    state->fan_button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(state->fan_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(state->fan_button,
                               "Smart fan control off; last fan level left unchanged");
    atk_object_set_name(gtk_widget_get_accessible(state->fan_button), "Smart fan control");
    GtkWidget *icon = gtk_drawing_area_new();
    gtk_widget_set_size_request(icon, 16, 16);
    g_signal_connect(icon, "draw", G_CALLBACK(fan_icon_draw), NULL);
    gtk_container_add(GTK_CONTAINER(state->fan_button), icon);
    g_signal_connect_data(state->fan_button, "toggled", G_CALLBACK(fan_toggled),
                          state_ref(state), button_state_unref, 0);
    gtk_box_pack_start(GTK_BOX(state->content), event_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(state->content), state->fan_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(plugin), state->content);
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
    gtk_widget_show_all(state->content);
    start_refresh(state);
}

XFCE_PANEL_PLUGIN_REGISTER(monitor_construct)
