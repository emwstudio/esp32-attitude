#!/usr/bin/env python3
"""杭州塔台员语音对话服务。

链路：ESP32 按住 KEY 录音 -> WiFi TCP AUD0 帧 -> STT(Qwen3-ASR-1.7B MLX 本地)
-> LLM(MiniCPM5-2B API，塔台员人设) -> TTS(VoxCPM2 API，克隆 ref_tower.wav)
-> AUD1 帧回发 ESP32 播放。

用法：.venv/bin/python server.py
API 全部强制直连（urllib 在 macOS 会读系统代理，代理开着会被掐）。
"""

import base64
import itertools
import json
import os
import socket
import sys
import time
import urllib.request

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
LISTEN_PORT = 7890
LLM_API = "https://api.modelbest.cn/v1/chat/completions"
TTS_API = "https://api.modelbest.cn/v1/audio/speech"
ASR_REPO = "appautomaton/qwen3-asr-1.7b-int8-mlx"
REF_AUDIO = os.path.join(HERE, "ref_tower.wav")
API_KEY = os.environ.get("MODELBEST_API_KEY", "")

SYSTEM_PROMPT = (
    "你是杭州萧山国际机场塔台管制员，女性，严格使用标准陆空通话用语，禁止自由发挥。"
    "飞行员呼号是电磁波001号（语音识别可能听成「电磁波幺幺栋」「电磁波001」等，都是同一架），"
    "你回话写「电磁波001」即可（系统会自动转成航空读法）。\n"
    "通话规则：\n"
    "1. 长指令开头叫一次呼号「电磁波001」即可；简短确认直接说「正确」「收到」「再见」，不带呼号。\n"
    "2. 绝对不要重复或复诵对方说过的话，直接给指令或答复；对方请求降落就给落地许可，请求引导就给高度航向。"
    "开场禁止套话（如「塔台呼叫确认」「落地许可」），直接呼号+指令内容开头，且每轮开场方式不许重复。\n"
    "3. 数字一律用阿拉伯数字书写（如 高度 300、速度 280、航向 60、跑道 06、抬轮速度 250、"
    "修压 1012、频率 125.75），禁止写中文数字或洞/幺/两/拐（系统会自动转成航空读法）。\n"
    "4. 只准使用标准话术词汇：收到、正确、错误、证实、重复、允许、保持、上到、下到、"
    "改平、注意、联系、再见、复诵、稍等、雷达识别、活动通报、可以落地、可以起飞、进跑道等待、复飞。"
    "禁止自创词组（如「核实高度变化率」「塔台呼叫确认」「落地许可」「正常」）。\n"
    "5. 禁止寒暄（包括「你好」「您好」「谢谢」）、解释、英文、表情和任何非标准用语；"
    "对方打招呼或闲聊，用「收到」或指令挡回去。每次一到两句，不超过 30 字。\n"
    "6. 高度用米，速度用公里每小时，风向风速格式「地面风 310 度 2 米秒」。\n"
    "7. 每条飞行员消息末尾附【仪表实时数据】——这是你的雷达屏幕和判断依据，回应必须与数据一致：\n"
    "   - 先根据数据判断飞机处于什么阶段（地面/滑跑/爬升/巡航/进近/五边），再决定怎么回应；\n"
    "   - 请求与数据矛盾时，必须指出矛盾而不是照背模板（例如在 1700 米空中请求起飞→提醒他已经在空中；"
    "在 1400 米请求降落→高度太高，先下高度）；\n"
    "   - 数据异动要管：垂直速度绝对值超过 10 米每秒（猛爬升/猛下降）→ 先让改平；"
    "横滚绝对值超过 30 度 → 提醒「注意坡度，改平」；\n"
    "   - 你的雷达只有高度、速度、航向：只有这三项能报数值，且只挑与当前指令相关的一两项说；"
    "俯仰/横滚/垂直速度绝不报数值，绝不说「正常」。\n"
    "8. 灵活机动，体现管制员的临场处置：\n"
    "   - 数据异常主动管：快速掉高度→「注意下降率」；大坡度→「改平」；速度掉太快→提醒失速；\n"
    "   - 按阶段给指令：五边/低空（<400米 且在下降）→ 落地许可；巡航 → 航路通报或保持；"
    "地面/低速 → 推出开车或起飞指令；\n"
    "   - 飞行员连续多轮只说「塔台」时，每轮必须给不同内容：数据通报、活动通报、流程推进、"
    "流量控制、天气提醒轮着来，禁止重复使用「雷达识别」开场；「杭州塔台」只在首次联系时报，之后省略；\n"
    "   - 可以合理虚构管制环境让通话更真实：活动通报（方位/距离/高度）、流量控制稍等、"
    "换跑道、移交频率、天气提醒，但要专业克制，一轮最多一个花样；\n"
    "   - 同样的话不许连续两轮用同样的句式，措辞在标准话术框架内自然变化。\n"
    "9. 消息末尾的（本轮处置方向：…）是带班主任的提示，按它执行，但措辞自己组织，不要念出来。\n"
    "示例：\n"
    "飞行员：塔台，电磁波001号，高度 900，请求引导。【仪表实时数据：高度900米，速度480公里每小时，航向90度，俯仰2度，横滚0度，垂直速度1米每秒】\n"
    "管制员：电磁波001，杭州塔台，雷达识别，保持高度 900，航向 090。\n"
    "飞行员：收到，保持 900，航向 090。\n"
    "管制员：正确。\n"
    "飞行员：请求降落。【仪表实时数据：高度580米，速度420公里每小时，航向90度，俯仰-2度，横滚1度，垂直速度-4.5米每秒】\n"
    "管制员：电磁波001，高度 580，速度 420，可以落地，跑道 06，地面风 310 度 2 米秒。\n"
    "飞行员：请求降落。【仪表实时数据：高度1405米，速度106公里每小时，航向51度，俯仰12度，横滚9度，垂直速度14米每秒】\n"
    "管制员：电磁波001，高度 1405 太高，先改平，下到 600 再建立五边，航向 051 保持。\n"
    "飞行员：请求起飞。【仪表实时数据：高度1693米，速度109公里每小时，航向50度，俯仰11度，横滚-1度，垂直速度13.6米每秒】\n"
    "管制员：电磁波001，你已经在 1693 米空中，不是地面，核实情况再报。\n"
    "飞行员：请求起飞。【仪表实时数据：高度5米，速度0公里每小时，航向60度，俯仰0度，横滚0度，垂直速度0米每秒】\n"
    "管制员：电磁波001，可以起飞，跑道 06，抬轮速度 250，修压 1012，离地联系进近 125.75，再见。\n"
    "飞行员：塔台，这里是电磁波001号。【仪表实时数据：高度1289米，速度92公里每小时，航向51度，俯仰21度，横滚6度，垂直速度25.1米每秒】\n"
    "管制员：电磁波001，你在快速爬升，先改平，再报高度。\n"
    "飞行员：你好。【仪表实时数据：高度1200米，速度510公里每小时，航向270度，俯仰1度，横滚-2度，垂直速度0.5米每秒】\n"
    "管制员：收到，报你的意图。\n"
    "飞行员：塔台。【仪表实时数据：高度3000米，速度600公里每小时，航向180度，俯仰0度，横滚0度，垂直速度0米每秒】\n"
    "管制员：收到，保持 3000，航向 180，你 11 点钟方向有相对活动，注意观察。\n"
    "飞行员：塔台。【仪表实时数据：高度2980米，速度605公里每小时，航向182度，俯仰0度，横滚1度，垂直速度0米每秒】\n"
    "管制员：电磁波001，巡航正常，预计 10 分钟后下高度，保持监听。\n"
    "飞行员：报下数据。【仪表实时数据：高度300米，速度280公里每小时，航向60度，俯仰-5度，横滚2度，垂直速度-12米每秒】\n"
    "管制员：电磁波001，高度 300，速度 280，航向 060，下降率大，注意下降率。\n"
    "飞行员：塔台，五边了。【仪表实时数据：高度250米，速度260公里每小时，航向60度，俯仰-4度，横滚3度，垂直速度-5.5米每秒】\n"
    "管制员：电磁波001，五边收到，对正跑道 06，可以落地，注意侧风。\n"
    "飞行员：附近有别的飞机吗？\n"
    "管制员：电磁波001，活动通报，2 点钟方向，5 公里，同向，高度 1200，注意避让。"
)

_NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))

# ---- 电台信道模拟（对讲机味，对齐 esp32-radar 做法）---------------------
# 窄带(300~3400Hz) + 软限幅 → （可选降速）→ 全频白噪底噪 + 结尾静噪尾音
# 注意顺序：噪声必须在降速插值之后加，否则高频嘶声被平滑掉
RADIO_DRIVE = 1.4       # 压缩驱动（再大就糊）
RADIO_CLICK = True      # 结尾静噪尾音 shhk
RADIO_TEMPO = 1.0       # 1.0 = 不降速（最清晰），调小则语速放缓音调下沉
RADIO_HISS = 110        # 底噪 ±幅度/32768

REF_TEXT = (  # ref_tower.wav (0.15~10.15s，前10秒干净段；呼号与后段同一架=幺六洞八)
    "幺六洞八地面，幺六洞八什么时候能推呀？动作稍快点，时间是有限制的。"
    "幺六洞八地面，幺六洞八已经完全准备好了。"
)


def _biquad_hp(fc, fs, q=0.707):
    w = 2 * np.pi * fc / fs
    a = np.exp(-w / (2 * q))
    b0 = (1 + a) / 2
    return np.array([b0, -2 * b0, b0]), np.array([1, -2 * a, a * a])


def _biquad_lp(fc, fs, q=0.707):
    w = 2 * np.pi * fc / fs
    a = np.exp(-w / (2 * q))
    b0 = (1 - a) / 2
    return np.array([b0, 2 * b0, b0]), np.array([1, -2 * a, a * a])


