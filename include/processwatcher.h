/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef PROCESSWATCHER_H
#define PROCESSWATCHER_H

#ifdef __cplusplus
extern "C" {
#endif

struct proc;
struct event_handler_info;
struct polling_params;

typedef void (*process_register_callback_t)(pid_t pid, uid_t uid, int oomadj, int pidfd);
typedef void (*process_exit_callback_t)(pid_t pid);

struct processwatcher_config {
    int epollfd;                             // epoll fd to add netlink socket to
    int *maxevents;                          // pointer to maxevents counter
    process_register_callback_t on_register; // callback for new processes
    process_exit_callback_t on_exit;         // callback for process exit
    bool enable_debug;                       // enable debug logging
};

/**
 * Memory pressure states for dynamic process monitoring
 */
enum memory_pressure_state {
    MEMORY_STATE_NORMAL,    /* No memory pressure - netlink monitoring disabled */
    MEMORY_STATE_PRESSURE,  /* Memory pressure detected - netlink monitoring active */
};

/**
 * Initialize the process watcher
 * @param config Configuration structure
 * @return true on success, false on failure
 */
bool processwatcher_init(const struct processwatcher_config *config);

/**
 * Cleanup the process watcher
 */
void processwatcher_cleanup(void);

/**
 * Register all currently running processes
 * Calls the register callback for each process found
 */
void processwatcher_register_all_existing(void);

/**
 * Get OOM score adjustment for a process
 * @param pid Process ID
 * @return OOM score adjustment value
 */
int processwatcher_get_oom_score_adj(pid_t pid);

/**
 * Get UID of a process
 * @param pid Process ID
 * @return UID of the process, 0 if unable to determine
 */
uid_t processwatcher_get_process_uid(pid_t pid);

/**
 * Check if process watcher is initialized
 * @return true if initialized, false otherwise
 */
bool processwatcher_is_initialized(void);

/**
* Set the memory pressure monitoring state
* @param state MEMORY_STATE_NORMAL to disable monitoring, MEMORY_STATE_PRESSURE to enable
* @return true on success, false on failure
*/
bool processwatcher_set_monitoring_state(enum memory_pressure_state state);

/**
* Get the current memory pressure monitoring state
* @return Current monitoring state
*/
enum memory_pressure_state processwatcher_get_monitoring_state(void);

#ifdef __cplusplus
}
#endif

#endif /* PROCESSWATCHER_H */
