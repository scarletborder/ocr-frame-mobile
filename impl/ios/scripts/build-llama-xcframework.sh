#!/usr/bin/env bash
# 构建 llama.xcframework（含 libmtmd，多模态 C API）。
#
# 官方 build-xcframework.sh 默认 LLAMA_BUILD_MTMD=ON，会把 tools/mtmd 下的
# libmtmd.a 合进 llama.framework，并把 mtmd.h / mtmd-helper.h 一起拷进 Headers/。
# 因此 App 侧只需 import <llama/llama.h> 与 <llama/mtmd.h>，无需自带 C++ 源码。
#
# 需要：Xcode（xcrun）、cmake ≥ 3.28。首次构建约 10–20 分钟。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLAMA_REF="${LLAMA_REF:-master}"
IOS_MIN="${IOS_MIN_OS_VERSION:-17.0}"
SRC="${LLAMA_SRC:-${ROOT}/.build/llama.cpp}"
# CI 只出真机 ipa 时可以只编 ios-device，省一半时间
PLATFORMS="${LLAMA_PLATFORMS:-ios-device ios-sim}"

command -v cmake >/dev/null || { echo "缺少 cmake（需 3.28+）：brew install cmake" >&2; exit 1; }
command -v xcrun >/dev/null || { echo "缺少 Xcode 命令行工具（xcrun）" >&2; exit 1; }

if [ ! -d "${SRC}/.git" ]; then
  echo "==> 克隆 llama.cpp (${LLAMA_REF}) 到 ${SRC}"
  mkdir -p "$(dirname "${SRC}")"
  git clone --depth 1 --branch "${LLAMA_REF}" https://github.com/ggml-org/llama.cpp "${SRC}"
else
  echo "==> 复用已有仓库 ${SRC}"
fi

cd "${SRC}"
echo "==> 构建 iOS 切片（${PLATFORMS}），部署目标 iOS ${IOS_MIN}"
# shellcheck disable=SC2086
IOS_MIN_OS_VERSION="${IOS_MIN}" ./build-xcframework.sh ${PLATFORMS}

if [ ! -d "${SRC}/build-apple/llama.xcframework" ]; then
  echo "!! 构建产物缺失：${SRC}/build-apple/llama.xcframework" >&2
  exit 1
fi

mkdir -p "${ROOT}/Frameworks"
rm -rf "${ROOT}/Frameworks/llama.xcframework"
cp -R "${SRC}/build-apple/llama.xcframework" "${ROOT}/Frameworks/"

echo "==> 完成：impl/ios/Frameworks/llama.xcframework"
