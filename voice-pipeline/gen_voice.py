#!/usr/bin/env python3
"""用面壁平台 VoxCPM2 API 克隆波音 GPWS 音色，批量合成告警语音 -> assets/<name>.wav

无需 GPU。用法:
  export MODELBEST_API_KEY=xxx
  python3 gen_voice.py                 # 默认用 ref_boeing.wav 克隆
  python3 gen_voice.py --ref other.wav

输入 voice_manifest.json: { "pull_up": "拉升！拉升！立即拉升！", ... }
输出 assets/*.wav (16kHz mono 16bit)，并把 whoop.wav 前置于 pull_up.wav。
"""
import argparse
import base64
import json
import os
import sys
import urllib.request

import numpy as np

API = "https://api.modelbest.cn/v1/audio/speech"
HERE = os.path.dirname(os.path.abspath(__file__))

# 强制直连：urllib 在 macOS 会读系统代理，代理一开请求就被掐
_NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def synth(text: str, ref_b64: str, api_key: str, retries: int = 3) -> bytes:
    """返回 48kHz 16bit mono PCM（去掉流式占位 WAV 头）"""
    body = json.dumps({
        "model": "VoxCPM2",
        "input": text,
        "ref_audio": f"data:audio/wav;base64,{ref_b64}",
        "response_format": "wav",
    }).encode()
    last_err = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(API, data=body, headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {api_key}",
            })
            audio = bytearray()
            with _NO_PROXY.open(req, timeout=180) as r:
                for line in r:
                    line = line.decode("utf-8", "ignore").strip()
                    if not line.startswith("data:"):
                        continue
                    payload = line[5:].strip()
                    if payload == "[DONE]":
                        break
                    try:
                        evt = json.loads(payload)
                    except json.JSONDecodeError:
                        continue
                    if evt.get("type") == "speech.audio.delta":
                        audio += base64.b64decode(evt.get("audio", ""))
            data = bytes(audio)
            if data[:4] == b"RIFF":
                # 流式 WAV 头（sizes 为 0xFFFFFFFF），PCM 从 'data' chunk 后开始
                idx = data.find(b"data", 12)
                data = data[idx + 8:] if idx > 0 else data[44:]
            if data:
                return data
        except Exception as e:  # 网络抖动，重试
            last_err = e
            print(f"  重试 {attempt + 1}/{retries}: {e}", flush=True)
    if last_err:
        raise last_err
    return b""


def save_wav_16k(pcm48: bytes, path: str):
    """48kHz int16 mono -> 16kHz int16 mono WAV"""
    x = np.frombuffer(pcm48, dtype=np.int16).astype(np.float32)
    n_out = int(len(x) * 16000 / 48000)
    xi = np.linspace(0, len(x) - 1, n_out)
    y = np.interp(xi, np.arange(len(x), dtype=np.float32), x)
    peak = np.max(np.abs(y)) or 1.0
    y = y / max(peak / 30000.0, 1.0)  # 仅在过载时衰减
    import wave
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(y.astype(np.int16).tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default=os.path.join(HERE, "ref_boeing.wav"))
    ap.add_argument("--manifest", default=os.path.join(HERE, "voice_manifest.json"))
    ap.add_argument("--out", default=os.path.join(HERE, "assets"))
    args = ap.parse_args()

    api_key = os.environ.get("MODELBEST_API_KEY")
    if not api_key:
        sys.exit("请先 export MODELBEST_API_KEY=...")

    with open(args.ref, "rb") as f:
        ref_b64 = base64.b64encode(f.read()).decode()
    with open(args.manifest, encoding="utf-8") as f:
        manifest = json.load(f)

    os.makedirs(args.out, exist_ok=True)
    for name, text in manifest.items():
        print(f"[{name}] {text}", flush=True)
        pcm = synth(text, ref_b64, api_key)
        if not pcm:
            print("  !! 空响应，跳过")
            continue
        path = os.path.join(args.out, f"{name}.wav")
        save_wav_16k(pcm, path)
        print(f"  -> {path} ({len(pcm)//2/48000:.1f}s)")

    # pull_up 前置 whoop 警报音（如存在）
    whoop = os.path.join(args.out, "whoop.wav")
    pull = os.path.join(args.out, "pull_up.wav")
    if os.path.exists(whoop) and os.path.exists(pull):
        import wave

        def read_pcm(p):
            with wave.open(p, "rb") as w:
                return w.readframes(w.getnframes())
        merged = read_pcm(whoop) + read_pcm(pull)
        with wave.open(pull, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
            w.writeframes(merged)
        print("pull_up.wav <- whoop + voice 合并完成")

    print("done")


if __name__ == "__main__":
    main()
