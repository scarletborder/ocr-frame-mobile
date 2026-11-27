#!/usr/bin/env bash
# 构建 vlcore_pc（桌面端等效验证器）。依赖先就位：bash scripts/bootstrap.sh [--with-llama]
#
# 自动挑生成器：有 MSVC(cl) 就用 VS 生成器，否则用 MinGW / Unix Makefiles。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

command -v cmake >/dev/null || {
  echo "!! 缺 cmake。Windows：pip install cmake；Ubuntu：sudo apt install cmake" >&2
  exit 1
}

ARGS=(-S "${ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)

if [ -n "${CMAKE_GENERATOR:-}" ]; then
  ARGS+=(-G "${CMAKE_GENERATOR}")
elif command -v cl >/dev/null 2>&1; then
  :   # 让 CMake 自己选已安装的最新 VS 生成器
elif command -v g++ >/dev/null 2>&1; then
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ARGS+=(-G "MinGW Makefiles") ;;
    *)                    ARGS+=(-G "Unix Makefiles") ;;
  esac
fi

echo "==> 配置：cmake ${ARGS[*]}"
cmake "${ARGS[@]}"

echo "==> 构建（-j ${JOBS}）"
cmake --build "${BUILD_DIR}" --config Release -j "${JOBS}"

EXE="${BUILD_DIR}/vlcore_pc"
[ -x "${EXE}" ] || EXE="${BUILD_DIR}/vlcore_pc.exe"
[ -x "${EXE}" ] || EXE="${BUILD_DIR}/Release/vlcore_pc.exe"
[ -f "${EXE}" ] || { echo "!! 找不到产物，看看上面的编译输出" >&2; exit 1; }

echo
echo "==> 产物：${EXE}"
MODELS="${MODELS:-${ROOT}/models}"
if [ -d "${MODELS}" ]; then
  echo
  echo "发现模型目录，可直接跑："
  echo "  \"${EXE}\" --detect-only --layout \"${MODELS}/PP-DocLayoutV3.onnx\" --image <页面.png> --overlay page.boxes.png"
  echo "  \"${EXE}\" --image <页面.png> --layout \"${MODELS}/PP-DocLayoutV3.onnx\" \\"
  echo "      --vlm \"${MODELS}/PaddleOCR-VL-1.6-GGUF.gguf\" --mmproj \"${MODELS}/PaddleOCR-VL-1.6-GGUF-mmproj.gguf\""
fi
