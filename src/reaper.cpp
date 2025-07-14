/*
 *  Copyright 2021 Google, Inc
 *  Copyright 2025 Furi Labs
 *  Copyright 2025 Bardia Moshiri <bardia@furilabs.com>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <cstdio>
#include <fcntl.h>
#include <glib.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>

#include "pidfd-compat.h"
#include "reaper.h"

#define NS_PER_SEC 1000000000LL
#define MS_PER_SEC 1000LL
#define NS_PER_MS (NS_PER_SEC / MS_PER_SEC)
#define THREAD_POOL_SIZE 2

#define HIGH_PRIORITY -10
#define NORMAL_PRIORITY 0

#ifndef __NR_process_mrelease
#define __NR_process_mrelease 448
#endif

static int process_mrelease(int pidfd, unsigned int flags) {
    return syscall(__NR_process_mrelease, pidfd, flags);
}

static inline long get_time_diff_ms(struct timespec *from,
                                    struct timespec *to) {
    return (to->tv_sec - from->tv_sec) * (long)MS_PER_SEC +
           (to->tv_nsec - from->tv_nsec) / (long)NS_PER_MS;
}

static bool set_process_cgroup(int pid, const char *cgroup_path) {
    char path[PATH_MAX];
    char pid_str[32];
    int fd;

    snprintf(path, sizeof(path), "%s/cgroup.procs", cgroup_path);
    snprintf(pid_str, sizeof(pid_str), "%d", pid);

    fd = open(path, O_WRONLY);
    if (fd < 0)
        return false;

    bool success = (write(fd, pid_str, strlen(pid_str)) > 0);
    close(fd);
    return success;
}

static void set_process_group_and_prio(int pid, int prio) {
    DIR *d;
    char proc_path[PATH_MAX];
    struct dirent *de;

    /* Try to move to performance cgroup if available */
    if (!set_process_cgroup(pid, "/sys/fs/cgroup/cpuset/foreground")) {
        /* Fallback to setting CPU affinity to all cores */
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        for (int i = 0; i < get_nprocs(); i++) {
            CPU_SET(i, &cpu_set);
        }
        sched_setaffinity(pid, sizeof(cpu_set), &cpu_set);
    }

    snprintf(proc_path, sizeof(proc_path), "/proc/%d/task", pid);
    if (!(d = opendir(proc_path))) {
        g_warning("Failed to open %s; errno=%d: process pid(%d) might have died",
                  proc_path,
                  errno,
                  pid);
        return;
    }

    while ((de = readdir(d))) {
        int t_pid;

        if (de->d_name[0] == '.')
            continue;
        t_pid = atoi(de->d_name);

        if (!t_pid) {
            g_warning("Failed to get t_pid for '%s' of pid(%d)", de->d_name, pid);
            continue;
        }

        if (setpriority(PRIO_PROCESS, t_pid, prio) && errno != ESRCH)
            g_warning("Unable to raise priority of killing t_pid (%d): errno=%d", t_pid, errno);
    }
    closedir(d);
}

static void *reaper_main(void *param) {
    Reaper *reaper = static_cast<Reaper *>(param);
    struct timespec start_tm, end_tm;
    struct Reaper::target_proc target;
    pid_t tid = gettid();

    /* Set high priority for reaper thread */
    if (setpriority(PRIO_PROCESS, tid, HIGH_PRIORITY))
        g_warning("Unable to raise priority of the reaper thread (%d): errno=%d", tid, errno);

    /* Set CPU affinity to all cores */
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (int i = 0; i < get_nprocs(); i++) {
        CPU_SET(i, &cpu_set);
    }

    if (sched_setaffinity(tid, sizeof(cpu_set), &cpu_set))
        g_warning("Failed to set CPU affinity for reaper thread: %s", strerror(errno));

    for (;;) {
        target = reaper->dequeue_request();

        if (reaper->debug_enabled())
            clock_gettime(CLOCK_MONOTONIC_COARSE, &start_tm);

        if (pidfd_send_signal(target.pidfd, SIGKILL, NULL, 0)) {
            reaper->notify_kill_failure(target.pid);
            goto done;
        }

        set_process_group_and_prio(target.pid, NORMAL_PRIORITY);

        if (process_mrelease(target.pidfd, 0)) {
            g_printerr("process_mrelease %d failed: %s", target.pid, strerror(errno));
            goto done;
        }

        if (reaper->debug_enabled()) {
            clock_gettime(CLOCK_MONOTONIC_COARSE, &end_tm);
            g_debug("Process %d was reaped in %ldms", target.pid, get_time_diff_ms(&start_tm, &end_tm));
        }

    done:
        close(target.pidfd);
        reaper->request_complete();
    }

    return NULL;
}

