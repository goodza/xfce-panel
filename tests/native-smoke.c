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

static GtkWidget *
find_toggle(GtkWidget *widget)
{
    if (GTK_IS_TOGGLE_BUTTON(widget))
        return widget;
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        GtkWidget *found = NULL;
        for (GList *p = children; p != NULL && found == NULL; p = p->next)
            found = find_toggle(p->data);
        g_list_free(children);
        return found;
    }
    return NULL;
}

typedef struct {
    const gchar *name;
    GtkWidget *found;
} FindWidget;

static void
find_named_child(GtkWidget *widget, gpointer data)
{
    FindWidget *search = data;
    if (search->found != NULL)
        return;
    if (g_str_equal(gtk_widget_get_name(widget), search->name))
        search->found = widget;
    else if (GTK_IS_CONTAINER(widget))
        gtk_container_forall(GTK_CONTAINER(widget), find_named_child, search);
}

static GtkWidget *
find_named(GtkWidget *widget, const gchar *name)
{
    FindWidget search = { name, NULL };
    find_named_child(widget, &search);
    return search.found;
}

static gboolean
probe_range(Construct construct, gint id, gint off, gint automatic, const gchar *warning)
{
    XfcePanelPlugin *plugin = construct("rust-monitor", id, "Range probe", "Isolated test",
                                       NULL, gdk_screen_get_default());
    if (plugin == NULL)
        return FALSE;
    GtkWidget *window = gtk_offscreen_window_new();
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(plugin));
    gtk_widget_show_all(window);
    GtkWidget *low = find_named(window, "fan-off-temperature");
    GtkWidget *high = find_named(window, "fan-auto-temperature");
    GtkWidget *status = find_named(window, "fan-settings-status");
    GtkWidget *toggle = find_toggle(GTK_WIDGET(plugin));
    gboolean valid = low != NULL && high != NULL && status != NULL && toggle != NULL &&
        gtk_range_get_value(GTK_RANGE(low)) == off &&
        gtk_range_get_value(GTK_RANGE(high)) == automatic &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)) &&
        (warning == NULL || strstr(gtk_label_get_text(GTK_LABEL(status)), warning) != NULL);
    gtk_widget_destroy(window);
    return valid;
}

