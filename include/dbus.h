/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef DBUS_H
#define DBUS_H

#include <gio/gio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBUS_SERVICE_NAME "io.furios.Lmkd"
#define DBUS_OBJECT_PATH "/io/furios/Lmkd"
#define DBUS_INTERFACE_NAME "io.furios.Lmkd"

typedef struct {
    GDBusConnection *connection;
    guint registration_id;
} LmkdDBusService;

extern LmkdDBusService *g_lmkd_dbus_service;

/**
 * Memory pressure states
 */
enum dbus_pressure_state {
    DBUS_PRESSURE_STATE_NORMAL = 0,
    DBUS_PRESSURE_STATE_PRESSURE = 1
};

/**
 * Initialize the LMKD D-Bus service
 * @param service Service structure to initialize
 * @param error Error pointer for error details
 * @return TRUE on success, FALSE on failure
 */
gboolean lmkd_dbus_service_init(LmkdDBusService *service, GError **error);

/**
 * Cleanup the LMKD D-Bus service
 * @param service Service structure to cleanup
 */
void lmkd_dbus_service_cleanup(LmkdDBusService *service);

/**
 * Emit ProcessKilled D-Bus signal
 * @param service D-Bus service instance
 * @param pid Process ID that was killed
 * @param uid User ID of the killed process
 * @param oom_score_adj OOM score adjustment of the killed process
 * @param process_name Name of the killed process
 * @param kill_reason Reason for killing the process
 * @param rss_kb RSS memory usage in KB
 * @param swap_kb Swap memory usage in KB
 * @param thrashing Current thrashing percentage
 * @param max_thrashing Maximum thrashing percentage
 * @param free_mem_kb Free memory in KB
 * @param free_swap_kb Free swap in KB
 */
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
                                  int64_t free_swap_kb);

/**
 * Emit PressureStateChanged D-Bus signal
 * @param service D-Bus service instance
 * @param old_state Previous pressure state
 * @param new_state New pressure state
 */
void lmkd_dbus_emit_pressure_state_changed(LmkdDBusService *service,
                                           enum dbus_pressure_state old_state,
                                           enum dbus_pressure_state new_state);

/**
 * Check if D-Bus service is initialized and connected
 * @param service D-Bus service instance
 * @return TRUE if initialized and connected, FALSE otherwise
 */
gboolean lmkd_dbus_is_connected(LmkdDBusService *service);

#ifdef __cplusplus
}
#endif

#endif /* DBUS_H */
