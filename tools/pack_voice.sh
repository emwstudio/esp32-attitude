#!/usr/bin/env bash
# 把 voice-pipeline/assets 的 WAV 打包到 SD 卡 / SPIFFS 镜像。
# 用法:
#   tools/pack_voice.sh --sd /Volumes/SDCARD      # 拷到 SD 卡 voice/ 目录
#   tools/pack_voice.sh --spiffs                  # 拷进 firmware/spiffs_image/voice（随固件烧录）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/voice-pipeline/assets"

[ -d "$SRC" ] || { echo "先运行 voice-pipeline 生成 assets/*.wav"; exit 1; }

case "${1:-}" in
  --sd)
    DST="${2:?需要 SD 卡挂载点，如 /Volumes/SDCARD}"
    mkdir -p "$DST/voice"
    cp -v "$SRC"/*.wav "$DST/voice/"
    ;;
  --spiffs)
    DST="$ROOT/firmware/spiffs_image/voice"
    mkdir -p "$DST"
    cp -v "$SRC"/*.wav "$DST/"
    echo "已拷入 $DST ，重新 idf.py flash 会一并烧录 SPIFFS"
    ;;
  *)
    echo "用法: $0 --sd <SD挂载点> | --spiffs"; exit 1;;
esac
