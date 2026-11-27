#!/usr/bin/env bash
# 构建 llama.cpp + libmtmd（桌面端真 VLM 用），产物落在 third_party/llama.cpp + third_party/llama-build。
#
# 为什么必须带 mtmd：PaddleOCR-VL 是 M-RoPE 模型，视觉 token 的 4 分量位置与非因果注意力
# 只有 libmtmd 的 mtmd_helper_eval_chunks 能正确表达（见 impl/shard/src/engines/mtmd_vlm.cpp）。
#
# 平台说明：
#   · Ubuntu / macOS / Windows(MSVC)：正常路径；
#   · Windows(MinGW)：能编出来，但上游在本仓库验证环境里 mtmd_init_from_file 会段错误，
#     所以默认拒绝；确实要试就加 --force-mingw。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD="${ROOT}/third_party"
SRC="${THIRD}/llama.cpp"
BUILD="${THIRD}/llama-build"
REF="${LLAMA_REF:-master}"
FORCE_MINGW=0

for arg in "$@"; do
  case "${arg}" in
    --force-mingw) FORCE_MINGW=1 ;;
    -h|--help) echo "用法: bash scripts/build-llama.sh [--force-mingw]"; exit 0 ;;
    *) echo "未知参数：${arg}" >&2; exit 2 ;;
  esac
done

command -v cmake >/dev/null || {
  echo "!! 缺 cmake。Windows：pip install cmake 或装 CMake 3.28+；Ubuntu：sudo apt install cmake" >&2
  exit 1
}

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    if ! command -v cl >/dev/null 2>&1 && [ "${FORCE_MINGW}" != "1" ]; then
      cat >&2 <<'EOF'
!! 当前是 MinGW 环境，未检测到 MSVC(cl)。
   已知问题：MinGW 编出来的 libmtmd 在 mtmd_init_from_file 会段错误（上游问题，官方
   llama-mtmd-cli 同样崩）。真 VLM 请开 "x64 Native Tools Command Prompt for VS" 再跑本脚本；
   只验证版面/提示词/Markdown 则不需要 llama.cpp（--detect-only / --stub-vlm）。
   确实要用 MinGW 试：bash scripts/build-llama.sh --force-mingw
EOF
      exit 1
    fi
    ;;
esac

if [ ! -d "${SRC}/.git" ]; then
  echo "==> 克隆 llama.cpp (${REF}) → ${SRC}"
  git clone --depth 1 --branch "${REF}" https://github.com/ggml-org/llama.cpp "${SRC}"
else
  echo "==> 复用已有源码 ${SRC}"
fi

[ -f "${SRC}/tools/mtmd/mtmd-helper.h" ] || {
  echo "!! ${SRC}/tools/mtmd/mtmd-helper.h 不存在：llama.cpp 太旧，M-RoPE 求值会失败" >&2
  echo "   删掉 ${SRC} 重新跑本脚本（或 git pull 上游 master）" >&2
  exit 1
}

GENERATOR_ARGS=()
if command -v cl >/dev/null 2>&1 && [ -z "${CMAKE_GENERATOR:-}" ]; then
  GENERATOR_ARGS=(-A x64)
fi

echo "==> 配置（llama + mtmd，关掉示例/测试/服务端）"
cmake -S "${SRC}" -B "${BUILD}" ${CMAKE_GENERATOR:+-G "${CMAKE_GENERATOR}"} "${GENERATOR_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_TOOLS=OFF \
  -DLLAMA_BUILD_SERVER=OFF \
  -DLLAMA_BUILD_COMMON=OFF \
  -DLLAMA_BUILD_MTMD=ON \
  -DGGML_OPENMP=OFF

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
echo "==> 构建 llama / mtmd（-j ${JOBS}）"
cmake --build "${BUILD}" --target llama mtmd --config Release -j "${JOBS}"

# 校验：mtmd 必须真的进了产物，否则 mtmd_vlm.cpp 会静默退化成不支持 M-RoPE 的实现。
FOUND_HELPER=0
for lib in "${BUILD}/tools/mtmd/libmtmd.a" "${BUILD}/tools/mtmd/Release/mtmd.lib" \
           "${BUILD}/tools/mtmd/Release/libmtmd.a"; do
  [ -f "${lib}" ] && FOUND_HELPER=1
done
[ "${FOUND_HELPER}" = "1" ] || {
  echo "!! 找不到 libmtmd（${BUILD}/tools/mtmd）：check LLAMA_BUILD_MTMD=ON" >&2
  exit 1
}

echo "==> 完成：${BUILD}"
