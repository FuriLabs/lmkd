/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2021 Google, Inc
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#pragma once

#include <atomic>
#include <fcntl.h>
#include <sys/resource.h>

class Watchdog {
  private:
    int timeout_;
    timer_t timer_;
    std::atomic<bool> timer_created_;
    void (*callback_)();

  public:
    Watchdog(int timeout, void (*callback)()) : timeout_(timeout), timer_created_(false), callback_(callback) {}

    bool init();
    bool start();
    bool stop();
    /* used by the watchdog_main */
    bool create_timer(sigset_t &sigset);
    void bite() const {
        if (callback_)
            callback_();
    }
};
