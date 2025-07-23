/*
 * Copyright (C) 2025 Furi Labs
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "dbus.h"

LmkdDBusService *g_lmkd_dbus_service = NULL;
static guint32 total_kills = 0;
static enum dbus_pressure_state current_pressure_state = DBUS_PRESSURE_STATE_NORMAL;

static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='io.furios.Lmkd'>"
    "    <signal name='ProcessKilled'>"
    "      <arg type='i' name='pid'/>"
    "      <arg type='i' name='uid'/>"
    "      <arg type='i' name='oom_score_adj'/>"
    "      <arg type='s' name='process_name'/>"
    "      <arg type='s' name='kill_reason'/>"
    "      <arg type='x' name='rss_kb'/>"
    "      <arg type='x' name='swap_kb'/>"
    "      <arg type='i' name='thrashing'/>"
    "      <arg type='i' name='max_thrashing'/>"
    "      <arg type='x' name='free_mem_kb'/>"
    "      <arg type='x' name='free_swap_kb'/>"
    "    </signal>"
    "    <signal name='PressureStateChanged'>"
    "      <arg type='u' name='old_state'/>"
    "      <arg type='u' name='new_state'/>"
    "      <arg type='s' name='old_state_name'/>"
    "      <arg type='s' name='new_state_name'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static const gchar *pressure_state_to_string(enum dbus_pressure_state state) {
    switch (state) {
        case DBUS_PRESSURE_STATE_NORMAL:
            return "normal";
        case DBUS_PRESSURE_STATE_PRESSURE:
            return "pressure";
        default:
            return "unknown";
    }
}

void lmkd_dbus_emit_process_killed(LmkdDBusService *service,
                                   int32_t pid,
                                   int32_t uid,
                                   int32_t oom_score_adj,
                                   const gchar *process_name,
                                   const gchar *kill_reason,
                                   int64_t rss_kb,
                                   int64_t swap_kb,
                                   int32_t thrashing,
                                   int32_t max_thrashing,
                                   int64_t free_mem_kb,
                                   int64_t free_swap_kb) {
    if (!service || !service->connection) {
        g_debug("D-Bus service not initialized, cannot emit ProcessKilled signal");
        return;
    }

    GError *error = NULL;

    total_kills++;

    if (!process_name)
        process_name = "<unknown>";
    if (!kill_reason)
        kill_reason = "unknown";

    g_debug("Emitting ProcessKilled signal: pid=%d, uid=%d, oom_score_adj=%d, "
            "process_name='%s', kill_reason='%s', rss_kb=%" G_GINT64_FORMAT ", "
            "swap_kb=%" G_GINT64_FORMAT ", thrashing=%d%%, max_thrashing=%d%%, "
            "free_mem_kb=%" G_GINT64_FORMAT ", free_swap_kb=%" G_GINT64_FORMAT,
            pid, uid, oom_score_adj, process_name, kill_reason, rss_kb, swap_kb,
            thrashing, max_thrashing, free_mem_kb, free_swap_kb);

    gboolean result = g_dbus_connection_emit_signal(service->connection,
                                                    NULL, /* destination (broadcast) */
                                                    DBUS_OBJECT_PATH,
                                                    DBUS_INTERFACE_NAME,
                                                    "ProcessKilled",
                                                    g_variant_new("(iiissxxiixx)",
                                                                  pid,
                                                                  uid,
                                                                  oom_score_adj,
                                                                  process_name,
                                                                  kill_reason,
                                                                  rss_kb,
                                                                  swap_kb,
                                                                  thrashing,
                                                                  max_thrashing,
                                                                  free_mem_kb,
                                                                  free_swap_kb),
                                                    &error);

    if (!result) {
        g_warning("Failed to emit ProcessKilled signal: %s", error->message);
        g_error_free(error);
    } else {
        g_debug("ProcessKilled signal emitted successfully (total kills: %u)", total_kills);
    }
}