bool Reaper::is_reaping_supported() {
    static enum {
        UNKNOWN,
        SUPPORTED,
        UNSUPPORTED
    } reap_support = UNKNOWN;

    if (reap_support == UNKNOWN) {
        if (process_mrelease(-1, 0) && errno == ENOSYS)
            reap_support = UNSUPPORTED;
        else
            reap_support = SUPPORTED;
    }
    return reap_support == SUPPORTED;
}

bool Reaper::init(int comm_fd) {
    char name[16];
    struct sched_param param = {
        .sched_priority = 0,
    };

    if (thread_cnt_ > 0)
        return false;

    thread_pool_ = new pthread_t[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&thread_pool_[thread_cnt_], NULL, reaper_main, this)) {
            g_printerr("pthread_create failed: %s", strerror(errno));
            continue;
        }

        /* set normal scheduling policy for the reaper thread */
        if (pthread_setschedparam(thread_pool_[thread_cnt_], SCHED_OTHER, &param))
            g_warning("set SCHED_OTHER failed %s", strerror(errno));
        snprintf(name, sizeof(name), "lmkd_reaper%d", thread_cnt_);
        if (pthread_setname_np(thread_pool_[thread_cnt_], name))
            g_warning("pthread_setname_np failed: %s", strerror(errno));
        thread_cnt_++;
    }

    if (!thread_cnt_) {
        delete[] thread_pool_;
        return false;
    }

    queue_.reserve(thread_cnt_);
    comm_fd_ = comm_fd;
    return true;
}

bool Reaper::async_kill(const struct target_proc &target) {
    if (target.pidfd == -1)
        return false;
    if (!thread_cnt_)
        return false;

    mutex_.lock();
    if (active_requests_ >= thread_cnt_) {
        mutex_.unlock();
        return false;
    }
    active_requests_++;

    /* Duplicate pidfd instead of reusing the original one to avoid synchronization and refcounting
     * when both reaper and main threads are using or closing the pidfd */
    queue_.push_back({dup(target.pidfd), target.pid, target.uid});
    /* Wake up a reaper thread */
    cond_.notify_one();
    mutex_.unlock();

    return true;
}

int Reaper::kill(const struct target_proc &target, bool synchronous) {
    /* CAP_KILL required */
    if (target.pidfd < 0)
        return ::kill(target.pid, SIGKILL);

    if (!synchronous && async_kill(target))
        /* we assume the kill will be successful and if it fails we will be notified */
        return 0;

    int result = pidfd_send_signal(target.pidfd, SIGKILL, NULL, 0);
    if (result)
        return result;

    return 0;
}

Reaper::target_proc Reaper::dequeue_request() {
    struct target_proc target;
    std::unique_lock<std::mutex> lock(mutex_);

    while (queue_.empty()) {
        cond_.wait(lock);
    }
    target = queue_.back();
    queue_.pop_back();

    return target;
}

void Reaper::request_complete() {
    std::scoped_lock<std::mutex> lock(mutex_);
    active_requests_--;
}

void Reaper::notify_kill_failure(int pid) {
    std::scoped_lock<std::mutex> lock(mutex_);

    g_printerr("Failed to kill process %d", pid);
    if (TEMP_FAILURE_RETRY(write(comm_fd_, &pid, sizeof(pid))) != sizeof(pid))
        g_printerr("thread communication write failed: %s", strerror(errno));
}
