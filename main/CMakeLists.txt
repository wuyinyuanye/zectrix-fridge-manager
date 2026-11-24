# The two UI fonts are extracted locally from the official ZECTRIX 3.7.5 firmware and are
# not distributed with the source. Fail early with instructions instead of a cryptic error.
foreach(font official_font_17.bin official_font_29.bin)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/assets/${font}")
        message(FATAL_ERROR
            "main/assets/${font} is missing.\n"
            "Generate it from the official firmware image:\n"
            "  python3 tools/extract_official_375_fonts.py --firmware <official_3.7.5.bin> --out-dir main/assets\n"
            "See README_FRIDGE.md -> Fonts.")
    endif()
endforeach()

idf_component_register(
    SRCS
        "app_main.cc"
        "pixel_canvas.cc"
        "fridge_store.cc"
        "audio_service.cc"
        "xiaozhi_client.cc"
        "wifi_web_service.cc"
        "fridge_ui.cc"
        "qrcodegen.c"
    INCLUDE_DIRS "."
    REQUIRES
        zectrix_board
        zectrix_epd
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_http_client
        esp_websocket_client
        78__esp-opus-encoder
        78__esp-opus
        esp-tls
        mbedtls
        nvs_flash
        spiffs
        json
        heap
        spi_flash
    EMBED_FILES
        "assets/official_font_17.bin"
        "assets/official_font_29.bin"
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
