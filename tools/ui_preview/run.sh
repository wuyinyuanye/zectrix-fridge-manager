#!/bin/sh
# Render all e-paper pages/popups on the host and build a contact sheet.
# Usage: tools/ui_preview/run.sh [out_dir]   (default: build/ui_preview)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$ROOT/build/ui_preview}"
CJSON_DIR="${IDF_PATH:-}/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
  echo "cJSON not found: set IDF_PATH to your ESP-IDF checkout" >&2; exit 1
fi
mkdir -p "$OUT"
BIN="$OUT/ui_preview"
cc -c -O1 -I"$ROOT/main" "$ROOT/main/qrcodegen.c" -o "$OUT/qrcodegen.o"
cc -c -O1 "$CJSON_DIR/cJSON.c" -o "$OUT/cJSON.o"
cc -c -DFONT17="\"$ROOT/main/assets/official_font_17.bin\"" -DFONT29="\"$ROOT/main/assets/official_font_29.bin\"" \
   "$HERE/fonts.S" -o "$OUT/fonts.o"
c++ -std=c++17 -O1 -Wall -Wno-unused-function -I"$HERE/stubs" -I"$ROOT/main" -I"$CJSON_DIR" \
   "$HERE/scenes.cc" "$HERE/epd_emulator.cc" "$ROOT/main/fridge_ui.cc" "$ROOT/main/pixel_canvas.cc" \
   "$ROOT/main/fridge_store.cc" "$OUT/qrcodegen.o" "$OUT/cJSON.o" "$OUT/fonts.o" -o "$BIN"
rm -f "$OUT"/*.pgm "$OUT"/*.png
"$BIN" "$OUT"
python3 "$HERE/to_png.py" "$OUT"
