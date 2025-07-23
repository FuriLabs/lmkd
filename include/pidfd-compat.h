/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef PIDFD_COMPAT_H
#define PIDFD_COMPAT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef __NR_pidfd_open
#if defined(__x86_64__)
#define __NR_pidfd_open 434
#define __NR_pidfd_send_signal 424
#elif defined(__aarch64__)
#define __NR_pidfd_open 434
#define __NR_pidfd_send_signal 424
#elif defined(__arm__)
#define __NR_pidfd_open 434
#define __NR_pidfd_send_signal 424
#elif defined(__i386__)
#define __NR_pidfd_open 434
#define __NR_pidfd_send_signal 424
#else
#define __NR_pidfd_open 434
#define __NR_pidfd_send_signal 424
#endif
#endif

#define pidfd_open(pid, flags) syscall(__NR_pidfd_open, (pid), (flags))
#define pidfd_send_signal(pidfd, sig, info, flags) syscall(__NR_pidfd_send_signal, (pidfd), (sig), (info), (flags))

static inline int pidfd_test_support(void) {
    int fd = syscall(__NR_pidfd_open, getpid(), 0);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return (errno != ENOSYS);
}

#endif /* PIDFD_COMPAT_H */
