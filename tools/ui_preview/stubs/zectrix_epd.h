#pragma once
// Host-preview stand-in for components/zectrix_epd/include/zectrix_epd.h (same API surface used by UI)
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ZECTRIX_EPD_PANEL_WIDTH 400
#define ZECTRIX_EPD_PANEL_HEIGHT 300
#define ZECTRIX_EPD_1BPP_FRAME_BYTES 15000
#define ZECTRIX_EPD_4BPP_FRAME_BYTES 60000
typedef struct zectrix_epd_t* zectrix_epd_handle_t;
typedef struct { int x; int y; int width; int height; } zectrix_epd_rect_t;
esp_err_t zectrix_epd_power_on(zectrix_epd_handle_t handle);
esp_err_t zectrix_epd_power_off(zectrix_epd_handle_t handle);
esp_err_t zectrix_epd_refresh_full_1bpp(zectrix_epd_handle_t h, const uint8_t* fb, size_t n);
esp_err_t zectrix_epd_refresh_partial_1bpp(zectrix_epd_handle_t h, const zectrix_epd_rect_t* r,
                                           const uint8_t* fb, size_t n);
esp_err_t zectrix_epd_refresh_full_4bpp(zectrix_epd_handle_t h, const uint8_t* fb, size_t n);
#ifdef __cplusplus
}
#endif
