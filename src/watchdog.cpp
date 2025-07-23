/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2021 Google, Inc
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#include <errno.h>
#include <glib.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "watchdog.h"

static bool set_thread_to_performance_cores(pid_t tid) {
    char tid_str[32];
    int fd;

    /* Try to move to performance cgroup if available */
    const char *cgroup_paths[] = {
        "/sys/fs/cgroup/cpuset/foreground/cgroup.procs",
        "/sys/fs/cgroup/cpuset/performance/cgroup.procs",
        "/sys/fs/cgroup/cpuset.cpus.effective", /* cgroup v2 */
        NULL
    };

    snprintf(tid_str, sizeof(tid_str), "%d", tid);

    for (int i = 0; cgroup_paths[i]; i++) {
        fd = open(cgroup_paths[i], O_WRONLY);
        if (fd >= 0) {
            bool success = (write(fd, tid_str, strlen(tid_str)) > 0);
            close(fd);
            if (success)
                return true;
        }
    }

    /* Fallback to setting CPU affinity to all cores */
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (int i = 0; i < get_nprocs(); i++) {
        CPU_SET(i, &cpu_set);
    }

    return (sched_setaffinity(tid, sizeof(cpu_set), &cpu_set) == 0);
}

static void *watchdog_main(void *param) {
    Watchdog *watchdog = static_cast<Watchdog *>(param);
    sigset_t sigset;
    int signum;
    pid_t tid = gettid();

    /* Try to ensure the thread uses performance cores */
    if (!set_thread_to_performance_cores(tid))
        g_debug("Failed to assign performance cpuset to the watchdog thread");

    /* Set high priority for watchdog thread */
    if (setpriority(PRIO_PROCESS, tid, -10))
        g_debug("Unable to raise priority of watchdog thread (%d): errno=%d", tid, errno);

    if (!watchdog->create_timer(sigset)) {
        g_printerr("Watchdog timer creation failed!");
        return NULL;
    }

    while (true) {
        if (sigwait(&sigset, &signum) == -1)
            g_printerr("sigwait failed: %s", strerror(errno));
        watchdog->bite();
    }

    return NULL;
}

bool Watchdog::init() {
    pthread_t thread;

    if (pthread_create(&thread, NULL, watchdog_main, this)) {
        g_printerr("pthread_create failed: %s", strerror(errno));
        return false;
    }
    if (pthread_setname_np(thread, "lmkd_watchdog"))
        g_warning("pthread_setname_np failed: %s", strerror(errno));

    return true;
}

bool Watchdog::start() {
    /* Start the timer and keep it active until it's disarmed */
    struct itimerspec new_timer;

    if (!timer_created_)
        return false;

    new_timer.it_value.tv_sec = timeout_;
    new_timer.it_value.tv_nsec = 0;
    new_timer.it_interval.tv_sec = timeout_;
    new_timer.it_interval.tv_nsec = 0;

    if (timer_settime(timer_, 0, &new_timer, NULL)) {
        g_printerr("timer_settime failed: %s", strerror(errno));
        return false;
    }

    return true;
}

bool Watchdog::stop() {
    struct itimerspec new_timer = {};

    if (!timer_created_)
        return false;

    if (timer_settime(timer_, 0, &new_timer, NULL)) {
        g_printerr("timer_settime failed: %s", strerror(errno));
        return false;
    }

    return true;
}

bool Watchdog::create_timer(sigset_t &sigset) {
    struct sigevent sevent;

    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &sigset, NULL)) {
        g_printerr("sigprocmask failed: %s", strerror(errno));
        return false;
    }

    sevent.sigev_notify = SIGEV_THREAD_ID;
    sevent.sigev_value.sival_ptr = &timer_;
    sevent.sigev_signo = SIGALRM;

    /* For SIGEV_THREAD_ID, we need to use the _sigev_un union */
    sevent._sigev_un._tid = gettid();

    if (timer_create(CLOCK_MONOTONIC, &sevent, &timer_)) {
        g_printerr("timer_create failed: %s", strerror(errno));
        return false;
    }

    timer_created_ = true;
    return true;
}
