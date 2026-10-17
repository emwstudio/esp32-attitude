#!/usr/bin/env python3
"""用面壁智能 MiniCPM5-2B 批量生成驾驶舱告警文案 -> texts.json

用法:
  export MODELBEST_API_KEY=xxx
  # 模型 ID 从 https://platform.modelbest.cn/console/models 复制
  python3 gen_texts.py --model minicpm5-2b

输出 texts.json: { "<场景id>": ["候选文案1", "候选文案2", ...], ... }
人工挑选后填入 voice_manifest.json 交给 gen_voice.py 合成。
"""
import argparse
import json
import os
import sys
import urllib.request

API = "https://api.modelbest.cn/v1/chat/completions"

SCENARIOS = {
    "system_ready": "系统启动完成、自检通过时的播报",
    "pitch_high": "俯仰角过大（超过30度）的警告",
    "bank_angle": "横滚坡度超过45度的警告",
    "sink_rate": "下降率过大的警告",
    "stall": "空速过低即将失速的紧急警告",
    "pull_up": "即将撞地的最紧急拉升警告（对应 GPWS PULL UP）",
    "terrain": "注意地形的警告",
    "altitude_low": "高度过低的警告",
}

PROMPT = (
    "你是飞机驾驶舱语音告警系统的文案设计师。为以下场景写 {n} 条中文告警播报文案：{desc}。\n"
    "要求：每条不超过12个字，语气冷静专业，类似真实民航驾驶舱告警（如“拉升”“注意地形”），"
    "可以中英混合（如“PULL UP，拉升”）。只输出一个 JSON 数组，不要输出其他内容。"
)


def chat(model: str, prompt: str, api_key: str) -> str:
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "stream": False,
        "temperature": 0.7,
    }).encode()
    req = urllib.request.Request(
        API, data=body,
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {api_key}"},
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        data = json.loads(r.read())
    return data["choices"][0]["message"]["content"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="面壁平台模型 ID，如 minicpm5-2b")
    ap.add_argument("--n", type=int, default=4, help="每场景候选条数")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "texts.json"))
    args = ap.parse_args()

    api_key = os.environ.get("MODELBEST_API_KEY")
    if not api_key:
        sys.exit("请先 export MODELBEST_API_KEY=...")

    out = {}
    for sid, desc in SCENARIOS.items():
        prompt = PROMPT.format(n=args.n, desc=desc)
        print(f"[{sid}] ...", flush=True)
        raw = chat(args.model, prompt, api_key)
        try:
            start = raw.index("[")
            end = raw.rindex("]") + 1
            out[sid] = json.loads(raw[start:end])
        except (ValueError, json.JSONDecodeError):
            print(f"  解析失败，原文: {raw!r}")
            out[sid] = [raw.strip()]
        for line in out[sid]:
            print(f"   - {line}")

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    print(f"\n-> {args.out}  （人工挑选后写入 voice_manifest.json）")


if __name__ == "__main__":
    main()