class RadioFilter:
    """对流式 int16 PCM 块做电台信道模拟，滤波状态跨块保持。"""

    def __init__(self, fs=16000):
        self.fs = fs
        self.started = False
        self.drive_norm = np.tanh(RADIO_DRIVE)
        # 以 1kHz 正弦实测带内增益并归一，避免压缩级把带外信号重新抬平
        t = np.arange(fs // 4, dtype=np.float32) / fs
        probe = 0.5 * np.sin(2 * np.pi * 1000 * t)
        self.stages = []
        for b, a in (_biquad_hp(300, fs), _biquad_hp(300, fs),
                     _biquad_lp(3400, fs), _biquad_lp(3400, fs)):
            self.stages.append([b, a, np.zeros(2)])  # b, a, zi
        g = np.sqrt((self._run(probe)[fs // 8:] ** 2).mean()) / (0.5 / np.sqrt(2))
        self.gain = max(g, 1e-6)
        for st in self.stages:  # 探针用完，清零滤波状态
            st[2] = np.zeros(2)

    def _run(self, x):
        for st in self.stages:
            b, a, zi = st
            y = np.empty_like(x)
            for i in range(len(x)):
                v = b[0] * x[i] + zi[0]
                zi[0] = b[1] * x[i] - a[1] * v + zi[1]
                zi[1] = b[2] * x[i] - a[2] * v
                y[i] = v
            x = y
        return x

    def _click(self, n=48):
        t = np.arange(n)
        return (np.random.randn(n) * np.exp(-t / 12.0) * 0.35).astype(np.float32)

    def _squelch_tail(self, n=960):
        """静噪尾音：60ms 带限噪声快速衰减，真电台标志性的 'shhk' 收尾（非数码 tick）"""
        t = np.arange(n, dtype=np.float32)
        x = np.random.randn(n).astype(np.float32) * np.exp(-t / 200.0) * 0.13
        # 单极点低通 ~2kHz，去掉数码感的超高频
        a = np.exp(-2 * np.pi * 2000 / 16000)
        y = np.empty_like(x)
        acc = np.float32(0)
        for i in range(n):
            acc = (1 - a) * x[i] + a * acc
            y[i] = acc
        return y

    def process(self, pcm16: bytes) -> bytes:
        # 只做窄带 + 软限幅；噪声和咔哒必须放在降速之后加，
        # 否则线性插值会把白噪高频部分平滑掉（嘶声变闷，失去电台味）
        x = np.frombuffer(pcm16, dtype=np.int16).astype(np.float32) / 32768.0
        if RADIO_CLICK and not self.started and len(x):
            self.started = True
        x = self._run(x) / self.gain
        x = np.tanh(RADIO_DRIVE * x) / self.drive_norm          # 软限幅
        return (np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes()

    def hiss(self, pcm16: bytes) -> bytes:
        """降速之后调用：全频白噪底噪（幅度 RADIO_HISS）"""
        x = np.frombuffer(pcm16, dtype=np.int16).astype(np.float32)
        noise = np.random.randint(-RADIO_HISS, RADIO_HISS + 1, len(x)).astype(np.float32)
        x = np.clip(x + noise, -32768, 32767).astype(np.int16)
        return x.tobytes()

    def tail(self) -> bytes:
        if not RADIO_CLICK or not self.started:
            return b""
        self.started = False
        return (self._squelch_tail() * 32768).astype(np.int16).tobytes()


class TempoStretch:
    """0.9x 降速播放（语速放缓 + 音调下沉 ~10%），跨块保持插值相位。"""

    def __init__(self, ratio=RADIO_TEMPO):
        self.ratio = ratio      # 读指针步进；<1 = 输出变长 = 放慢
        self.buf = np.array([], dtype=np.float32)
        self.phase = 0.0

    def process(self, pcm16: bytes) -> bytes:
        x = np.frombuffer(pcm16, dtype=np.int16).astype(np.float32)
        self.buf = np.concatenate([self.buf, x])
        n = len(self.buf)
        if n < 2:
            return b""
        # 输出采样点：phase, phase+ratio, ... 落在 buf 内才产出
        count = int((n - 1 - self.phase) / self.ratio)
        if count <= 0:
            return b""
        pos = self.phase + self.ratio * np.arange(count)
        i0 = pos.astype(np.int64)
        frac = (pos - i0).astype(np.float32)
        y = self.buf[i0] * (1 - frac) + self.buf[i0 + 1] * frac
        used = int(pos[-1])          # 已消费的整数样本数
        self.phase = pos[-1] + self.ratio - used
        self.buf = self.buf[used:]
        return y.astype(np.int16).tobytes()


# 导演提示：弱模型多轮会自我复读，按情境给处置方向（措辞和判断仍归 LLM）
_CHIT_CUES = [
    "数据通报（报高度速度航向）",
    "活动通报（虚构一个方位/距离/高度的交通活动）",
    "流程推进（按当前数据判断阶段，给下一步指令）",
    "流量控制（排序、稍等、预计等待时间）",
    "天气/风提醒（地面风或颠簸提醒）",
]
_chit_cycle = itertools.cycle(_CHIT_CUES)


def direct_cue(said: str, flight: dict) -> str:
    """只给异常提醒方向和闲聊轮换，局面判断完整留给 LLM。"""
    try:
        vs = float(flight["vs"]); roll = float(flight["roll"])
    except (ValueError, KeyError):
        vs = roll = 0.0
    if vs < -8:
        return "数据异动：垂直速度过大，先让改平，再谈其它"
    if vs > 10:
        return "数据异动：在快速爬升，先让改平，再谈其它"
    if abs(roll) > 30:
        return "数据异动：坡度过大，提醒「注意坡度，改平」"
    if said in ("塔台。", "塔台", "你好。", "你好", "在吗。", "在吗"):
        return next(_chit_cycle)   # 只叫塔台：轮换花样防复读
    return "自由处置（先看数据判断飞机处于什么阶段，再回应；请求与数据矛盾时指出矛盾，不要照背模板）"


def llm_chat(messages):
    body = json.dumps({
        "model": "MiniCPM5-2B",
        "messages": messages,
        "stream": False,
        "temperature": 0.7,      # 保留临场发挥的灵活性（话术框架内变化）
        "max_tokens": 600,       # 推理链也吃预算，给够防空回复
    }).encode()
    req = urllib.request.Request(LLM_API, data=body, headers={
        "Content-Type": "application/json",
        "Authorization": f"Bearer {API_KEY}",
    })
    with _NO_PROXY.open(req, timeout=60) as r:
        resp = json.loads(r.read())
    return resp["choices"][0]["message"]["content"].strip()


# 阿拉伯数字 → 航空读法（逐位确定性转换，不信 2B 小模型自觉——radar 仓同款思路）
_DIG_SPOKEN = {"0": "洞", "1": "幺", "2": "两", "3": "三", "4": "四",
               "5": "五", "6": "六", "7": "拐", "8": "八", "9": "九", ".": "点"}


def to_spoken(text: str) -> str:
    return "".join(_DIG_SPOKEN.get(ch, ch) for ch in text)


TRANSCRIPT = os.path.join(HERE, "transcript.txt")


def log_transcript(said: str, spoken: str, flight: dict):
    """纯净对话记录（demo 录制用）：时间 + 飞行员台词 + 塔台台词 + 当刻飞行数据"""
    ts = time.strftime("%H:%M:%S")
    with open(TRANSCRIPT, "a", encoding="utf-8") as f:
        f.write(f"[{ts}] 飞行员: {said}\n")
        f.write(f"[{ts}] 塔  台: {spoken}\n")
        f.write(f"         (高度{flight['alt']}m 速度{flight['spd']}km/h 航向{flight['hdg']}° "
                f"俯仰{flight['pitch']}° 横滚{flight['roll']}° 垂速{flight['vs']}m/s)\n\n")


def tts_stream(text, ref_b64, on_chunk):
    """VoxCPM2 克隆合成，SSE 边收边把 16kHz PCM 块塞给 on_chunk（真流式）"""
    body = json.dumps({
        "model": "VoxCPM2",
        "input": text,
        "ref_audio": f"data:audio/wav;base64,{ref_b64}",
        "ref_text": REF_TEXT,   # 参考音逐字稿：显著提高克隆相似度（radar 仓同款做法）
        "response_format": "wav",
    }).encode()
    req = urllib.request.Request(TTS_API, data=body, headers={
        "Content-Type": "application/json",
        "Authorization": f"Bearer {API_KEY}",
    })
    first = True
    carry = np.array([], dtype=np.float32)  # 重采样相位余量
    radio = RadioFilter(16000)              # 电台信道模拟
    stretch = TempoStretch()                # 0.9x 降速（语速+音调，电台质感）

    def push(pcm48: bytes):
        nonlocal first, carry
        if not pcm48:
            return
        if first:  # 去掉流式 WAV 占位头
            idx = pcm48.find(b"data", 0, 64)
            pcm48 = pcm48[idx + 8:] if idx > 0 else pcm48[44:]
            first = False
        x = np.concatenate([carry, np.frombuffer(pcm48, dtype=np.int16).astype(np.float32)])
        n_out = len(x) // 3  # 48k→16k 整除 3
        if n_out == 0:
            carry = x
            return
        y = x[: n_out * 3].reshape(-1, 3).mean(axis=1)  # 三点均值抗混叠
        carry = x[n_out * 3:]
        out = stretch.process(radio.process(y.astype(np.int16).tobytes()))
        if out:
            on_chunk(radio.hiss(out))

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
                push(base64.b64decode(evt.get("audio") or evt.get("delta") or b""))
            elif evt.get("type") == "speech.audio.done":
                break
    tail = radio.tail()  # 收尾 squelch 咔哒
    if tail:
        on_chunk(tail)


def send_frame(sock, magic: bytes, payload: bytes):
    sock.sendall(magic + len(payload).to_bytes(4, "little") + payload)


def run_client_loop(sock, asr, ref_b64, history):
    """TCP 主循环：AUD0 帧 -> STT -> LLM -> TTS -> AUD1 回发；LOG0 帧 -> 设备日志打印；
    STA0 帧 -> 设备实时飞行数据（alt/spd/hdg/pitch/roll/vs），注入 LLM 上下文"""
    state = "scan"
    want = 0
    buf = bytearray()
    slide = bytearray()
    is_log = False
    flight = {"alt": "-", "spd": "-", "hdg": "-", "pitch": "-", "roll": "-", "vs": "-"}
    run_client_loop.last_said = ""   # 上一轮用户说的话（连续重复检测用）

    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue   # 空闲等待，继续收
        if not chunk:
            raise ConnectionError("client closed")
        for c in chunk:
            if state == "scan":
                # 滑动窗口匹配 AUD0（音频帧）/ LOG0（设备日志）/ STA0（飞行数据）
                slide.append(c)
                if len(slide) > 4:
                    slide.pop(0)
                if bytes(slide) in (b"AUD0", b"LOG0", b"STA0"):
                    is_log = bytes(slide) == b"LOG0"
                    state = "hdr"
                    buf = bytearray()
            elif state == "hdr":
                buf.append(c)
                if len(buf) == 4:
                    want = int.from_bytes(buf, "little")
                    buf = bytearray()
                    state = "data" if 0 < want <= 640 * 1024 else "scan"
            else:
                buf.append(c)
                if len(buf) >= want:
                    state = "scan"
                    pcm = bytes(buf)
                    buf = bytearray()
                    if is_log:
                        print(f"  [dev] {pcm.decode('utf-8', 'ignore')}", flush=True)
                        continue
                    if pcm.startswith(b"alt="):   # STA0 飞行数据帧
                        for kv in pcm.decode("utf-8", "ignore").split(","):
                            if "=" in kv:
                                k, v = kv.split("=", 1)
                                if k in flight:
                                    flight[k] = v
                        print(f"  [sta] {pcm.decode('utf-8', 'ignore')}", flush=True)
                        continue
                    t0 = time.time()
                    audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
                    out = asr.generate(audio, sample_rate=16000, language="Chinese")
                    text = out.text.strip()
                    print(f"[STT {time.time()-t0:.1f}s] {text}", flush=True)
                    if not text or len(text) < 2:
                        print("  (空/太短，忽略)", flush=True)
                        continue

                    t0 = time.time()
                    said = text.split("\n")[0]
                    cue = direct_cue(said, flight)
                    # 连续两轮说一样的话时追加换花样指令（防"状态正常，请继续保持"式复读）
                    if said == run_client_loop.last_said:
                        cue += "；对方连续问了同样的问题，换一种说法回答，不要重复上一轮的内容"
                    run_client_loop.last_said = said
                    text += (f"\n【仪表实时数据：高度{flight['alt']}米，速度{flight['spd']}公里每小时，"
                             f"航向{flight['hdg']}度，俯仰{flight['pitch']}度，横滚{flight['roll']}度，"
                             f"垂直速度{flight['vs']}米每秒】"
                             f"\n（本轮处置方向：{cue}；不要照搬上一轮的句式）")
                    history.append({"role": "user", "content": text})
                    reply = llm_chat(history)
                    if not reply:
                        reply = llm_chat(history)   # 推理链偶尔吃光预算出空回复，重试一次
                    # 防泄漏：小模型偶尔把注入的数据块/处置方向复读进回复，确定性剥掉
                    import re
                    reply = re.split(r"【|（本轮|\(本轮|处置方向", reply)[0].strip()
                    if not reply:
                        reply = "重复"              # 仍失败：标准话术"再说一遍"
                    history.append({"role": "assistant", "content": reply})
                    if len(history) > 13:  # system + 最近6轮（防长历史自我复读）
                        history = [history[0]] + history[-12:]
                    spoken = to_spoken(reply)   # 数字 → 航空读法（确定性转换，只影响播报）
                    print(f"[LLM {time.time()-t0:.1f}s] {reply}  ->  {spoken}", flush=True)

                    t0 = time.time()
                    nchunks = [0]

                    def on_chunk(pcm16):
                        nchunks[0] += 1
                        for off in range(0, len(pcm16), 16384):   # 切块防大单帧
                            send_frame(sock, b"AUD1", pcm16[off:off + 16384])

                    tts_stream(spoken, ref_b64, on_chunk)
                    send_frame(sock, b"AUD1", b"")   # 空帧 = 流结束标记
                    print(f"[TTS {time.time()-t0:.1f}s] 流式 {nchunks[0]} 块", flush=True)
                    log_transcript(text.split("\n")[0], spoken, flight)


def main():
    if not API_KEY:
        sys.exit("请先 export MODELBEST_API_KEY=...")

    print("[1/3] 加载 Qwen3-ASR-1.7B (MLX int8)...", flush=True)
    import mlx_speech
    asr = mlx_speech.asr.load(ASR_REPO)
    print("      ASR 就绪", flush=True)

    with open(REF_AUDIO, "rb") as f:
        ref_b64 = base64.b64encode(f.read()).decode()
    print(f"[2/3] 塔台员参考音 {os.path.getsize(REF_AUDIO) // 1024}KB", flush=True)

    history = [{"role": "system", "content": SYSTEM_PROMPT}]

    with open(TRANSCRIPT, "a", encoding="utf-8") as f:
        f.write(f"===== 录制场次 {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n\n")

    # TCP 监听，等设备接入；断开后回到监听
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", LISTEN_PORT))
    srv.listen(1)
    print(f"[3/3] TCP 监听 :{LISTEN_PORT}，等待设备连接...", flush=True)

    while True:
        sock, addr = srv.accept()
        print(f"  设备接入: {addr}", flush=True)
        # 设备硬复位时 TCP 不会挥手：keepalive 快速探死活，避免 recv 卡死旧连接
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        if sys.platform == "darwin":
            sock.setsockopt(socket.IPPROTO_TCP, 0x10, 5)   # macOS 叫 TCP_KEEPALIVE(0x10)
        elif hasattr(socket, "TCP_KEEPIDLE"):
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 5)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 3)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 2)
        sock.settimeout(60)   # 兜底：recv 最长阻塞 60s
        try:
            run_client_loop(sock, asr, ref_b64, history)
        except (ConnectionError, OSError) as e:
            print(f"  设备断开({e})，等待重连...", flush=True)
        finally:
            sock.close()


if __name__ == "__main__":
    main()
