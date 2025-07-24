/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#include <glib.h>
#include <gio/gio.h>
#include <libnotify/notify.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#define DBUS_SERVICE_NAME "io.furios.Lmkd"
#define DBUS_OBJECT_PATH "/io/furios/Lmkd"
#define DBUS_INTERFACE_NAME "io.furios.Lmkd"

typedef struct {
    GDBusConnection *connection;
    guint signal_subscription_id;
    GMainLoop *main_loop;
    gboolean notifications_enabled;
} LmkdUserServer;

static LmkdUserServer *g_client = NULL;

static void show_process_killed_notification(gint32 pid,
                                             gint32 uid,
                                             gint32 oom_score_adj,
                                             const gchar *process_name,
                                             const gchar *kill_reason,
                                             gint64 rss_kb,
                                             gint64 swap_kb,
                                             gint32 thrashing,
                                             gint32 max_thrashing,
                                             gint64 free_mem_kb,
                                             gint64 free_swap_kb) {
    if (!g_client->notifications_enabled)
        return;

    g_debug("Process killed: pid=%d, uid=%d, oom_score_adj=%d, "
            "process_name='%s', kill_reason='%s', rss_kb=%" G_GINT64_FORMAT ", "
            "swap_kb=%" G_GINT64_FORMAT ", thrashing=%d%%, max_thrashing=%d%%, "
            "free_mem_kb=%" G_GINT64_FORMAT ", free_swap_kb=%" G_GINT64_FORMAT,
            pid, uid, oom_score_adj, process_name, kill_reason, rss_kb, swap_kb,
            thrashing, max_thrashing, free_mem_kb, free_swap_kb);

    NotifyNotification *notification;
    GError *error = NULL;

    const gchar *basename = g_path_get_basename(process_name);
    gchar *summary = g_strdup_printf("Process Killed: %s", basename);

    notification = notify_notification_new(summary, "Device is not responding and low on memory", "dialog-error");

    notify_notification_set_urgency(notification, NOTIFY_URGENCY_CRITICAL);

    notify_notification_set_timeout(notification, 5000);

    if (!notify_notification_show(notification, &error)) {
        g_warning("Failed to show notification: %s", error->message);
        g_error_free(error);
    }

    g_object_unref(notification);
    g_free(summary);
}

static void on_signal_received(GDBusConnection *connection,
                               const gchar *sender_name,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *signal_name,
                               GVariant *parameters,
                               gpointer user_data) {
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    g_debug("Received signal: %s", signal_name);

    if (g_strcmp0(signal_name, "ProcessKilled") == 0) {
        gint32 pid, uid, oom_score_adj, thrashing, max_thrashing;
        gchar *process_name, *kill_reason;
        gint64 rss_kb, swap_kb, free_mem_kb, free_swap_kb;

        g_variant_get(parameters, "(iiissxxiixx)",
                      &pid, &uid, &oom_score_adj,
                      &process_name, &kill_reason,
                      &rss_kb, &swap_kb,
                      &thrashing, &max_thrashing,
                      &free_mem_kb, &free_swap_kb);

        g_debug("Process killed: %s (PID: %d, Reason: %s)",
                process_name, pid, kill_reason);

        show_process_killed_notification(pid, uid, oom_score_adj,
                                         process_name, kill_reason,
                                         rss_kb, swap_kb,
                                         thrashing, max_thrashing,
                                         free_mem_kb, free_swap_kb);

        g_free(process_name);
        g_free(kill_reason);
    }
}

static gboolean init_client(LmkdUserServer *client, GError **error) {
    GError *local_error = NULL;

    g_debug("Initializing LMKD user server");

    client->connection = NULL;
    client->signal_subscription_id = 0;
    client->notifications_enabled = TRUE;

    /* Initialize libnotify */
    if (!notify_init("Low Memory Killer")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to initialize libnotify");
        return FALSE;
    }

    g_debug("Libnotify initialized successfully");

    /* Connect to system bus */
    client->connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &local_error);
    if (!client->connection) {
        g_propagate_error(error, local_error);
        return FALSE;
    }

    g_debug("Connected to system bus");

    /* Subscribe to signals */
    client->signal_subscription_id = g_dbus_connection_signal_subscribe(
        client->connection,
        DBUS_SERVICE_NAME,
        DBUS_INTERFACE_NAME,
        NULL,
        DBUS_OBJECT_PATH,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_signal_received,
        client,
        NULL
    );

    if (client->signal_subscription_id == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to subscribe to D-Bus signals");
        return FALSE;
    }

    g_debug("Subscribed to LMKD signals (ID: %u)", client->signal_subscription_id);

    client->main_loop = g_main_loop_new(NULL, FALSE);

    g_debug("LMKD user server initialized successfully");

    return TRUE;
}

static void cleanup_client(LmkdUserServer *client) {
    if (!client)
        return;

    if (client->main_loop) {
        g_main_loop_unref(client->main_loop);
        client->main_loop = NULL;
    }

    if (client->signal_subscription_id > 0) {
        g_dbus_connection_signal_unsubscribe(client->connection,
                                             client->signal_subscription_id);
        client->signal_subscription_id = 0;
    }

    if (client->connection) {
        g_object_unref(client->connection);
        client->connection = NULL;
    }

    if (client->notifications_enabled) {
        notify_uninit();
        client->notifications_enabled = FALSE;
    }
}

static void signal_handler(int sig) {
    g_debug("Received signal %d, shutting down gracefully...", sig);

    if (g_client && g_client->main_loop)
        g_main_loop_quit(g_client->main_loop);
}

int main(void) {
    LmkdUserServer client = {0};
    GError *error = NULL;

    g_client = &client;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!init_client(&client, &error)) {
        g_error("Failed to initialize user server: %s", error->message);
        g_error_free(error);
        return 1;
    }

    g_main_loop_run(client.main_loop);

    cleanup_client(&client);

    return 0;
}
