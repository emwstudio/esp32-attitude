#!/usr/bin/env bash
# 用 macOS 自带 say 生成占位语音（没有 GPU / API Key 时先把固件跑起来）。
# 产物：assets/<name>.wav（16kHz 单声道 16bit）
# 之后用 gen_texts.py + gen_voice.py（VoxCPM2 克隆音色）替换为正式版。
set -eu
cd "$(dirname "$0")"
mkdir -p assets tmp_say

gen() {
  say -v Tingting -o "tmp_say/$1.aiff" "$2"
  afconvert -f WAVE -d LEI16@16000 -c 1 "tmp_say/$1.aiff" "assets/$1.wav"
  echo "assets/$1.wav"
}

gen system_ready "系统自检完成，姿态仪就绪"
gen pitch_high "注意俯仰角，俯仰角过大"
gen bank_angle "坡度，注意坡度"
gen sink_rate "下降率，下降率过大"
gen stall "失速，失速，注意空速"
gen pull_up "拉升，拉升，立即拉升"
gen terrain "注意地形，注意地形"
gen altitude_low "高度过低，注意高度"

rm -rf tmp_say
echo "done -> assets/"
