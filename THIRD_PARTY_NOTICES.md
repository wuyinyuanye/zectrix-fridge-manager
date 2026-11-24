# Third-party notices

This project is distributed under the MIT License. The following third-party
software or assets retain their own licenses.

## TRMNL16 Regular

The embedded ASCII bitmap table in
`components/zectrix_demo_ui/font/zectrix_ascii_font_8x16.h` was rasterized from
TRMNL16 Regular by Heavyweight Digital Type Foundry. The font is licensed under
the SIL Open Font License, Version 1.1, included at
`licenses/TRMNL_FONT_LICENSE.txt`.

## Espressif ESP-IDF

ESP-IDF is provided by Espressif Systems under its respective open-source
licenses. It is a build dependency and is not copied into this project.

## Espressif esp_codec_dev

The audio codec abstraction is obtained through ESP-IDF Component Manager as
`espressif/esp_codec_dev`. Its license file is delivered with the downloaded
component in `managed_components/espressif__esp_codec_dev/LICENSE`.

## Demonstration artwork

The lighthouse, snowy-path and mountain images in `main/assets` were created
for the Zectrix hardware demonstration and are distributed with this project
under the project MIT License by Zectrix Lab.

## QR Code generator library (qrcodegen)

`main/qrcodegen.c` and `main/qrcodegen.h` are the C QR Code generator library by
Project Nayuki, distributed under the MIT License (see the header of each file).
https://www.nayuki.io/page/qr-code-generator-library

## Opus codec and Opus encoder wrapper

Voice audio is encoded with `78/esp-opus` (Xiph.Org Opus, BSD-3-Clause) and
`78/esp-opus-encoder`, both obtained through ESP-IDF Component Manager. Their
license files are delivered with the downloaded components in `managed_components/`.

## Espressif esp_websocket_client

Obtained through ESP-IDF Component Manager as `espressif/esp_websocket_client`
(Apache-2.0). Its license is delivered in `managed_components/`.

## UI fonts (not distributed)

The 17px / 29px Chinese bitmap fonts used by the fridge UI
(`main/assets/official_font_17.bin`, `main/assets/official_font_29.bin`) are
extracted locally from the official ZECTRIX NOTE4 3.7.5 firmware with
`tools/extract_official_375_fonts.py`. They are **not** part of this repository;
each user generates them from firmware they already own.
