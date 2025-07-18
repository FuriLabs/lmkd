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

#include <cstdio>
#include <ctype.h>
#include <fcntl.h>
#include <glib.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pwd.h>

#include "pidfd-compat.h"
#include "processwatcher.h"

struct event_handler_info {
    int data;
    void (*handler)(int data, uint32_t events, struct polling_params *poll_params);
    bool bypass_call_handler;
};

struct polling_params {
    void *dummy;
};

#define MAX_SYSTEM_USERS 32

static struct {
    int netlink_sock;
    struct processwatcher_config config;
    struct event_handler_info netlink_hinfo;
    bool initialized;
    uid_t system_uids[MAX_SYSTEM_USERS];
    int system_uid_count;
    enum memory_pressure_state current_state;
    bool netlink_active;
} pw_state = {-1, {0, nullptr, nullptr, nullptr, false}, {0, nullptr}, false, {0}, 0, MEMORY_STATE_NORMAL, false};

static const char *system_users[] = {
    "dnsmasq",
    "nobody",
    "radio",
    "geoclue",
    "systemd-network",
    "systemd-timesync",
    "systemd-resolve",
    "_apt",
    "polkitd",
    "avahi",
    "messagebus",
    "system"
};

static void cache_system_uids(void) {
    pw_state.system_uid_count = 0;

    /* Always include root (UID 0) */
    pw_state.system_uids[pw_state.system_uid_count++] = 0;

    /* Look up system users */
    for (size_t i = 0; i < sizeof(system_users) / sizeof(system_users[0]); i++) {
        struct passwd *pw = getpwnam(system_users[i]);
        if (pw && pw_state.system_uid_count < MAX_SYSTEM_USERS) {
            pw_state.system_uids[pw_state.system_uid_count++] = pw->pw_uid;
            g_debug("Cached system user %s with UID %d", system_users[i], pw->pw_uid);
        }
    }

    g_debug("Cached %d system UIDs for filtering", pw_state.system_uid_count);
}

static bool is_system_uid(uid_t uid) {
    for (int i = 0; i < pw_state.system_uid_count; i++) {
        if (pw_state.system_uids[i] == uid)
            return true;
    }
    return false;
}

static int set_proc_ev_listen(int nl_sock, int enable) {
    enum proc_cn_mcast_op mcast_op;

    char msg[NLMSG_SPACE(sizeof(struct cn_msg) + sizeof(enum proc_cn_mcast_op))];
    struct nlmsghdr *nlh = (struct nlmsghdr *)msg;
    struct cn_msg *cn_msg;

    memset(msg, 0, sizeof(msg));

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct cn_msg) + sizeof(enum proc_cn_mcast_op));
    nlh->nlmsg_type = NLMSG_DONE;
    nlh->nlmsg_flags = 0;
    nlh->nlmsg_seq = 0;
    nlh->nlmsg_pid = getpid();

    cn_msg = (struct cn_msg *)NLMSG_DATA(nlh);
    cn_msg->id.idx = CN_IDX_PROC;
    cn_msg->id.val = CN_VAL_PROC;
    cn_msg->len = sizeof(enum proc_cn_mcast_op);

    mcast_op = enable ? PROC_CN_MCAST_LISTEN : PROC_CN_MCAST_IGNORE;
    memcpy(cn_msg + 1, &mcast_op, sizeof(mcast_op));

    int ret = send(nl_sock, nlh, nlh->nlmsg_len, 0);
    if (ret == -1) {
        g_printerr("Failed to set proc event listen: %s", strerror(errno));
        return -1;
    }
    return 0;
}

uid_t processwatcher_get_process_uid(pid_t pid) {
    char path[PATH_MAX];
    struct stat st;

    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (stat(path, &st) == 0)
        return st.st_uid;
    return 0;
}

