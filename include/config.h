/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2013 The Android Open Source Project
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef __LMKD_CONFIG_H__
#define __LMKD_CONFIG_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the configuration system
 * @return true on success, false on failure
 */
bool config_init(void);

/**
 * Cleanup configuration resources
 */
void config_cleanup(void);

/**
 * Get a boolean configuration value
 * @param key Configuration key name
 * @param default_value Value to return if key is not found
 * @return Configuration value or default_value
 */
bool config_get_bool(const char *key, bool default_value);

/**
 * Get a 32-bit integer configuration value
 * @param key Configuration key name
 * @param default_value Value to return if key is not found
 * @return Configuration value or default_value
 */
int32_t config_get_int32(const char *key, int32_t default_value);

/**
 * Get a 64-bit integer configuration value
 * @param key Configuration key name
 * @param default_value Value to return if key is not found
 * @return Configuration value or default_value
 */
int64_t config_get_int64(const char *key, int64_t default_value);

#ifdef __cplusplus
}
#endif

#endif /* __LMKD_CONFIG_H__ */
