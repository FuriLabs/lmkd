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

#include "pidfd-compat.h"
#include "processwatcher.h"

struct event_handler_info {
    int data;
    void (*handler)(int data, uint32_t events, struct polling_params *poll_params);
};

struct polling_params {
    void *dummy;
};

static struct {
    int netlink_sock;
    struct processwatcher_config config;
    struct event_handler_info netlink_hinfo;
    bool initialized;
} pw_state = {-1, {0, nullptr, nullptr, nullptr, false}, {0, nullptr}, false};

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
        if (pw_state.config.enable_debug)
            g_printerr("Failed to set proc event listen: %s", strerror(errno));
        return -1;
    }
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
        if (errno != EINTR && errno != EAGAIN) {
            if (pw_state.config.enable_debug)
                g_printerr("Netlink recv failed: %s", strerror(errno));
        }
        return;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    for (; NLMSG_OK(nlh, (unsigned)len); nlh = NLMSG_NEXT(nlh, len)) {
        if (nlh->nlmsg_type == NLMSG_NOOP)
            continue;

        if (nlh->nlmsg_type == NLMSG_ERROR) {
            if (pw_state.config.enable_debug)
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
                    if (pw_state.config.enable_debug)
                        g_debug("Fork event: new process %d", child_pid);

                    /* Get process info and notify callback */
                    if (child_pid > 1 && pw_state.config.on_register) {
                        uid_t uid = processwatcher_get_process_uid(child_pid);
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
                    if (pw_state.config.enable_debug)
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

uid_t processwatcher_get_process_uid(pid_t pid) {
    char path[PATH_MAX];
    struct stat st;

    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (stat(path, &st) == 0)
        return st.st_uid;

    return 0;
}

void processwatcher_register_all_existing(void) {
    DIR *proc_dir;
    struct dirent *entry;
    pid_t pid;
    int count = 0;

    if (!pw_state.initialized || !pw_state.config.on_register)
        return;
    if (pw_state.config.enable_debug)
        g_debug("Registering all existing processes...");

    proc_dir = opendir("/proc");
    if (!proc_dir) {
        if (pw_state.config.enable_debug)
            g_printerr("Failed to open /proc: %s", strerror(errno));
        return;
    }

    while ((entry = readdir(proc_dir)) != NULL) {
        if (!is_numeric(entry->d_name))
            continue;

        pid = atoi(entry->d_name);
        if (pid > 1) {
            uid_t uid = processwatcher_get_process_uid(pid);
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

    if (pw_state.config.enable_debug)
        g_debug("Registered %d existing processes", count);
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

    if (pw_state.config.enable_debug)
        g_debug("Initializing process watcher...");

    pw_state.netlink_sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (pw_state.netlink_sock < 0) {
        if (pw_state.config.enable_debug)
            g_printerr("Failed to create netlink socket: %s", strerror(errno));
        return false;
    }

    int flags = fcntl(pw_state.netlink_sock, F_GETFL, 0);
    if (fcntl(pw_state.netlink_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (pw_state.config.enable_debug) {
            g_printerr("Failed to set socket non-blocking: %s", strerror(errno));
        }
        close(pw_state.netlink_sock);
        pw_state.netlink_sock = -1;
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = CN_IDX_PROC;

    if (bind(pw_state.netlink_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (pw_state.config.enable_debug)
            g_printerr("Failed to bind netlink socket: %s", strerror(errno));
        close(pw_state.netlink_sock);
        pw_state.netlink_sock = -1;
        return false;
    }

    /* Enable process event listening */
    if (set_proc_ev_listen(pw_state.netlink_sock, 1) < 0) {
        close(pw_state.netlink_sock);
        pw_state.netlink_sock = -1;
        return false;
    }

    if (pw_state.config.epollfd >= 0) {
        pw_state.netlink_hinfo.data = 0;
        pw_state.netlink_hinfo.handler = netlink_event_handler;
        epev.events = EPOLLIN;
        epev.data.ptr = &pw_state.netlink_hinfo;

        if (epoll_ctl(pw_state.config.epollfd, EPOLL_CTL_ADD, pw_state.netlink_sock, &epev) != 0) {
            if (pw_state.config.enable_debug)
                g_printerr("Failed to add netlink socket to epoll: %s", strerror(errno));
            set_proc_ev_listen(pw_state.netlink_sock, 0);
            close(pw_state.netlink_sock);
            pw_state.netlink_sock = -1;
            return false;
        }

        (*pw_state.config.maxevents)++;
    }

    pw_state.initialized = true;

    if (pw_state.config.enable_debug)
        g_debug("Process watcher initialized successfully");
    return true;
}

void processwatcher_cleanup(void) {
    if (!pw_state.initialized)
        return;
    if (pw_state.config.enable_debug)
        g_debug("Cleaning up process watcher...");

    if (pw_state.netlink_sock >= 0) {
        /* Remove from epoll if it was added */
        if (pw_state.config.epollfd >= 0) {
            struct epoll_event epev;
            epoll_ctl(pw_state.config.epollfd, EPOLL_CTL_DEL, pw_state.netlink_sock, &epev);
            (*pw_state.config.maxevents)--;
        }

        /* Disable process event listening */
        set_proc_ev_listen(pw_state.netlink_sock, 0);

        close(pw_state.netlink_sock);
        pw_state.netlink_sock = -1;
    }

    memset(&pw_state.config, 0, sizeof(pw_state.config));
    memset(&pw_state.netlink_hinfo, 0, sizeof(pw_state.netlink_hinfo));
    pw_state.initialized = false;

    if (pw_state.config.enable_debug)
        g_debug("Process watcher cleanup complete");
}

bool processwatcher_is_initialized(void) {
    return pw_state.initialized;
}
