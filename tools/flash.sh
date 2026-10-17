#!/usr/bin/env bash
# 编译并烧录固件。用法: tools/flash.sh [串口，如 /dev/cu.usbmodem101]
set -euo pipefail
cd "$(dirname "$0")/../firmware"
# shellcheck disable=SC1091
source ~/esp/esp-idf/export.sh >/dev/null
PORT="${1:-}"
if [ -n "$PORT" ]; then
  idf.py -p "$PORT" flash monitor
else
  idf.py flash
fi
