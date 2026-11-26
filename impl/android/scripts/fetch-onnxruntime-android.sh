#!/usr/bin/env bash
# 取 ONNX Runtime Android 的预编译库与头文件（版面分析引擎）。
#
# Gradle 的 com.microsoft.onnxruntime:onnxruntime-android 只提供 Java API；
# 共享核心走 C API，因此这里直接从 Maven Central 的 AAR 里解出：
#   headers/*.h                     → app/src/main/cpp/onnxruntime/（编译用）
#   jni/<abi>/libonnxruntime.so     → app/src/main/cpp/onnxruntime/（链接用）
#                                   + app/src/main/jniLibs/<abi>/（打包进 APK，运行时必需）
#
# 注意：只 link 不打包的话，APK 里不会有 libonnxruntime.so，真机会报
# `dlopen failed: library "libonnxruntime.so" not found`。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/app/src/main/cpp/onnxruntime"
JNI_LIBS="${ROOT}/app/src/main/jniLibs"
VERSION="${ONNXRUNTIME_VERSION:-1.22.0}"
ABI="${ONNXRUNTIME_ABI:-arm64-v8a}"
AAR_URL="https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/${VERSION}/onnxruntime-android-${VERSION}.aar"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

echo "==> 下载 onnxruntime-android ${VERSION}"
if ! curl -fL --retry 3 --connect-timeout 20 "${AAR_URL}" -o "${WORK}/onnxruntime.aar"; then
  echo "!! 下载失败：${AAR_URL}" >&2
  echo "   可用版本：https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/maven-metadata.xml" >&2
  echo "   可用 ONNXRUNTIME_VERSION=<ver> 覆盖" >&2
  exit 1
fi
if [ ! -s "${WORK}/onnxruntime.aar" ]; then
  echo "!! 下载文件为空" >&2
  exit 1
fi

echo "==> 解压 AAR"
unzip -q -o "${WORK}/onnxruntime.aar" -d "${WORK}/aar"

SO="${WORK}/aar/jni/${ABI}/libonnxruntime.so"
if [ ! -f "${SO}" ]; then
  echo "!! AAR 里没有 jni/${ABI}/libonnxruntime.so" >&2
  exit 1
fi
if [ ! -d "${WORK}/aar/headers" ]; then
  echo "!! AAR 里没有 headers/（无法编译 C API）" >&2
  exit 1
fi

mkdir -p "${DEST}" "${JNI_LIBS}/${ABI}"
cp -R "${WORK}/aar/headers/." "${DEST}/"
cp "${SO}" "${DEST}/libonnxruntime.so"
cp "${SO}" "${JNI_LIBS}/${ABI}/libonnxruntime.so"

echo "==> 完成："
echo "    编译/链接：impl/android/app/src/main/cpp/onnxruntime/"
echo "    打包运行时：impl/android/app/src/main/jniLibs/${ABI}/libonnxruntime.so"
