#!/usr/bin/env bash
# 把官方 PP-DocLayoutV3 静态图（*_infer 目录）导出成端上用的 ONNX。
#
# 真正的转换逻辑只有一份：impl/ios/tools/export_layout_onnx.py（手机端也用同一个文件）。
# 本脚本只负责：建一个 paddle2onnx 能装的 Python（3.10–3.12）虚拟环境、装依赖、调过去。
#
# 用法：
#   bash scripts/export-layout-onnx.sh --model-dir ~/models/PP-DocLayoutV3_infer \
#       --output models/PP-DocLayoutV3.onnx
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOL="$(cd "${ROOT}/../../ios/tools" && pwd)/export_layout_onnx.py"
VENV="${ROOT}/third_party/p2o-venv"

[ -f "${TOOL}" ] || { echo "!! 找不到 ${TOOL}" >&2; exit 1; }

# paddle2onnx 没有 cp313 轮子，优先找一个 3.10–3.12。
PY=""
for candidate in "py -3.12" "py -3.11" "py -3.10" "python3.12" "python3.11" "python3.10"; do
  if ${candidate} -c "import sys" >/dev/null 2>&1; then
    PY="${candidate}"
    break
  fi
done
[ -n "${PY}" ] || {
  echo "!! 需要一个 Python 3.10–3.12（paddle2onnx 没有 cp313 轮子）。" >&2
  echo "   Windows: py -3.12 -m venv .venv；Ubuntu: sudo apt install python3.12-venv" >&2
  exit 1
}

if [ ! -x "${VENV}/bin/python" ] && [ ! -x "${VENV}/Scripts/python.exe" ]; then
  echo "==> 建虚拟环境：${PY} -m venv ${VENV}"
  ${PY} -m venv "${VENV}"
  VPY="${VENV}/bin/python"
  [ -x "${VPY}" ] || VPY="${VENV}/Scripts/python.exe"
  "${VPY}" -m pip install -q --upgrade pip
  echo "==> 安装 paddle2onnx / onnx / onnxruntime / pillow（首次约 1–2 分钟）"
  "${VPY}" -m pip install -q paddle2onnx onnx onnxruntime pillow
fi
VPY="${VENV}/bin/python"
[ -x "${VPY}" ] || VPY="${VENV}/Scripts/python.exe"

echo "==> 调用共享导出脚本：${TOOL}"
"${VPY}" "${TOOL}" "$@"
