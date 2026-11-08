#define _POSIX_C_SOURCE 200809L
#include <gio/gio.h>
#include <gmodule.h>
#include <glib/gstdio.h>
#include <libxfce4panel/libxfce4panel.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef XfcePanelPlugin *(*Construct)(const gchar *, gint, const gchar *,
                                    const gchar *, gchar **, GdkScreen *);

#define CHECK(condition, message) do { \
    if (!(condition)) { g_printerr("FAIL: %s\n", message); goto cleanup; } \
    g_print("PASS: %s\n", message); \
} while (0)

static void
watchdog(int signal_number)
{
    static const char message[] = "FAIL: native smoke global watchdog (70 seconds)\n";
    (void) signal_number;
    (void) write(STDERR_FILENO, message, sizeof message - 1);
    _exit(124);
}

static void
pump(void)
{
    /* Bound dispatch even if a source continuously becomes ready. */
    for (guint i = 0; i < 32 && g_main_context_pending(NULL); i++)
        g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
}

static GtkWidget *
find_label(GtkWidget *widget)
{
    if (GTK_IS_LABEL(widget))
        return widget;
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        GtkWidget *found = NULL;
        for (GList *p = children; p != NULL && found == NULL; p = p->next)
            found = find_label(p->data);
        g_list_free(children);
        return found;
    }
    return NULL;
}

static gboolean
await_text(GtkWidget *label, const gchar *needle)
{
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    do {
        if (strstr(gtk_label_get_text(GTK_LABEL(label)), needle) != NULL)
            return TRUE;
        pump();
    } while (g_get_monotonic_time() < deadline);
    g_printerr("Last label: %s\n", gtk_label_get_text(GTK_LABEL(label)));
    return FALSE;
}

static gboolean
cpu_color_matches(GtkWidget *label, gboolean hot)
{
    PangoAttrList *list = gtk_label_get_attributes(GTK_LABEL(label));
    GSList *attributes = list != NULL ? pango_attr_list_get_attributes(list) : NULL;
    guint count = 0;
    gboolean valid = TRUE;
    for (GSList *p = attributes; p != NULL; p = p->next) {
        PangoAttribute *attribute = p->data;
        if (attribute->klass->type == PANGO_ATTR_FOREGROUND) {
            PangoAttrColor *color = (PangoAttrColor *) attribute;
            count++;
            valid = valid && color->color.red == 65535 && color->color.green == 0 &&
                    color->color.blue == 0 && attribute->start_index == 0 &&
                    attribute->end_index == strlen("80.0 °C");
        }
    }
    g_slist_free_full(attributes, (GDestroyNotify) pango_attribute_destroy);
    return hot ? valid && count == 1 : count == 0;
}

static gboolean
await_warning(GtkWidget *label, const gchar *text, gboolean hot)
{
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    do {
        if (strstr(gtk_label_get_text(GTK_LABEL(label)), text) && cpu_color_matches(label, hot))
            return TRUE;
        pump();
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}

/* Dispatch a real button event to the realized child, not a callback shortcut. */
static GdkEvent *
button_event(GtkWidget *box, GdkEventType type, guint button, guint modifiers)
{
    GdkEvent *event = gdk_event_new(type);
    GdkWindow *window = gtk_widget_get_window(box);
    GList *children = gdk_window_get_children(window);
    for (GList *p = children; p != NULL; p = p->next) {
        gpointer owner = NULL;
        gdk_window_get_user_data(p->data, &owner);
        if (owner == box) {
            window = p->data;
            break;
        }
    }
    event->button.window = g_object_ref(window);
    g_list_free(children);
    event->button.send_event = TRUE;
    event->button.time = GDK_CURRENT_TIME;
    event->button.button = button;
    event->button.state = modifiers;
    gdk_event_set_device(event, gdk_seat_get_pointer(
        gdk_display_get_default_seat(gtk_widget_get_display(box))));
    return event;
}

static gboolean
press(GtkWidget *box, GdkEventType type, guint button, guint modifiers)
{
    GdkEvent *event = button_event(box, type, button, modifiers);
    gboolean handled = gtk_widget_event(box, event);
    gdk_event_free(event);
    return handled;
}

static gboolean
parent_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void) widget;
    (void) event;
    *(gboolean *) data = TRUE;
    return TRUE; /* Observe propagation without opening an XFCE menu. */
}

