#!/usr/bin/env bash
# 下载 ONNX Runtime 的 iOS xcframework（C API，静态库）。
#
# 官方 iOS 安装方式就是 CocoaPods 的 pod-archive；这里直接解出 xcframework 放进
# Frameworks/，避免为一个静态库引入 CocoaPods 工作区。
# 可用 ONNXRUNTIME_VERSION 覆盖版本（默认 1.22.0）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${ONNXRUNTIME_VERSION:-1.22.0}"
ARCHIVE="https://download.onnxruntime.ai/pod-archive-onnxruntime-c-${VERSION}.zip"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

echo "==> 下载 onnxruntime-c ${VERSION}"
if ! curl -fL --retry 3 --connect-timeout 20 "${ARCHIVE}" -o "${WORK}/onnxruntime-c.zip"; then
  cat >&2 <<'FALLBACK'
!! 下载失败。两条替代路径（任选其一，然后重新执行本脚本之外的后续步骤）：

   A. 换版本重试：ONNXRUNTIME_VERSION=1.20.0 bash scripts/fetch-onnxruntime.sh
      （可用版本见 https://onnxruntime.ai/docs/install/#install-on-ios）

   B. 用 CocoaPods 拿同一个包：
        curl -fL -o ort.zip "https://download.onnxruntime.ai/pod-archive-onnxruntime-c-1.20.0.zip"
        unzip -q ort.zip -d /tmp/ort && find /tmp/ort -name 'onnxruntime.xcframework'
        cp -R <上一步找到的路径> Frameworks/
FALLBACK
  exit 1
fi

echo "==> 解压"
unzip -q -o "${WORK}/onnxruntime-c.zip" -d "${WORK}/pkg"

FRAMEWORK="$(find "${WORK}/pkg" -maxdepth 4 -name 'onnxruntime.xcframework' -print -quit)"
if [ -z "${FRAMEWORK}" ]; then
  echo "!! 压缩包里没有 onnxruntime.xcframework，请检查 ${ARCHIVE}" >&2
  exit 1
fi

# 提前校验：App 侧用 #include <onnxruntime/onnxruntime_c_api.h>，头文件不在就会静默退化
if ! find "${FRAMEWORK}" -name 'onnxruntime_c_api.h' -print -quit | grep -q .; then
  echo "!! xcframework 里没有 onnxruntime_c_api.h，无法编译版面引擎" >&2
  exit 1
fi

mkdir -p "${ROOT}/Frameworks"
rm -rf "${ROOT}/Frameworks/onnxruntime.xcframework"
cp -R "${FRAMEWORK}" "${ROOT}/Frameworks/"

echo "==> 完成：impl/ios/Frameworks/onnxruntime.xcframework"
