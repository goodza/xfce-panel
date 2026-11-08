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
    static const char message[] = "FAIL: native smoke global watchdog (40 seconds)\n";
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
    GtkWidget *window = NULL, *label;
    XfcePanelPlugin *plugin;
    GModule *module = NULL;
    Construct construct = NULL;
    struct sigaction action = {0};
    action.sa_handler = watchdog;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    alarm(40);

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
    g_clear_pointer(&tooltip, g_free);
    inline_text = g_strdup(text);
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
