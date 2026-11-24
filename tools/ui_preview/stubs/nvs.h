#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*) { return ESP_FAIL; }
static inline void nvs_close(nvs_handle_t) {}
static inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
static inline esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*) { return ESP_FAIL; }
static inline esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { return ESP_OK; }
static inline esp_err_t nvs_get_i32(nvs_handle_t, const char*, int32_t*) { return ESP_FAIL; }
static inline esp_err_t nvs_set_i32(nvs_handle_t, const char*, int32_t) { return ESP_OK; }
