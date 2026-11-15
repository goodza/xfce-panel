#include <gio/gio.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>

/* Run the XEmbed socket and wrapper in separate processes. */
typedef struct {
    gboolean finished;
    gboolean success;
} ChildResult;

static void
child_done(GObject *source, GAsyncResult *result, gpointer data)
{
    ChildResult *child = data;
    GError *error = NULL;
    child->success = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);
    if (error != NULL) {
        g_printerr("Embedded child: %s\n", error->message);
        g_error_free(error);
    }
    child->finished = TRUE;
}

int
main(int argc, char **argv)
{
    if (argc != 3 || !gtk_init_check(NULL, NULL) ||
        !GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        g_printerr("Usage: embedded-smoke NATIVE-SMOKE PLUGIN (requires X11 display)\n");
        return 1;
    }
    GtkWidget *host = gtk_window_new(GTK_WINDOW_POPUP);
    GtkWidget *socket = gtk_socket_new();
    gtk_widget_set_opacity(host, 0);
    gtk_window_move(GTK_WINDOW(host), -10000, -10000);
    gtk_widget_set_size_request(socket, 500, 24);
    gtk_container_add(GTK_CONTAINER(host), socket);
    gtk_widget_show_all(host);
    gchar *argument = g_strdup_printf("--embedded=%lu", (unsigned long) gtk_socket_get_id(GTK_SOCKET(socket)));
    GError *error = NULL;
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, argv[1], argv[2], argument, NULL);
    g_free(argument);
    if (process == NULL) {
        g_printerr("Starting embedded child: %s\n", error->message);
        g_error_free(error);
        gtk_widget_destroy(host);
        return 1;
    }
    ChildResult result = { FALSE, FALSE };
    g_subprocess_wait_check_async(process, NULL, child_done, &result);
    gint64 deadline = g_get_monotonic_time() + 80 * G_TIME_SPAN_SECOND;
    while (!result.finished && g_get_monotonic_time() < deadline) {
        for (guint i = 0; i < 32 && g_main_context_pending(NULL); i++)
            g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    if (!result.finished) {
        g_printerr("Embedded child deadline exceeded\n");
        g_subprocess_force_exit(process);
        while (!result.finished)
            g_main_context_iteration(NULL, TRUE);
    }
    g_object_unref(process);
    gtk_widget_destroy(host);
    if (result.success)
        g_print("PASS: cross-process embedded panel popup regression\n");
    return result.success ? 0 : 1;
}