static void netlink_event_handler(int data, uint32_t events, struct polling_params *poll_params) {
    (void)data;
    (void)events;
    (void)poll_params;

    char buf[4096];
    ssize_t len;

    len = recv(pw_state.netlink_sock, buf, sizeof(buf), 0);
    if (len < 0) {
        if (errno != EINTR && errno != EAGAIN)
            g_printerr("Netlink recv failed: %s", strerror(errno));
        return;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    for (; NLMSG_OK(nlh, (unsigned)len); nlh = NLMSG_NEXT(nlh, len)) {
        if (nlh->nlmsg_type == NLMSG_NOOP)
            continue;

        if (nlh->nlmsg_type == NLMSG_ERROR) {
            g_debug("Received error message from netlink");
            continue;
        }

        if (nlh->nlmsg_type == NLMSG_DONE) {
            struct cn_msg *cn_hdr = (struct cn_msg *)NLMSG_DATA(nlh);
            if (cn_hdr->id.idx == CN_IDX_PROC && cn_hdr->id.val == CN_VAL_PROC) {
                struct proc_event *ev = (struct proc_event *)cn_hdr->data;

                switch (ev->what) {
                case PROC_EVENT_FORK: {
                    pid_t child_pid = ev->event_data.fork.child_pid;
                    g_debug("Fork event: new process %d", child_pid);

                    /* Get process info and notify callback */
                    if (child_pid > 1 && pw_state.config.on_register) {
                        uid_t uid = processwatcher_get_process_uid(child_pid);

                        /* Skip system processes */
                        if (is_system_uid(uid)) {
                            g_debug("Skipping system process %d (UID %d)", child_pid, uid);
                            break;
                        }

                        int oomadj = processwatcher_get_oom_score_adj(child_pid);
                        int pidfd = -1;

                        /* Try to get pidfd if supported */
                        if (pidfd_test_support())
                            pidfd = pidfd_open(child_pid, 0);
                        pw_state.config.on_register(child_pid, uid, oomadj, pidfd);
                    }
                } break;
                case PROC_EVENT_EXIT: {
                    pid_t pid = ev->event_data.exit.process_pid;
                    g_debug("Exit event: process %d died", pid);
                    if (pw_state.config.on_exit)
                        pw_state.config.on_exit(pid);
                } break;
                default:
                    break;
                }
            }
        }
    }
}

static bool is_numeric(const char *str) {
    if (!str || !*str)
        return false;

    while (*str) {
        if (!isdigit(*str))
            return false;
        str++;
    }
    return true;
}

int processwatcher_get_oom_score_adj(pid_t pid) {
    char path[PATH_MAX];
    char score_str[32];
    int fd, score = 0;

    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t len = read(fd, score_str, sizeof(score_str) - 1);
        if (len > 0) {
            score_str[len] = '\0';
            if (len > 0 && score_str[len - 1] == '\n')
                score_str[len - 1] = '\0';
            score = atoi(score_str);
        }
        close(fd);
    }
    return score;
}

void processwatcher_register_all_existing(void) {
    DIR *proc_dir;
    struct dirent *entry;
    pid_t pid;
    int count = 0;

    if (!pw_state.initialized || !pw_state.config.on_register)
        return;

    g_debug("Registering all existing processes...");

    proc_dir = opendir("/proc");
    if (!proc_dir) {
        g_printerr("Failed to open /proc: %s", strerror(errno));
        return;
    }

    while ((entry = readdir(proc_dir)) != NULL) {
        if (!is_numeric(entry->d_name))
            continue;

        pid = atoi(entry->d_name);
        if (pid > 1) {
            uid_t uid = processwatcher_get_process_uid(pid);

            /* Skip system processes */
            if (is_system_uid(uid)) {
                g_debug("Skipping existing system process %d (UID %d)", pid, uid);
                continue;
            }

            int oomadj = processwatcher_get_oom_score_adj(pid);
            int pidfd = -1;

            if (pidfd_test_support()) {
                pidfd = pidfd_open(pid, 0);
                /* If pidfd fails, process might be dead, skip it */
                if (pidfd < 0)
                    continue;
            }

            pw_state.config.on_register(pid, uid, oomadj, pidfd);
            count++;
        }
    }

    closedir(proc_dir);

    g_debug("Registered %d existing processes", count);
}

