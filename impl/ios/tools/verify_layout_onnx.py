#!/usr/bin/env python3
"""在桌面端复跑 iOS 端完全相同的版面预处理/后处理，用于核对坐标与阈值。

本脚本就是 impl/shard/src/core.cpp + src/engines/ort_layout.cpp 的参考实现，
约定**逐条对齐官方 PP-DocLayoutV3 模型包里的 `inference.yml`**：
  · Resize：target_size 800x800、keep_ratio=false → 横竖各自**拉伸**，不补边
  · NormalizeImage：norm_type=none、mean 0、std 1 → **只除以 255**（不减均值不除方差）
  · im_shape=[原图高, 原图宽]、scale_factor=[1,1]
  · 输出 [N,7] = [class_id, score, x1, y1, x2, y2, order]，**坐标已是原图像素**

历史坑（2026-11-27 实测，同一页用官方 pdmodel 对照）：
  改用「letterbox + 补 114 + (x/255-mean)/std」后，整页只剩 3 条候选且正文被当成 image；
  再叠加「除一次缩放比」会把整段正文框缩到页面上半部分 → 端上表现为文本被截断。

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


def stretch(image: Image.Image) -> np.ndarray:
    """官方：拉伸到 800x800（不保持比例、不补边），只 /255。

    插值用双线性：官方 `inference.yml` 的 `interp: 2` 即 cv2.INTER_LINEAR，
    与 impl/shard/src/core.cpp 的 SampleBilinear 一致。
    """
    array = np.asarray(image.convert("RGB").resize((SIDE, SIDE), Image.BILINEAR), dtype=np.float32)
    array = array / 255.0
    chw = np.transpose(array, (2, 0, 1))[None, ...]
    return np.ascontiguousarray(chw)


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
    width, height = image.size
    chw = stretch(image)

    feed: dict[str, np.ndarray] = {}
    for item in session.get_inputs():
        if item.name == "image":
            feed[item.name] = chw
        elif item.name == "im_shape":
            feed[item.name] = np.array([[height, width]], dtype=np.float32)
        elif item.name == "scale_factor":
            # 固定 1：图内已按 im_shape 反算回原图坐标，传 1/r 会二次缩放。
            feed[item.name] = np.array([[1.0, 1.0]], dtype=np.float32)
        else:
            print(f"!! 未知输入 {item.name}：模型不是 PaddleX 导出形态", file=sys.stderr)
            return 2

    outputs = session.run(None, feed)
    detection = next((out for out in outputs if out.ndim == 2 and out.shape[-1] == 7), None)
    if detection is None:
        print("!! 没有 [N,7] 检测输出，实际 shape：", [out.shape for out in outputs], file=sys.stderr)
        return 2

    rows = detection[detection[:, 1] >= args.score]
    print(f"\n共 {len(rows)} 个候选（score >= {args.score}），原图 {width}x{height}")
    print(f"{'label':<18}{'score':>7}{'order':>8}   bbox(x1,y1,x2,y2)")
    for row in rows[np.argsort(rows[:, 6])]:
        class_id = int(round(float(row[0])))
        label = LABELS[class_id] if 0 <= class_id < len(LABELS) else f"cls{class_id}"
        x1, y1, x2, y2 = row[2:6].tolist()
        skipped = "  (不排序)" if label in SKIP_ORDER else ""
        inside = 0 <= x1 and 0 <= y1 and x2 <= width and y2 <= height
        warn = "" if inside else "  !! 框越出页面"
        print(f"{label:<18}{row[1]:>7.3f}{row[6]:>8.1f}   "
              f"({x1:.0f},{y1:.0f},{x2:.0f},{y2:.0f}){skipped}{warn}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
