// Host-side e-paper panel emulator: keeps what the real panel would show and dumps it as PGM.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "zectrix_epd.h"

namespace {
constexpr int kW = ZECTRIX_EPD_PANEL_WIDTH;
constexpr int kH = ZECTRIX_EPD_PANEL_HEIGHT;
std::vector<uint8_t> g_panel(kW * kH, 255);  // 8-bit gray, what the glass shows

void Blit1bpp(int x0, int y0, int w, int h, const uint8_t* src) {
    const int stride = w / 8;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int px = x0 + x, py = y0 + y;
            if (px < 0 || px >= kW || py < 0 || py >= kH) continue;
            const uint8_t byte = src[y * stride + (x >> 3)];
            g_panel[py * kW + px] = (byte & (0x80u >> (x & 7))) ? 255 : 0;
        }
    }
}
}  // namespace

extern "C" {
int64_t esp_timer_get_time(void) {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
esp_err_t zectrix_epd_power_on(zectrix_epd_handle_t) { return ESP_OK; }
esp_err_t zectrix_epd_power_off(zectrix_epd_handle_t) { return ESP_OK; }
esp_err_t zectrix_epd_refresh_full_1bpp(zectrix_epd_handle_t, const uint8_t* fb, size_t n) {
    if (n < ZECTRIX_EPD_1BPP_FRAME_BYTES) return ESP_FAIL;
    Blit1bpp(0, 0, kW, kH, fb);
    return ESP_OK;
}
esp_err_t zectrix_epd_refresh_partial_1bpp(zectrix_epd_handle_t, const zectrix_epd_rect_t* r,
                                           const uint8_t* fb, size_t n) {
    if (!r || (r->width % 8) != 0 || n < static_cast<size_t>(r->width / 8 * r->height)) return ESP_FAIL;
    Blit1bpp(r->x, r->y, r->width, r->height, fb);
    return ESP_OK;
}
esp_err_t zectrix_epd_refresh_full_4bpp(zectrix_epd_handle_t, const uint8_t* fb, size_t n) {
    if (n < ZECTRIX_EPD_4BPP_FRAME_BYTES) return ESP_FAIL;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const uint8_t b = fb[y * (kW / 2) + (x >> 1)];
            const uint8_t g = (x & 1) ? (b & 0x0F) : (b >> 4);
            g_panel[y * kW + x] = static_cast<uint8_t>(g * 17);
        }
    }
    return ESP_OK;
}
}  // extern "C"

bool DumpPanelPgm(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P5\n%d %d\n255\n", kW, kH);
    std::fwrite(g_panel.data(), 1, g_panel.size(), f);
    std::fclose(f);
    return true;
}
