#!/usr/bin/env python3
"""whoop 警报、提示音等程序合成音。产物：assets/<name>.wav (16kHz mono 16bit)"""
import numpy as np
import wave
import os

SR = 16000
OUT = os.path.join(os.path.dirname(__file__), "assets")
os.makedirs(OUT, exist_ok=True)


def save(name, samples):
    samples = np.clip(samples, -1, 1)
    pcm = (samples * 32767).astype(np.int16)
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"assets/{name}.wav")


def whoop(duration=2.0, f_lo=650, f_hi=950, period=0.55):
    """GPWS 'whoop'：锯齿扫频警报"""
    t = np.arange(int(SR * duration)) / SR
    phase = (t % period) / period
    freq = f_lo + (f_hi - f_lo) * phase
    sig = np.sin(2 * np.pi * np.cumsum(freq) / SR)
    env = np.minimum(1, t * 20) * np.minimum(1, (duration - t) * 20)
    return sig * env * 0.8


def chime(freqs=(880, 1174), tone=0.14, gap=0.05):
    """双音提示 ding-dong"""
    parts = []
    for f in freqs:
        t = np.arange(int(SR * tone)) / SR
        env = np.exp(-t * 12)
        parts.append(np.sin(2 * np.pi * f * t) * env)
        parts.append(np.zeros(int(SR * gap)))
    return np.concatenate(parts) * 0.8


def cavalry(freq=760, period=0.6, duration=2.4):
    """失速告警式抖动音（stick-shaker 风格）"""
    t = np.arange(int(SR * duration)) / SR
    mod = (np.sin(2 * np.pi * t / period) > 0).astype(float)
    return np.sin(2 * np.pi * freq * t) * mod * 0.7


if __name__ == "__main__":
    save("whoop", whoop())
    save("chime", chime())
    save("stall_warn", cavalry())