bool processwatcher_set_monitoring_state(enum memory_pressure_state state) {
    if (!pw_state.initialized)
        return false;

    if (pw_state.current_state == state)
        return true;

    if (state == MEMORY_STATE_PRESSURE) {
        if (!pw_state.netlink_active) {
            /* Join the process events group */
            int val = CN_IDX_PROC;
            if (setsockopt(pw_state.netlink_sock, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &val, sizeof(val)) < 0) {
                g_printerr("Failed to join netlink group: %s", strerror(errno));
                return false;
            }

            if (set_proc_ev_listen(pw_state.netlink_sock, 1) < 0)
                return false;
            pw_state.netlink_active = true;
            g_debug("Enabled netlink process monitoring");
        }
        processwatcher_register_all_existing();
    } else {
        if (pw_state.netlink_active) {
            if (set_proc_ev_listen(pw_state.netlink_sock, 0) < 0)
                return false;

            /* Leave the process events group */
            int val = CN_IDX_PROC;
            if (setsockopt(pw_state.netlink_sock, SOL_NETLINK, NETLINK_DROP_MEMBERSHIP, &val, sizeof(val)) < 0)
                g_printerr("Failed to leave netlink group: %s", strerror(errno));

            pw_state.netlink_active = false;
            g_debug("Disabled netlink process monitoring");
        }
    }

    pw_state.current_state = state;
    return true;
}

enum memory_pressure_state processwatcher_get_monitoring_state(void) {
    return pw_state.current_state;
}

bool processwatcher_init(const struct processwatcher_config *config) {
    struct sockaddr_nl addr;
    struct epoll_event epev;

    if (pw_state.initialized) {
        g_warning("Process watcher already initialized");
        return false;
    }

    if (!config || !config->maxevents) {
        g_printerr("Invalid configuration provided to process watcher");
        return false;
    }

    pw_state.config = *config;

    g_debug("Initializing process watcher...");

    /* Cache system user UIDs for filtering */
    cache_system_uids();

    pw_state.netlink_sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (pw_state.netlink_sock < 0) {
        g_printerr("Failed to create netlink socket: %s", strerror(errno));
        return false;
    }

    int flags = fcntl(pw_state.netlink_sock, F_GETFL, 0);
    if (fcntl(pw_state.netlink_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        g_printerr("Failed to set socket non-blocking: %s", strerror(errno));
        close(pw_state.netlink_sock);
        pw_state.netlink_sock = -1;
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    /* Don't join any groups initially */
    addr.nl_groups = 0;

    if (bind(pw_state.netlink_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        g_printerr("Failed to bind netlink socket: %s", strerror(errno));
        close(pw_state.netlink_sock);
        pw_state.netlink_sock = -1;
        return false;
    }

    if (pw_state.config.epollfd >= 0) {
        pw_state.netlink_hinfo.data = 0;
        pw_state.netlink_hinfo.handler = netlink_event_handler;
        pw_state.netlink_hinfo.bypass_call_handler = true;
        epev.events = EPOLLIN;
        epev.data.ptr = &pw_state.netlink_hinfo;

        if (epoll_ctl(pw_state.config.epollfd, EPOLL_CTL_ADD, pw_state.netlink_sock, &epev) != 0) {
            g_printerr("Failed to add netlink socket to epoll: %s", strerror(errno));
            close(pw_state.netlink_sock);
            pw_state.netlink_sock = -1;
            return false;
        }

        (*pw_state.config.maxevents)++;
    }

    pw_state.initialized = true;
    pw_state.current_state = MEMORY_STATE_NORMAL;
    pw_state.netlink_active = false;

    g_debug("Process watcher initialized successfully");

    return true;
}

void processwatcher_cleanup(void) {
    if (!pw_state.initialized)
        return;

    g_debug("Cleaning up process watcher...");

    if (pw_state.netlink_sock >= 0) {
        /* Remove from epoll if it was added */
        if (pw_state.config.epollfd >= 0) {
            struct epoll_event epev;
            epoll_ctl(pw_state.config.epollfd, EPOLL_CTL_DEL, pw_state.netlink_sock, &epev);
            (*pw_state.config.maxevents)--;
        }

        /* Disable process event listening */
        if (pw_state.netlink_active)
            set_proc_ev_listen(pw_state.netlink_sock, 0);

        close(pw_state.netlink_sock);
        pw_state.netlink_sock = -1;
    }

    memset(&pw_state.config, 0, sizeof(pw_state.config));
    memset(&pw_state.netlink_hinfo, 0, sizeof(pw_state.netlink_hinfo));
    pw_state.system_uid_count = 0;
    memset(pw_state.system_uids, 0, sizeof(pw_state.system_uids));
    pw_state.initialized = false;
    pw_state.current_state = MEMORY_STATE_NORMAL;
    pw_state.netlink_active = false;

    g_debug("Process watcher cleanup complete");
}

bool processwatcher_is_initialized(void) {
    return pw_state.initialized;
}
