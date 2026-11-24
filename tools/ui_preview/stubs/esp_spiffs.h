#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
typedef struct {
    const char* base_path;
    const char* partition_label;
    size_t max_files;
    bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;
static inline esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t*) { return ESP_FAIL; }