void lmkd_dbus_emit_pressure_state_changed(LmkdDBusService *service,
                                           enum dbus_pressure_state old_state,
                                           enum dbus_pressure_state new_state) {
    if (!service || !service->connection) {
        g_debug("D-Bus service not initialized, cannot emit PressureStateChanged signal");
        return;
    }

    /* Don't emit signal if state hasn't actually changed */
    if (old_state == new_state) {
        g_debug("Pressure state unchanged (%s), not emitting signal",
                pressure_state_to_string(new_state));
        return;
    }

    GError *error = NULL;
    const gchar *old_state_name = pressure_state_to_string(old_state);
    const gchar *new_state_name = pressure_state_to_string(new_state);

    g_debug("Emitting PressureStateChanged signal: %s (%u) -> %s (%u)",
            old_state_name, old_state, new_state_name, new_state);

    gboolean result = g_dbus_connection_emit_signal(service->connection,
                                                    NULL, /* destination (broadcast) */
                                                    DBUS_OBJECT_PATH,
                                                    DBUS_INTERFACE_NAME,
                                                    "PressureStateChanged",
                                                    g_variant_new("(uuss)",
                                                                  old_state,
                                                                  new_state,
                                                                  old_state_name,
                                                                  new_state_name),
                                                    &error);

    if (!result) {
        g_warning("Failed to emit PressureStateChanged signal: %s", error->message);
        g_error_free(error);
    } else {
        /* Update current state tracking */
        current_pressure_state = new_state;
        g_debug("PressureStateChanged signal emitted successfully: %s -> %s",
                old_state_name, new_state_name);
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    NULL,
    NULL,
    NULL
};

gboolean lmkd_dbus_service_init(LmkdDBusService *service, GError **error) {
    GDBusNodeInfo *introspection_data;
    GError *local_error = NULL;

    g_debug("Initializing LMKD D-Bus service");

    if (!service) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Service structure is NULL");
        return FALSE;
    }

    service->connection = NULL;
    service->registration_id = 0;
    current_pressure_state = DBUS_PRESSURE_STATE_NORMAL;

    /* Connect to system bus */
    service->connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &local_error);
    if (!service->connection) {
        g_warning("Failed to connect to system bus: %s", local_error->message);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    g_debug("Connected to system bus successfully");

    introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &local_error);
    if (!introspection_data) {
        g_warning("Failed to parse introspection XML: %s", local_error->message);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    /* Register object */
    service->registration_id = g_dbus_connection_register_object(service->connection,
                                                                 DBUS_OBJECT_PATH,
                                                                 introspection_data->interfaces[0],
                                                                 &interface_vtable,
                                                                 service,  /* user_data */
                                                                 NULL,     /* user_data_free_func */
                                                                 &local_error);

    g_dbus_node_info_unref(introspection_data);

    if (service->registration_id == 0) {
        g_warning("Failed to register object: %s", local_error->message);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    g_debug("Object registered with ID: %u", service->registration_id);

    /* Request service name */
    GVariant *result = g_dbus_connection_call_sync(service->connection,
                                                   "org.freedesktop.DBus",
                                                   "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus",
                                                   "RequestName",
                                                   g_variant_new("(su)", DBUS_SERVICE_NAME, 0),
                                                   G_VARIANT_TYPE("(u)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   -1,
                                                   NULL,
                                                   &local_error);

    if (!result) {
        g_warning("Failed to request service name: %s", local_error->message);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    guint32 request_result;
    g_variant_get(result, "(u)", &request_result);
    g_variant_unref(result);

    if (request_result != 1) {
        g_warning("Failed to acquire service name, result: %u", request_result);
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                    "Failed to acquire service name '%s'", DBUS_SERVICE_NAME);
        return FALSE;
    }

    g_debug("Service name '%s' acquired successfully", DBUS_SERVICE_NAME);
    g_debug("LMKD D-Bus service initialized successfully");

    return TRUE;
}

void lmkd_dbus_service_cleanup(LmkdDBusService *service) {
    if (!service)
        return;

    g_debug("Cleaning up LMKD D-Bus service");

    if (service->registration_id > 0) {
        g_dbus_connection_unregister_object(service->connection, service->registration_id);
        service->registration_id = 0;
        g_debug("D-Bus object unregistered");
    }

    if (service->connection) {
        g_object_unref(service->connection);
        service->connection = NULL;
        g_debug("D-Bus connection closed");
    }

    g_debug("LMKD D-Bus service cleanup completed");
}

gboolean lmkd_dbus_is_connected(LmkdDBusService *service) {
    if (!service)
        return FALSE;

    return (service->connection != NULL && service->registration_id > 0);
}
