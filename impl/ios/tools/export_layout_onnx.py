#!/usr/bin/env python3
"""PP-DocLayoutV3 静态图推理模型 → ONNX（给 iOS 端 ONNX Runtime 用）。

为什么走静态图而不是仓库里 data/models/vl_layout 的 safetensors：
  · 仓库里那份是 PaddleX transformers 引擎用的 HF 目录，iOS 侧没有 transformers；
  · PaddleX 官方另外发布静态图 `PP-DocLayoutV3_infer`（inference.pdmodel/pdiparams），
    paddle2onnx 对它的转换误差实测约 1.57%（社区 V2/V3 对比），可用于移动端。

用法：
    # 注意：paddle2onnx 只发 cp38–cp312 的轮子（无 sdist、无 cp313），
    #       所以请用 Python 3.10/3.11/3.12 建 venv，不要用 3.13。
    py -3.12 -m venv .venv && .venv/Scripts/activate     # Windows
    pip install paddle2onnx onnx onnxruntime pillow
    python tools/export_layout_onnx.py --model-dir ~/models/PP-DocLayoutV3_infer \
        --output ~/models/PP-DocLayoutV3.onnx

导出的图必须是 PaddleX 形态的三个输入（image / im_shape / scale_factor）与
[N,7] 检测输出，见 tools/verify_layout_onnx.py 与 impl/shard/src/engines/ort_layout.cpp 的注释。
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def resolve_paddle2onnx() -> list[str] | None:
    """定位 paddle2onnx 可执行文件。

    paddle2onnx 的 wheel 里没有 `__main__.py`，所以 `python -m paddle2onnx` 会失败；
    优先用控制台脚本（venv 的 Scripts/paddle2onnx.exe / bin/paddle2onnx）。
    """
    found = shutil.which("paddle2onnx")
    if found:
        return [found]
    for name in ("paddle2onnx.exe", "paddle2onnx"):
        candidate = Path(sys.executable).with_name(name)
        if candidate.exists():
            return [str(candidate)]
    probe = subprocess.run([sys.executable, "-m", "paddle2onnx", "--help"],
                           capture_output=True, text=True)
    if probe.returncode == 0:
        return [sys.executable, "-m", "paddle2onnx"]
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="PP-DocLayoutV3 → ONNX")
    parser.add_argument("--model-dir", required=True, type=Path,
                        help="PP-DocLayoutV3_infer 目录（含 inference.pdmodel / inference.pdiparams）")
    parser.add_argument("--output", default="PP-DocLayoutV3.onnx", type=Path)
    parser.add_argument("--opset", type=int, default=14)
    args = parser.parse_args()

    model_dir: Path = args.model_dir.expanduser()
    pdmodel = model_dir / "inference.pdmodel"
    pdiparams = model_dir / "inference.pdiparams"
    if not pdmodel.exists() or not pdiparams.exists():
        print(f"!! {model_dir} 下没有 inference.pdmodel / inference.pdiparams", file=sys.stderr)
        print("   请确认下载的是 PaddleX 静态图推理模型（*_infer.tar 解包后的目录），"
              "不是 transformers 引擎用的 safetensors 目录。", file=sys.stderr)
        return 2

    launcher = resolve_paddle2onnx()
    if launcher is None:
        print("!! 找不到 paddle2onnx。请先 pip install paddle2onnx（Python 3.10–3.12）", file=sys.stderr)
        print("   注意：paddle2onnx 没有 cp313 轮子，Python 3.13 装不上。", file=sys.stderr)
        return 2

    command = launcher + [
        "--model_dir", str(model_dir),
        "--model_filename", "inference.pdmodel",
        "--params_filename", "inference.pdiparams",
        "--save_file", str(args.output),
        "--opset_version", str(args.opset),
        "--enable_onnx_checker", "True",
    ]
    print("==>", " ".join(command))
    subprocess.run(command, check=True)
    print(f"==> 完成：{args.output}")
    print("   下一步：python tools/verify_layout_onnx.py --model "
          f"{args.output} --image <一张真实页面.png>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