static gboolean
await_tooltip(GtkWidget *widget, const gchar *needle)
{
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    do {
        gchar *text = gtk_widget_get_tooltip_text(widget);
        gboolean found = text != NULL && strstr(text, needle) != NULL;
        g_free(text);
        if (found)
            return TRUE;
        pump();
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}

static gboolean
await_counter(gint (*counter)(void), gint minimum)
{
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    do {
        if (counter() >= minimum)
            return TRUE;
        pump();
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}

static void
observe_destroy(GtkWidget *widget, gpointer data)
{
    (void) widget;
    *(gboolean *) data = TRUE;
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
dismiss_popup_via_key_signal(GtkWidget *popover)
{
    GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
    event->key.window = g_object_ref(gtk_widget_get_window(popover));
    event->key.send_event = TRUE;
    event->key.keyval = GDK_KEY_Escape;
    gdk_event_set_device(event, gdk_seat_get_keyboard(
        gdk_display_get_default_seat(gtk_widget_get_display(popover))));
    gboolean handled = FALSE;
    g_signal_emit_by_name(popover, "key-press-event", &event->key, &handled);
    gdk_event_free(event);
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (gtk_widget_get_visible(popover) && g_get_monotonic_time() < deadline)
        pump();
    if (!handled || gtk_widget_get_visible(popover))
        g_printerr("Escape diagnostic: handled=%d visible=%d mapped=%d realized=%d\n",
                   handled, gtk_widget_get_visible(popover), gtk_widget_get_mapped(popover),
                   gtk_widget_get_realized(popover));
    return handled && !gtk_widget_get_visible(popover);
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
    gboolean popup_destroyed = FALSE;
    GError *error = NULL;
    gchar *root = NULL, *xfce = NULL, *panel = NULL, *config = NULL;
    gchar *cpu = NULL, *fan = NULL, *fan2 = NULL, *script = NULL, *started = NULL;
    gchar *survived = NULL, *contents = NULL, *tooltip = NULL, *inline_text = NULL;
    gchar *pid_text = NULL, *range_rc = NULL;
    GtkWidget *window = NULL, *label, *box;
    XfcePanelPlugin *plugin;
    GModule *module = NULL;
    Construct construct = NULL;
    struct sigaction action = {0};
    action.sa_handler = watchdog;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    alarm(70);

    CHECK(argc == 2 || (argc == 3 && g_str_equal(argv[2], "--fan-stub")),
          "plugin path argument supplied");
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
    if (g_getenv("RUST_MONITOR_TEST_NO_ANIMATIONS") != NULL)
        g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
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
    GtkWidget *fan_button = find_toggle(GTK_WIDGET(plugin));
    CHECK(fan_button != NULL && gtk_widget_get_visible(fan_button) &&
          !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fan_button)),
          "fan icon toggle is visible and starts off");
    CHECK(await_tooltip(fan_button, "last fan level left unchanged"),
          "off tooltip explains retained fan level");
    GtkWidget *popover = find_named(window, "fan-temperature-settings");
    GtkWidget *off_scale = find_named(window, "fan-off-temperature");
    GtkWidget *auto_scale = find_named(window, "fan-auto-temperature");
    GtkWidget *range_status = find_named(window, "fan-settings-status");
    CHECK(popover != NULL && off_scale != NULL && auto_scale != NULL && range_status != NULL,
          "temperature popup and both named sliders constructed");
    CHECK(!gtk_widget_get_visible(popover) &&
          gtk_range_get_value(GTK_RANGE(off_scale)) == 58 &&
          gtk_range_get_value(GTK_RANGE(auto_scale)) == 60,
          "popup starts closed with the default 58–60 C range");
    CHECK(press(box, GDK_BUTTON_PRESS, 1, 0) && gtk_widget_get_visible(popover) &&
          !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fan_button)),
          "left-click opens sliders without enabling smart control");
    CHECK(dismiss_popup_via_key_signal(popover), "Escape key signal closes the freshly opened popup");
    CHECK(press(box, GDK_BUTTON_PRESS, 1, 0) && gtk_widget_get_visible(popover),
          "popup can reopen after Escape");
    GtkAdjustment *adjustment = gtk_range_get_adjustment(GTK_RANGE(off_scale));
    CHECK(gtk_adjustment_get_lower(adjustment) == 30 &&
          gtk_adjustment_get_upper(adjustment) == 90 &&
          gtk_adjustment_get_step_increment(adjustment) == 1 &&
          gtk_scale_get_digits(GTK_SCALE(off_scale)) == 0,
          "sliders expose integer 30–90 C bounds and one-degree steps");
    gtk_range_set_value(GTK_RANGE(off_scale), 45);
    gtk_range_set_value(GTK_RANGE(auto_scale), 70);
    gtk_range_set_value(GTK_RANGE(off_scale), 90);
    CHECK(gtk_range_get_value(GTK_RANGE(off_scale)) == 69,
          "lower threshold clamps below upper rather than crossing it");
    gtk_range_set_value(GTK_RANGE(auto_scale), 30);
    CHECK(gtk_range_get_value(GTK_RANGE(auto_scale)) == 70,
          "upper threshold clamps above lower rather than crossing it");
    gtk_range_set_value(GTK_RANGE(off_scale), 45);
    range_rc = xfce_panel_plugin_save_location(plugin, FALSE);
    CHECK(range_rc != NULL && g_str_has_prefix(range_rc, root) &&
          strstr(gtk_label_get_text(GTK_LABEL(range_status)), "Range saved"),
          "range saved in isolated per-instance XFCE rc file");
    CHECK(put(range_rc, "# Keep this comment\n[Other]\nkeep=value\n[SmartFan]\noff-celsius=45\nauto-celsius=70\n"),
          "existing unrelated rc settings and comment seeded");
    gtk_range_set_value(GTK_RANGE(off_scale), 46);
    g_clear_pointer(&contents, g_free);
    CHECK(g_file_get_contents(range_rc, &contents, NULL, NULL) &&
          strstr(contents, "keep=value") && strstr(contents, "# Keep this comment") &&
          strstr(contents, "off-celsius=46") && strstr(contents, "auto-celsius=70"),
          "range writes preserve unrelated keys/comments and store both thresholds");
    g_clear_pointer(&contents, g_free);
    CHECK(g_unlink(range_rc) == 0 && g_mkdir(range_rc, 0700) == 0,
          "unwritable settings target simulated with a directory");
    gtk_range_set_value(GTK_RANGE(off_scale), 47);
    CHECK(gtk_range_get_value(GTK_RANGE(off_scale)) == 47 &&
          strstr(gtk_label_get_text(GTK_LABEL(range_status)), "Not saved") &&
          strstr(gtk_label_get_text(GTK_LABEL(range_status)), "this session"),
          "failed save keeps session range and displays a warning");
    CHECK(g_rmdir(range_rc) == 0, "save failure fixture removed");
    gtk_range_set_value(GTK_RANGE(off_scale), 48);
    CHECK(strstr(gtk_label_get_text(GTK_LABEL(range_status)), "Range saved"),
          "next adjustment retries saving and clears the error");
    gtk_widget_hide(popover);
    CHECK(!gtk_widget_get_visible(popover) &&
          probe_range(construct, 27182, 48, 70, NULL) &&
          probe_range(construct, 27183, 58, 60, NULL),
          "reconstructed instance restores its range but starts off; another instance uses defaults");
    CHECK(put(range_rc, "[SmartFan]\noff-celsius=90\nauto-celsius=30\n") &&
          probe_range(construct, 27182, 58, 60, "Saved range unavailable"),
          "invalid saved range uses defaults and surfaces a warning");
    CHECK(put(range_rc, "malformed settings [") &&
          probe_range(construct, 27182, 58, 60, "Saved range unavailable"),
          "malformed settings also fall back visibly");
    gtk_range_set_value(GTK_RANGE(off_scale), 47);
    CHECK(g_file_get_contents(range_rc, &contents, NULL, NULL) &&
          g_str_equal(contents, "malformed settings [") &&
          strstr(gtk_label_get_text(GTK_LABEL(range_status)), "Not saved"),
          "malformed rc is not silently overwritten; new range remains session-only");
    g_clear_pointer(&contents, g_free);
    CHECK(put(range_rc, "[SmartFan]\noff-celsius=30\nauto-celsius=31\n") &&
          probe_range(construct, 27182, 30, 31, NULL) &&
          put(range_rc, "[SmartFan]\noff-celsius=89\nauto-celsius=90\n") &&
          probe_range(construct, 27182, 89, 90, NULL),
          "extreme valid ranges reconstruct without GTK scale errors");
    gtk_range_set_value(GTK_RANGE(off_scale), 46);
    gtk_range_set_value(GTK_RANGE(off_scale), 47);
    CHECK(press(box, GDK_BUTTON_PRESS, 1, 0) && gtk_widget_get_visible(popover),
          "popup reopens after closing and persistence checks");
    CHECK(dismiss_popup_via_key_signal(popover), "Escape key signal dismisses the temperature popup");
    if (argc == 3) {
        gint (*calls)(void) = NULL, (*running)(void) = NULL, (*overlap)(void) = NULL;
        gint (*fan_off)(void) = NULL, (*fan_auto)(void) = NULL;
        CHECK(g_module_symbol(module, "smoke_fan_calls", (gpointer *) &calls) &&
              g_module_symbol(module, "smoke_fan_running", (gpointer *) &running) &&
              g_module_symbol(module, "smoke_fan_overlap", (gpointer *) &overlap) &&
              g_module_symbol(module, "smoke_fan_off", (gpointer *) &fan_off) &&
              g_module_symbol(module, "smoke_fan_auto", (gpointer *) &fan_auto),
              "test-only fan backend resolved (no hardware or sudo)");
        CHECK(calls() == 0, "disabled startup never invokes fan controller");
        g_clear_pointer(&contents, g_free);
        contents = g_strdup_printf(
            "interval_seconds=3600\ncpu_sensor='%s'\nfan_sensors=[]\ndisks=[]\n"
            "[[extensions]]\nname='Slow'\ncommand=['/bin/sh','-c','touch \"%s\"; sleep 30']\n"
            "timeout_ms=3000\n", cpu, started);
        CHECK(put(config, contents), "slow metrics with one-hour refresh configured");
        gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
        while (!g_file_test(started, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline)
            pump();
        CHECK(g_file_test(started, G_FILE_TEST_EXISTS), "slow metric collection is in flight");
        gint64 before = g_get_monotonic_time();
        gtk_button_clicked(GTK_BUTTON(fan_button));
        CHECK(g_get_monotonic_time() - before < 100000 &&
              gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fan_button)),
              "fan enable returns immediately without blocking GTK");
        CHECK(await_counter(running, 1) && fan_off() == 47 && fan_auto() == 70,
              "first worker receives a snapshot of the selected range");
        gtk_range_set_value(GTK_RANGE(off_scale), 50);
        gtk_range_set_value(GTK_RANGE(auto_scale), 75);
        CHECK(fan_off() == 47 && fan_auto() == 70 &&
              await_tooltip(fan_button, "simulated permission failure"),
              "in-flight check keeps old range and reports failure during slow metrics");
        CHECK(press(box, GDK_BUTTON_PRESS, 3, 0) && ram_only(label) &&
              gtk_widget_get_visible(fan_button) &&
              gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fan_button)),
              "RAM-only mode leaves fan icon visible and enabled");
        CHECK(await_tooltip(fan_button, "level auto (simulated)") && calls() >= 2 &&
              fan_off() == 50 && fan_auto() == 75,
              "next fan check receives new range and retries despite one-hour metric delay");
        CHECK(await_counter(calls, 3) && running() == 1,
              "next fan check starts for off-during-flight test");
        gtk_button_clicked(GTK_BUTTON(fan_button));
        deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
        gint stopped_calls = calls();
        while (g_get_monotonic_time() < deadline)
            pump();
        CHECK(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fan_button)) &&
              calls() == stopped_calls && running() == 0 &&
              await_tooltip(fan_button, "last fan level left unchanged"),
              "off cancels future checks and ignores stale in-flight status without restoring auto");
        gtk_button_clicked(GTK_BUTTON(fan_button));
        CHECK(await_counter(running, 1), "fan check starts before rapid toggles");
        for (guint i = 0; i < 10; i++)
            gtk_button_clicked(GTK_BUTTON(fan_button));
        CHECK(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fan_button)) &&
              await_counter(calls, stopped_calls + 2) && overlap() == 0,
              "rapid off/on toggles serialize checks and start a fresh generation");
        CHECK(await_counter(running, 1), "fan worker active before widget destruction");
        g_signal_connect(popover, "destroy", G_CALLBACK(observe_destroy), &popup_destroyed);
        CHECK(press(box, GDK_BUTTON_PRESS, 1, 0) && gtk_widget_get_visible(popover),
              "popup opened before removal during fan work");
        before = g_get_monotonic_time();
        gtk_widget_destroy(window);
        window = NULL;
        CHECK(popup_destroyed && running() == 0 && g_get_monotonic_time() - before < 2 * G_TIME_SPAN_SECOND,
              "removal waits for fan worker without GTK deadlock or a restoration command");
        stopped_calls = calls();
        deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
        while (g_get_monotonic_time() < deadline)
            pump();
        CHECK(calls() == stopped_calls && overlap() == 0,
              "removed plugin never schedules another fan check");
        result = 0;
        goto cleanup;
    }
    inline_text = g_strdup(gtk_label_get_text(GTK_LABEL(label)));
    g_clear_pointer(&tooltip, g_free);
    tooltip = gtk_widget_get_tooltip_text(label);
    CHECK(press(box, GDK_BUTTON_PRESS, 3, 0) && ram_only(label),
          "ordinary right-click immediately selects only available RAM");
    CHECK(press(box, GDK_BUTTON_PRESS, 1, 0) && gtk_widget_get_visible(popover) && ram_only(label),
          "left-click opens range controls in RAM-only mode without changing the view");
    gtk_widget_hide(popover);
    gchar *same_tooltip = gtk_widget_get_tooltip_text(label);
    CHECK(g_strcmp0(same_tooltip, tooltip) == 0, "toggle preserves complete plain-text tooltip");
    g_free(same_tooltip);
    CHECK(!press(box, GDK_BUTTON_PRESS, 3, GDK_CONTROL_MASK) &&
          !press(box, GDK_BUTTON_PRESS, 1, GDK_CONTROL_MASK) &&
          !press(box, GDK_BUTTON_PRESS, 2, 0) && ram_only(label),
          "Ctrl-modified clicks and middle button remain unhandled without toggling");
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
          !strstr(gtk_label_get_text(GTK_LABEL(label)), " · ") &&
          gtk_orientable_get_orientation(GTK_ORIENTABLE(gtk_widget_get_parent(box))) ==
              GTK_ORIENTATION_VERTICAL && gtk_widget_get_visible(fan_button),
          "injected deskbar mode stacks metrics and fan icon (not live panel orientation)");
    g_signal_emit_by_name(plugin, "mode-changed", XFCE_PANEL_PLUGIN_MODE_HORIZONTAL);
    CHECK(g_str_equal(gtk_label_get_text(GTK_LABEL(label)), inline_text) &&
          gtk_orientable_get_orientation(GTK_ORIENTABLE(gtk_widget_get_parent(box))) ==
              GTK_ORIENTATION_HORIZONTAL,
          "injected horizontal mode callback restores inline metrics and icon layout");
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
    g_signal_connect(popover, "destroy", G_CALLBACK(observe_destroy), &popup_destroyed);
    CHECK(press(box, GDK_BUTTON_PRESS, 1, 0) && gtk_widget_get_visible(popover),
          "popup opened before removal during metric work");
    gint64 before = g_get_monotonic_time();
    gtk_widget_destroy(window);
    window = NULL;
    CHECK(popup_destroyed, "open popup is destroyed with the plugin");
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
    if (range_rc != NULL && g_file_test(range_rc, G_FILE_TEST_IS_DIR))
        g_rmdir(range_rc);
    const gchar *files[] = {config, cpu, fan, fan2, script, started, survived, range_rc};
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
    g_free(range_rc);
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
