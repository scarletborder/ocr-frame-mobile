#!/usr/bin/env python3
"""在桌面端复跑 iOS 端完全相同的版面预处理/后处理，用于核对坐标与阈值。

本脚本就是 impl/shard/src/core.cpp + src/engines/ort_layout.cpp 的参考实现：
  · letterbox 到 800x800，右下补 114
  · RGB、CHW、(x/255 - mean) / std，mean=[0.8286,0.8281,0.8282]、std=0.1889
  · im_shape=[h,w]、scale_factor=[1/r,1/r]
  · 输出 [N,7] = [class_id, score, x1, y1, x2, y2, order]，坐标在 letterbox 空间
  · 原图坐标 = 输出坐标 / r

用法：
    pip install onnxruntime numpy pillow
    python tools/verify_layout_onnx.py --model PP-DocLayoutV3.onnx --image page.png
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

SIDE = 800
PAD = 114
MEAN = np.array([0.8286, 0.8281, 0.8282], dtype=np.float32)
STD = np.array([0.1889, 0.1889, 0.1889], dtype=np.float32)

LABELS = [
    "abstract", "algorithm", "aside_text", "chart", "content",
    "display_formula", "doc_title", "figure_title", "footer", "footer_image",
    "footnote", "formula_number", "header", "header_image", "image",
    "inline_formula", "number", "paragraph_title", "reference", "reference_content",
    "seal", "table", "text", "vertical_text", "vision_footnote",
]
SKIP_ORDER = {
    "figure_title", "vision_footnote", "image", "chart", "table",
    "header", "header_image", "footer", "footer_image", "footnote", "aside_text",
}


def letterbox(image: Image.Image) -> tuple[np.ndarray, float]:
    width, height = image.size
    ratio = SIDE / max(width, height)
    draw_w = max(1, round(width * ratio))
    draw_h = max(1, round(height * ratio))
    canvas = Image.new("RGB", (SIDE, SIDE), (PAD, PAD, PAD))
    canvas.paste(image.convert("RGB").resize((draw_w, draw_h), Image.BICUBIC), (0, 0))
    array = np.asarray(canvas, dtype=np.float32) / 255.0
    array = (array - MEAN) / STD
    chw = np.transpose(array, (2, 0, 1))[None, ...]
    return np.ascontiguousarray(chw), draw_w / width


def main() -> int:
    parser = argparse.ArgumentParser(description="校验 PP-DocLayoutV3 ONNX 的 I/O 与坐标")
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--score", type=float, default=0.4)
    args = parser.parse_args()

    import onnxruntime as ort

    session = ort.InferenceSession(str(args.model), providers=["CPUExecutionProvider"])
    print("inputs :", [(item.name, item.shape, item.type) for item in session.get_inputs()])
    print("outputs:", [(item.name, item.shape, item.type) for item in session.get_outputs()])

    image = Image.open(args.image)
    chw, ratio = letterbox(image)
    width, height = image.size

    feed: dict[str, np.ndarray] = {}
    for item in session.get_inputs():
        if item.name == "image":
            feed[item.name] = chw
        elif item.name == "im_shape":
            feed[item.name] = np.array([[height, width]], dtype=np.float32)
        elif item.name == "scale_factor":
            feed[item.name] = np.array([[1.0 / ratio, 1.0 / ratio]], dtype=np.float32)
        else:
            print(f"!! 未知输入 {item.name}：模型不是 PaddleX 导出形态", file=sys.stderr)
            return 2

    outputs = session.run(None, feed)
    detection = next((out for out in outputs if out.ndim == 2 and out.shape[-1] == 7), None)
    if detection is None:
        print("!! 没有 [N,7] 检测输出，实际 shape：", [out.shape for out in outputs], file=sys.stderr)
        return 2

    rows = detection[detection[:, 1] >= args.score]
    print(f"\n共 {len(rows)} 个候选（score >= {args.score}），原图 {width}x{height}，r={ratio:.4f}")
    print(f"{'label':<18}{'score':>7}{'order':>8}   bbox(x1,y1,x2,y2)")
    for row in rows[np.argsort(rows[:, 6])]:
        class_id = int(round(float(row[0])))
        label = LABELS[class_id] if 0 <= class_id < len(LABELS) else f"cls{class_id}"
        x1, y1, x2, y2 = (row[2:6] / ratio).tolist()
        skipped = "  (不排序)" if label in SKIP_ORDER else ""
        print(f"{label:<18}{row[1]:>7.3f}{row[6]:>8.1f}   "
              f"({x1:.0f},{y1:.0f},{x2:.0f},{y2:.0f}){skipped}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
