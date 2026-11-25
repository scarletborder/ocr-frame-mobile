#!/usr/bin/env bash
# 拉取 llama.cpp 源码到 app/src/main/cpp/llama.cpp。
#
# 不需要单独构建：Gradle 的 externalNativeBuild 会在配置阶段
# add_subdirectory(llama.cpp)，由 NDK 直接编译出 llama / mtmd 静态库
# （CMakeLists 已把 LLAMA_BUILD_MTMD 打开、其它目标全关）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/app/src/main/cpp/llama.cpp"
REF="${LLAMA_REF:-master}"

if [ -d "${SRC}/.git" ]; then
  echo "==> 复用已有仓库 ${SRC}"
  git -C "${SRC}" fetch --depth 1 origin "${REF}" && git -C "${SRC}" checkout FETCH_HEAD
else
  echo "==> 克隆 llama.cpp (${REF})"
  mkdir -p "$(dirname "${SRC}")"
  git clone --depth 1 --branch "${REF}" https://github.com/ggml-org/llama.cpp "${SRC}"
fi

echo "==> 完成。首次 Gradle 构建会编译 llama.cpp（arm64-v8a，约 5–15 分钟）"
