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
