#!/usr/bin/env bash
# 桌面端依赖一键就位（Windows Git Bash / Ubuntu 通用）：
#   1. ONNX Runtime（版面）—— 从 NuGet 官方包取，win-x64 与 linux-x64 同一个包；
#   2. stb 单头文件（解码 + 叠加图）；
#   3. 可选：llama.cpp + libmtmd（真 VLM）—— 传 --with-llama。
#
# 产物：third_party/onnxruntime/{include,lib}、third_party/stb_*.h、third_party/llama.cpp + llama-build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD="${ROOT}/third_party"
ORT_VERSION="${ONNXRUNTIME_VERSION:-1.22.0}"
WITH_LLAMA=0

for arg in "$@"; do
  case "${arg}" in
    --with-llama) WITH_LLAMA=1 ;;
    -h|--help)
      echo "用法: bash scripts/bootstrap.sh [--with-llama]"
      exit 0
      ;;
    *) echo "未知参数：${arg}" >&2; exit 2 ;;
  esac
done

command -v python >/dev/null 2>&1 || command -v python3 >/dev/null 2>&1 || {
  echo "需要 python（用于解压 zip）" >&2; exit 1;
}
PY="$(command -v python || command -v python3)"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) RID="win-x64" ;;
  Linux*)               RID="linux-x64" ;;
  Darwin*)
    echo "!! macOS 请用 impl/ios/scripts/fetch-onnxruntime.sh 取 xcframework，" >&2
    echo "   或自行放置 third_party/onnxruntime/{include,lib}（libonnxruntime.dylib）。" >&2
    exit 1
    ;;
  *) echo "!! 未知平台：$(uname -s)" >&2; exit 1 ;;
esac

echo "==> 平台 ${RID}，ONNX Runtime ${ORT_VERSION}"

# ---------------------------------------------------------------- ONNX Runtime
if [ -f "${THIRD}/onnxruntime/include/onnxruntime_c_api.h" ] && \
   [ -n "$(ls -A "${THIRD}/onnxruntime/lib" 2>/dev/null || true)" ]; then
  echo "==> ONNX Runtime 已存在，跳过下载"
else
  mkdir -p "${THIRD}/onnxruntime" "${THIRD}/download"
  pkg="${THIRD}/download/onnxruntime-${ORT_VERSION}.nupkg"
  if [ ! -s "${pkg}" ]; then
    echo "==> 下载 Microsoft.ML.OnnxRuntime ${ORT_VERSION}（NuGet，约 120MB，含各平台二进制）"
    curl -fL --retry 3 \
      "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/${ORT_VERSION}" \
      -o "${pkg}"
  fi
  "${PY}" - "${pkg}" "${THIRD}/onnxruntime" "${RID}" <<'PY'
import os
import shutil
import sys
import zipfile

pkg, dest, rid = sys.argv[1], sys.argv[2], sys.argv[3]
include_dest = os.path.join(dest, "include")
lib_dest = os.path.join(dest, "lib")
os.makedirs(include_dest, exist_ok=True)
os.makedirs(lib_dest, exist_ok=True)

with zipfile.ZipFile(pkg) as zf:
    for name in zf.namelist():
        if name.startswith("build/native/include/") and name.endswith(".h"):
            with zf.open(name) as src, open(os.path.join(include_dest, os.path.basename(name)), "wb") as out:
                shutil.copyfileobj(src, out)
        elif name.startswith(f"runtimes/{rid}/native/"):
            base = os.path.basename(name)
            if not base:
                continue
            with zf.open(name) as src, open(os.path.join(lib_dest, base), "wb") as out:
                shutil.copyfileobj(src, out)
print("   include ->", include_dest)
print("   lib     ->", lib_dest)
PY
  [ -f "${THIRD}/onnxruntime/include/onnxruntime_c_api.h" ] || { echo "!! 头文件缺失" >&2; exit 1; }
  [ -n "$(ls -A "${THIRD}/onnxruntime/lib")" ] || { echo "!! 动态库缺失" >&2; exit 1; }
fi

# ---------------------------------------------------------------- stb
bash "${ROOT}/scripts/fetch-stb-image.sh"

# ---------------------------------------------------------------- llama.cpp
if [ "${WITH_LLAMA}" = "1" ]; then
  bash "${ROOT}/scripts/build-llama.sh"
else
  echo "==> 未构建 llama.cpp（真 VLM 需要）。需要时：bash scripts/bootstrap.sh --with-llama"
fi

cat <<'EOF'

==> 依赖就绪。下一步：

    bash scripts/build.sh
    ./build/vlcore_pc --help

模型（与手机完全一致）：
    PP-DocLayoutV3.onnx                      # 见 impl/ios/tools/export_layout_onnx.py
    PaddleOCR-VL-1.6-GGUF.gguf + -mmproj.gguf # huggingface-cli download PaddlePaddle/PaddleOCR-VL-1.6-GGUF
EOF
