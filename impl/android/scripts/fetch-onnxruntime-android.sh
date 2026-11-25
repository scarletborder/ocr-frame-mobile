#!/usr/bin/env bash
# 取 ONNX Runtime Android 的预编译库与头文件（版面分析引擎）。
#
# Gradle 的 com.microsoft.onnxruntime:onnxruntime-android 只提供 Java API；
# 共享核心走 C API，因此这里直接从 Maven 的 AAR 里解出 headers + libonnxruntime.so。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/app/src/main/cpp/onnxruntime"
VERSION="${ONNXRUNTIME_VERSION:-1.20.0}"
AAR_URL="https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/${VERSION}/onnxruntime-android-${VERSION}.aar"
WORK="$(mktemp -d)"

echo "==> 下载 onnxruntime-android ${VERSION}"
curl -fL --retry 3 "${AAR_URL}" -o "${WORK}/onnxruntime.aar"

echo "==> 解压 AAR"
unzip -q -o "${WORK}/onnxruntime.aar" -d "${WORK}/aar"

SO="${WORK}/aar/jni/arm64-v8a/libonnxruntime.so"
if [ ! -f "${SO}" ]; then
  echo "!! AAR 里没有 arm64-v8a/libonnxruntime.so，请检查 ${AAR_URL}" >&2
  exit 1
fi

mkdir -p "${DEST}"
cp "${SO}" "${DEST}/libonnxruntime.so"

if [ -d "${WORK}/aar/headers" ]; then
  cp -R "${WORK}/aar/headers/." "${DEST}/"
fi
# 兜底：headers 目录结构变化时，直接按文件名找 C API 头
if [ ! -f "${DEST}/onnxruntime_c_api.h" ]; then
  find "${WORK}/aar" -name 'onnxruntime_c_api.h' -exec cp {} "${DEST}/" \;
fi

if [ ! -f "${DEST}/onnxruntime_c_api.h" ]; then
  echo "!! 没找到 onnxruntime_c_api.h：本次构建将不含版面引擎（CMake 会给出警告）" >&2
  exit 1
fi

rm -rf "${WORK}"
echo "==> 完成：impl/android/app/src/main/cpp/onnxruntime/"
