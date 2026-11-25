#!/usr/bin/env bash
# 取 ONNX Runtime 的 iOS xcframework（仅 C API）。
#
# 背景：官方唯一的分发地址是 CocoaPods 的 pod-archive
#   https://download.onnxruntime.ai/pod-archive-onnxruntime-c-<ver>.zip
# 但该主机在部分网络下返回的证书与域名不匹配（curl: (60) no alternative certificate
# subject name matches ...），GitHub Actions 上会直接失败。因此按顺序尝试多个来源：
#
#   1. $ONNXRUNTIME_XCFRAMEWORK_URL（完全自定义，可配 $ONNXRUNTIME_XCFRAMEWORK_SHA256）
#   2. 官方 pod-archive
#   3. GitHub 上的 1.22.0 镜像（已钉 SHA256；动态 dylib 型 xcframework，含 C API 头）
#
# 产物统一落到 Frameworks/onnxruntime.xcframework，两种形态（静态 framework / 动态 dylib）
# 都能被工程消费：头文件路径由 shard 的 __has_include 三路探测，embed 统一为 true。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/Frameworks/onnxruntime.xcframework"
VERSION="${ONNXRUNTIME_VERSION:-1.22.0}"

OFFICIAL_URL="https://download.onnxruntime.ai/pod-archive-onnxruntime-c-${VERSION}.zip"
MIRROR_VERSION="1.22.0"
MIRROR_URL="https://github.com/vannmex/vannmex-ios-ml-artifacts/releases/download/v1.0.0/onnxruntime-ios-${MIRROR_VERSION}.xcframework.zip"
MIRROR_SHA256="37dd16e26b1f7bb96696f27b0949d345672f7b6bc53b3ede5bd4e5836632c441"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

hash_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    sha256sum "$1" | cut -d' ' -f1
  fi
}

framework_shape() {
  local fw="$1"
  if find "${fw}" -maxdepth 3 -name 'libonnxruntime*.dylib' -print -quit | grep -q .; then
    echo "dynamic"
  elif find "${fw}" -maxdepth 3 -type d -name 'onnxruntime.framework' -print -quit | grep -q .; then
    echo "static-framework"
  else
    echo "unknown"
  fi
}

verify_framework() {
  local fw="$1"
  [ -d "${fw}" ] || return 1
  [ -d "${fw}/ios-arm64" ] || { echo "   缺少 ios-arm64 切片" >&2; return 1; }
  find "${fw}" -name 'onnxruntime_c_api.h' -print -quit | grep -q . || { echo "   缺少 onnxruntime_c_api.h" >&2; return 1; }
  [ "$(framework_shape "${fw}")" != "unknown" ] || { echo "   既没有 framework 也没有 dylib" >&2; return 1; }
  return 0
}

# try_source <name> <url> [sha256]
try_source() {
  local name="$1" url="$2" expected_sha="${3:-}"
  local zip="${WORK}/pkg-${RANDOM}.zip"
  local unpack="${WORK}/unpack-${RANDOM}"

  echo "==> 尝试来源：${name}"
  echo "    ${url}"
  if ! curl -fL --retry 2 --connect-timeout 20 "${url}" -o "${zip}"; then
    echo "    下载失败，换下一个来源" >&2
    return 1
  fi
  # 某些 curl 后端（如 Windows schannel）TLS 失败时仍可能留下 0 字节文件且返回 0
  if [ ! -s "${zip}" ]; then
    echo "    下载文件为空（TLS/网络失败），换下一个来源" >&2
    return 1
  fi
  if ! unzip -t -q "${zip}" >/dev/null 2>&1; then
    echo "    压缩包损坏，换下一个来源" >&2
    return 1
  fi

  if [ -n "${expected_sha}" ]; then
    local actual
    actual="$(hash_of "${zip}")"
    if [ "${actual}" != "${expected_sha}" ]; then
      echo "    SHA256 不匹配（expected ${expected_sha}, actual ${actual}）" >&2
      return 1
    fi
    echo "    SHA256 校验通过"
  fi

  unzip -q -o "${zip}" -d "${unpack}" || { echo "    解压失败" >&2; return 1; }

  local fw
  fw="$(find "${unpack}" -maxdepth 5 -name 'onnxruntime.xcframework' -print -quit)"
  if [ -z "${fw}" ] || ! verify_framework "${fw}"; then
    echo "    包结构不符合预期" >&2
    return 1
  fi

  mkdir -p "${ROOT}/Frameworks"
  rm -rf "${DEST}"
  cp -R "${fw}" "${DEST}"
  echo "==> 完成：impl/ios/Frameworks/onnxruntime.xcframework（形态：$(framework_shape "${DEST}")）"
  return 0
}

if [ -n "${ONNXRUNTIME_XCFRAMEWORK_URL:-}" ]; then
  try_source "自定义 URL" "${ONNXRUNTIME_XCFRAMEWORK_URL}" "${ONNXRUNTIME_XCFRAMEWORK_SHA256:-}" && exit 0
fi

try_source "官方 pod-archive ${VERSION}" "${OFFICIAL_URL}" && exit 0

if [ "${VERSION}" != "${MIRROR_VERSION}" ]; then
  echo "!! 镜像固定为 ${MIRROR_VERSION}，与 ONNXRUNTIME_VERSION=${VERSION} 不一致" >&2
fi
try_source "GitHub 镜像 ${MIRROR_VERSION}" "${MIRROR_URL}" "${MIRROR_SHA256}" && exit 0

cat >&2 <<FALLBACK
!! 所有来源都失败了。手工兜底（任一即可）：

   A. 浏览器下载后放到 Frameworks/：
      ${MIRROR_URL}
      （sha256 应为 ${MIRROR_SHA256}）
      解压后把 onnxruntime.xcframework 整个目录放到 impl/ios/Frameworks/ 下。

   B. 内网镜像/自建存储：
      ONNXRUNTIME_XCFRAMEWORK_URL=https://your.mirror/ort.zip \\
      ONNXRUNTIME_XCFRAMEWORK_SHA256=<sha256> bash scripts/fetch-onnxruntime.sh
FALLBACK
exit 1
