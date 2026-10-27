#!/usr/bin/env bash
# Graba Pana en la ESP32-P4 4C con el esptool de ESP-IDF: el de PlatformIO se cuelga con el P4 rev 3.
# Uso: tools/flash_p4.sh [puerto]       (compila antes: cd firmware && pio run -e p4-4c)
# Necesita ESP-IDF 5.5+ instalado: o IDF_PATH apunta a él, o está en ~/.espressif/esp-idf-v*/
set -euo pipefail
cd "$(dirname "$0")/../firmware"
BIN=.pio/build/p4-4c/firmware.factory.bin
[ -f "$BIN" ] || { echo "falta $BIN: compila con 'pio run -e p4-4c'"; exit 1; }
PORT=${1:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}
[ -n "$PORT" ] || { echo "no encuentro la placa: pásale el puerto"; exit 1; }
IDF=${IDF_PATH:-$(ls -d ~/.espressif/esp-idf-v* 2>/dev/null | sort -V | tail -1)}
[ -f "$IDF/export.sh" ] || { echo "no encuentro ESP-IDF (define IDF_PATH)"; exit 1; }
# shellcheck disable=SC1091
source "$IDF/export.sh" >/dev/null 2>&1
python -m esptool --chip esp32p4 -p "$PORT" -b 2000000 write-flash 0x0 "$BIN"