static gboolean
ram_only(GtkWidget *label)
{
    const gchar *text = gtk_label_get_text(GTK_LABEL(label));
    gchar *end = NULL;
    double ram = g_ascii_strtod(text, &end);
    return end != text && ram >= 0 && g_str_equal(end, " GiB") &&
           cpu_color_matches(label, FALSE);
}

static gboolean
put(const gchar *path, const gchar *contents)
{
    GError *error = NULL;
    if (g_file_set_contents(path, contents, -1, &error))
        return TRUE;
    g_printerr("Writing %s: %s\n", path, error->message);
    g_error_free(error);
    return FALSE;
}

int
main(int argc, char **argv)
{
    int result = 1;
    GError *error = NULL;
    gchar *root = NULL, *xfce = NULL, *panel = NULL, *config = NULL;
    gchar *cpu = NULL, *fan = NULL, *fan2 = NULL, *script = NULL, *started = NULL;
    gchar *survived = NULL, *contents = NULL, *tooltip = NULL, *inline_text = NULL;
    gchar *pid_text = NULL;
    GtkWidget *window = NULL, *label, *box;
    XfcePanelPlugin *plugin;
    GModule *module = NULL;
    Construct construct = NULL;
    struct sigaction action = {0};
    action.sa_handler = watchdog;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    alarm(70);

    CHECK(argc == 2, "plugin path argument supplied");
    root = g_dir_make_tmp("rust-monitor-native-XXXXXX", &error);
    CHECK(root != NULL, "isolated temporary config directory created");
    gchar *absolute_root = g_canonicalize_filename(root, NULL);
    g_free(root);
    root = absolute_root;
    /* Must precede GTK, GModule loading, and all config-directory queries. */
    CHECK(g_setenv("XDG_CONFIG_HOME", root, TRUE), "XDG_CONFIG_HOME isolated before GTK init");
    xfce = g_build_filename(root, "xfce4", NULL);
    panel = g_build_filename(xfce, "panel", NULL);
    config = g_build_filename(panel, "rust-monitor.toml", NULL);
    cpu = g_build_filename(root, "cpu", NULL);
    fan = g_build_filename(root, "fan", NULL);
    fan2 = g_build_filename(root, "fan2", NULL);
    script = g_build_filename(root, "extension.sh", NULL);
    started = g_build_filename(root, "started", NULL);
    survived = g_build_filename(root, "survived", NULL);
    CHECK(g_mkdir_with_parents(panel, 0700) == 0 &&
          put(cpu, "42500\n") && put(fan, "1234\n") && put(fan2, "9876\n"),
          "regular CPU/fan fixture files created");
    contents = g_strdup_printf(
        "interval_seconds = 1\ncpu_sensor = '%s'\nfan_sensors = ['%s', '%s']\n"
        "disks = ['/']\n[[extensions]]\nname = 'Trusted'\n"
        "command = ['/bin/printf', '<ok>\\n<detail>\\n']\ntimeout_ms = 300\n", cpu, fan, fan2);
    CHECK(put(config, contents), "initial isolated configuration written");
    g_clear_pointer(&contents, g_free);
    CHECK(gtk_init_check(NULL, NULL), "GTK display available (no skip)");
    module = g_module_open(argv[1], G_MODULE_BIND_LOCAL);
    if (module == NULL)
        g_printerr("GModule: %s\n", g_module_error());
    CHECK(module != NULL, "built plugin opened via GModule");
    CHECK(g_module_symbol(module, "xfce_panel_module_construct", (gpointer *) &construct),
          "exported XFCE factory resolved");
    plugin = construct("rust-monitor", 27182, "Native smoke", "Isolated test", NULL,
                       gdk_screen_get_default());
    CHECK(plugin != NULL, "exported factory constructed real XFCE widget");
    window = gtk_offscreen_window_new();
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(plugin));
    gtk_widget_show_all(window);
    label = find_label(GTK_WIDGET(plugin));
    CHECK(gtk_widget_get_realized(GTK_WIDGET(plugin)) && label != NULL,
          "offscreen realization invoked registered constructor and created label");
    CHECK(await_text(label, "Trusted: <ok>"), "initial asynchronous snapshot arrived within 8 seconds");
    const gchar *text = gtk_label_get_text(GTK_LABEL(label));
    CHECK(strstr(text, "42.5 °C") && strstr(text, "1234 RPM") &&
          !strstr(text, "CPU") && !strstr(text, "Fan 1") &&
          !strstr(text, "RAM") && !strstr(text, "available") && !strstr(text, "9876 RPM"),
          "compact CPU, first fan and RAM text omits labels and second fan");
    double ram = -1, disk = -1;
    gchar **metrics = g_strsplit(text, " · ", -1);
    for (guint i = 0; metrics[i] != NULL; i++) {
        if (g_str_has_suffix(metrics[i], " GiB"))
            sscanf(metrics[i], "%lf GiB", &ram);
    }
    g_strfreev(metrics);
    const gchar *disk_text = strstr(text, "/ ");
    CHECK(ram > 0 &&
          disk_text && sscanf(disk_text, "/ %lf%%", &disk) == 1 && disk >= 0 && disk <= 100,
          "real memory availability and root filesystem occupancy displayed");
    tooltip = gtk_widget_get_tooltip_text(label);
    CHECK(!gtk_label_get_use_markup(GTK_LABEL(label)) &&
          strstr(gtk_label_get_label(GTK_LABEL(label)), "Trusted: <ok>") &&
          tooltip && strstr(tooltip, "Trusted: <ok>\n<detail>") &&
          strstr(tooltip, "GiB free available") && strstr(tooltip, "CPU 42.5 °C") &&
          strstr(tooltip, "Fan 1 (") && strstr(tooltip, "Fan 2 (") &&
          strstr(tooltip, "9876 RPM") && strstr(tooltip, "RAM "),
          "trusted angle-bracket output is literal label and plain-text tooltip");
    box = gtk_widget_get_parent(label);
    CHECK(GTK_IS_EVENT_BOX(box) &&
          !gtk_event_box_get_visible_window(GTK_EVENT_BOX(box)),
          "label wrapped in invisible-window event box");
    inline_text = g_strdup(text);
    CHECK(press(box, GDK_BUTTON_PRESS, 3, 0) && ram_only(label),
          "ordinary right-click immediately selects only available RAM");
    gchar *same_tooltip = gtk_widget_get_tooltip_text(label);
    CHECK(g_strcmp0(same_tooltip, tooltip) == 0, "toggle preserves complete plain-text tooltip");
    g_free(same_tooltip);
    CHECK(!press(box, GDK_BUTTON_PRESS, 3, GDK_CONTROL_MASK) &&
          !press(box, GDK_BUTTON_PRESS, 1, 0) &&
          !press(box, GDK_BUTTON_PRESS, 2, 0) && ram_only(label),
          "Ctrl-right-click and other buttons are unhandled without toggling");
    CHECK(press(box, GDK_2BUTTON_PRESS, 3, 0) &&
          press(box, GDK_3BUTTON_PRESS, 3, 0) && ram_only(label),
          "extra right-button multi-click events are consumed without toggling");
    guint signal_id = g_signal_lookup("button-press-event", GTK_TYPE_WIDGET);
    g_signal_handlers_block_matched(plugin, G_SIGNAL_MATCH_ID, signal_id, 0, NULL, NULL, NULL);
    gboolean propagated = FALSE;
    gulong observer = g_signal_connect(plugin, "button-press-event", G_CALLBACK(parent_press), &propagated);
    GdkEvent *event = button_event(box, GDK_BUTTON_PRESS, 3, GDK_CONTROL_MASK);
    gtk_propagate_event(box, event);
    gdk_event_free(event);
    gboolean ctrl_propagated = propagated;
    propagated = FALSE;
    event = button_event(box, GDK_BUTTON_PRESS, 3, 0);
    gtk_propagate_event(box, event);
    gtk_propagate_event(box, event);
    gdk_event_free(event);
    event = button_event(box, GDK_2BUTTON_PRESS, 3, 0);
    gtk_propagate_event(box, event);
    gdk_event_free(event);
    event = button_event(box, GDK_3BUTTON_PRESS, 3, 0);
    gtk_propagate_event(box, event);
    gdk_event_free(event);
    gboolean ordinary_consumed = !propagated;
    g_signal_handler_disconnect(plugin, observer);
    g_signal_handlers_unblock_matched(plugin, G_SIGNAL_MATCH_ID, signal_id, 0, NULL, NULL, NULL);
    CHECK(ctrl_propagated && ram_only(label), "GTK propagates Ctrl-right-click to parent without menu popup");
    CHECK(ordinary_consumed, "GTK stops ordinary and multi-click right presses before parent panel handler");
    for (guint i = 0; i < 10; i++)
        CHECK(press(box, GDK_BUTTON_PRESS, 3, 0), "rapid right-click handled");
    CHECK(ram_only(label), "rapid even toggles preserve minimized state");
    g_signal_emit_by_name(plugin, "mode-changed", XFCE_PANEL_PLUGIN_MODE_DESKBAR);
    CHECK(ram_only(label), "minimized deskbar remains RAM-only without CPU color");
    CHECK(press(box, GDK_BUTTON_PRESS, 3, 0) &&
          strchr(gtk_label_get_text(GTK_LABEL(label)), '\n') == NULL,
          "return to full uses current actual horizontal mode");
    CHECK(g_str_equal(gtk_label_get_text(GTK_LABEL(label)), inline_text), "return restores cached full text");
    g_clear_pointer(&tooltip, g_free);
    g_signal_emit_by_name(plugin, "mode-changed", XFCE_PANEL_PLUGIN_MODE_DESKBAR);
    CHECK(strchr(gtk_label_get_text(GTK_LABEL(label)), '\n') &&
          !strstr(gtk_label_get_text(GTK_LABEL(label)), " · "),
          "injected deskbar mode callback stacks metrics (not live panel orientation)");
    g_signal_emit_by_name(plugin, "mode-changed", XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
    CHECK(g_str_equal(gtk_label_get_text(GTK_LABEL(label)), inline_text),
          "injected horizontal mode callback restores inline metrics");
    g_signal_emit_by_name(plugin, "orientation-changed", GTK_ORIENTATION_VERTICAL);
    CHECK(strchr(gtk_label_get_text(GTK_LABEL(label)), '\n') != NULL,
          "injected vertical orientation callback stacks metrics");
    g_signal_emit_by_name(plugin, "orientation-changed", GTK_ORIENTATION_HORIZONTAL);
    CHECK(g_str_equal(gtk_label_get_text(GTK_LABEL(label)), inline_text),
          "injected horizontal orientation callback restores inline metrics");

    CHECK(put(cpu, "80000\n") && await_warning(label, "80.0 °C", FALSE),
          "exactly 80 C has no warning color");
    CHECK(put(cpu, "80001\n") && await_warning(label, "80.0 °C", TRUE),
          "raw temperature above 80 C is red even when display rounds to 80.0");
    CHECK(!gtk_label_get_use_markup(GTK_LABEL(label)) &&
          strstr(gtk_label_get_text(GTK_LABEL(label)), "Trusted: <ok>"),
          "CPU warning preserves literal extension text without markup");
    g_signal_emit_by_name(plugin, "mode-changed", XFCE_PANEL_PLUGIN_MODE_DESKBAR);
    CHECK(cpu_color_matches(label, TRUE) && strchr(gtk_label_get_text(GTK_LABEL(label)), '\n'),
          "stacked layout keeps red limited to CPU metric bytes");
    g_signal_emit_by_name(plugin, "mode-changed", XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
    CHECK(put(cpu, "80000\n") && await_warning(label, "80.0 °C", FALSE),
          "warning color clears when temperature falls to exactly 80 C");
    CHECK(put(cpu, "81000\n") && await_warning(label, "81.0 °C", TRUE),
          "warning color returns above threshold before sensor removal");

    CHECK(press(box, GDK_BUTTON_PRESS, 3, 0) && ram_only(label),
          "hot CPU warning is absent in RAM-only mode");
    CHECK(put(cpu, "80001\n"), "hot CPU changes while minimized");
    gint64 hot_deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    do {
        g_clear_pointer(&tooltip, g_free);
        tooltip = gtk_widget_get_tooltip_text(label);
        if (tooltip && strstr(tooltip, "CPU 80.0 °C"))
            break;
        pump();
    } while (g_get_monotonic_time() < hot_deadline);
    CHECK(tooltip && strstr(tooltip, "CPU 80.0 °C") && ram_only(label),
          "live polling updates tooltip and hot cache while remaining RAM-only");
    CHECK(press(box, GDK_BUTTON_PRESS, 3, 0) && cpu_color_matches(label, TRUE) &&
          strstr(gtk_label_get_text(GTK_LABEL(label)), "80.0 °C"),
          "return to full restores current hot CPU from cache");
    CHECK(press(box, GDK_BUTTON_PRESS, 3, 0) && ram_only(label), "minimized before config reload");

    contents = g_strdup_printf(
        "interval_seconds = 1\ncpu_sensor = '%s/missing-cpu'\n"
        "fan_sensors = ['%s/missing-fan']\ndisks = ['/']\n"
        "[[extensions]]\nname = 'Broken'\ncommand = ['/bin/sh', '-c', 'exit 7']\n"
        "timeout_ms = 300\n", root, root);
    CHECK(put(config, contents), "reload configuration written with missing sensors and failing extension");
    g_clear_pointer(&contents, g_free);
    /* Await tooltip, since failed metrics intentionally never enter the label. */
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    do {
        g_clear_pointer(&tooltip, g_free);
        tooltip = gtk_widget_get_tooltip_text(label);
        if (tooltip && strstr(tooltip, "Extension Broken:"))
            break;
        pump();
    } while (g_get_monotonic_time() < deadline);
    CHECK(ram_only(label), "config reload and live memory snapshots preserve RAM-only selection");
    CHECK(press(box, GDK_BUTTON_PRESS, 3, 0), "restore full after minimized reload");
    text = gtk_label_get_text(GTK_LABEL(label));
    CHECK(tooltip && strstr(tooltip, "Extension Broken: exit") &&
          strstr(tooltip, "7") && strstr(tooltip, "missing-cpu") &&
          strstr(tooltip, "missing-fan") && strstr(tooltip, "CPU temperature unavailable"),
          "one-second polling reload exposes missing sensor and extension errors within 8 seconds");
    CHECK(!strstr(text, "CPU") && !strstr(text, "Fan") && !strstr(text, "Broken") &&
          !strstr(text, "Trusted") && strstr(text, " GiB") && strstr(text, "/ "),
          "reload hides missing sensors and failed/removed extensions, retains memory/disk");
    CHECK(cpu_color_matches(label, FALSE), "missing CPU reading clears warning attributes");

    CHECK(put(script, "#!/bin/sh\n( printf '%s\\n' \"$$\" > \"$1\"; sleep 0.5; : > \"$2\" ) &\nwait\n"),
          "in-flight descendant marker fixture written");
    contents = g_strdup_printf(
        "interval_seconds = 1\ncpu_sensor = '%s'\nfan_sensors = []\ndisks = ['/']\n"
        "[[extensions]]\nname = 'Slow'\ncommand = ['/bin/sh', '%s', '%s', '%s']\n"
        "timeout_ms = 300\n", cpu, script, started, survived);
    CHECK(put(config, contents), "300ms timeout extension configuration written");
    deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    while (!g_file_test(started, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline)
        pump();
    CHECK(g_file_test(started, G_FILE_TEST_EXISTS), "extension descendant started before destroy within 8 seconds");
    CHECK(g_file_get_contents(started, &pid_text, NULL, NULL), "extension leader PID recorded");
    gchar *pid_end = NULL;
    gint64 leader_pid = g_ascii_strtoll(pid_text, &pid_end, 10);
    CHECK(leader_pid > 0 && leader_pid <= G_MAXINT && pid_end != pid_text &&
          g_str_equal(pid_end, "\n"), "extension leader PID is valid");
    gint64 before = g_get_monotonic_time();
    gtk_widget_destroy(window);
    window = NULL;
    gint64 elapsed = g_get_monotonic_time() - before;
    g_print("Destroy waited %.1f ms\n", elapsed / 1000.0);
    CHECK(elapsed < 2 * G_TIME_SPAN_SECOND,
          "destroy completes without main-context deadlock");
    errno = 0;
    CHECK(kill((pid_t) leader_pid, 0) == -1 && errno == ESRCH,
          "extension leader is terminated and reaped before destroy returns");
    deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline)
        pump();
    CHECK(!g_file_test(survived, G_FILE_TEST_EXISTS),
          "descendant never writes survived marker after destroy (one-second observation)");
    result = 0;

cleanup:
    if (window != NULL)
        gtk_widget_destroy(window);
    /* Drain task completion references before closing the actual module. */
    for (guint i = 0; i < 50; i++)
        pump();
    if (module != NULL)
        g_module_close(module);
    const gchar *files[] = {config, cpu, fan, fan2, script, started, survived};
    for (guint i = 0; i < G_N_ELEMENTS(files); i++)
        if (files[i] != NULL)
            g_unlink(files[i]);
    const gchar *dirs[] = {panel, xfce, root};
    for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
        if (dirs[i] != NULL && g_rmdir(dirs[i]) != 0) {
            g_printerr("FAIL: could not remove test directory %s\n", dirs[i]);
            result = 1;
        }
    }
    g_clear_error(&error);
    g_free(contents);
    g_free(tooltip);
    g_free(inline_text);
    g_free(pid_text);
    g_free(config);
    g_free(cpu);
    g_free(fan);
    g_free(fan2);
    g_free(script);
    g_free(started);
    g_free(survived);
    g_free(panel);
    g_free(xfce);
    g_free(root);
    alarm(0);
    return result;
}
