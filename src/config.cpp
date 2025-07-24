/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 */

#include "config.h"

#include <glib.h>

/* Configuration system */
static GKeyFile *config = NULL;
static bool config_loaded = false;

static void load_config(void) {
    if (config_loaded)
        return;

    config = g_key_file_new();
    GError *error = NULL;

    /* /etc first, then device-specific, then default */
    const char *config_paths[] = {
        "/etc/lmkd/lmkd.conf",
        "/usr/lib/furios/device/lmkd.conf",
        "/usr/share/lmkd/lmkd.conf",
        NULL
    };

    for (int i = 0; config_paths[i]; i++) {
        if (g_file_test(config_paths[i], G_FILE_TEST_EXISTS)) {
            if (g_key_file_load_from_file(config, config_paths[i], G_KEY_FILE_NONE, &error)) {
                g_debug("Loaded configuration from %s", config_paths[i]);
                break;
            } else {
                g_printerr("Failed to load config from %s: %s",
                           config_paths[i],
                           error->message);
                g_error_free(error);
                error = NULL;
            }
        }
    }
    config_loaded = true;
}

bool config_init(void) {
    load_config();
    return config != NULL;
}

void config_cleanup(void) {
    if (config) {
        g_key_file_free(config);
        config = NULL;
    }
    config_loaded = false;
}

bool config_get_bool(const char *key, bool default_value) {
    load_config();
    GError *error = NULL;

    if (!config)
        return default_value;

    gboolean value = g_key_file_get_boolean(config, "lmkd", key, &error);
    if (error) {
        g_error_free(error);
        return default_value;
    }
    return value;
}

int32_t config_get_int32(const char *key, int32_t default_value) {
    load_config();
    GError *error = NULL;

    if (!config)
        return default_value;

    gint value = g_key_file_get_integer(config, "lmkd", key, &error);
    if (error) {
        g_error_free(error);
        return default_value;
    }
    return (int32_t)value;
}

int64_t config_get_int64(const char *key, int64_t default_value) {
    load_config();
    GError *error = NULL;

    if (!config)
        return default_value;

    gint64 value = g_key_file_get_int64(config, "lmkd", key, &error);
    if (error) {
        g_error_free(error);
        return default_value;
    }
    return value;
}
