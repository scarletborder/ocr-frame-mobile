#!/usr/bin/env bash
# 打一个「未签名 .ipa」，交给 Windows 侧 Sideloadly / AltStore 用 Apple ID 重签安装。
#
# 为什么不需要证书：Sideloadly 会遍历 .app 内所有 Mach-O（含 Frameworks/*.framework）
# 用你的证书重签，所以这里必须产出完全未签名的产物。
#
# 用法（Mac 或 CI 的 macOS runner）：
#   bash scripts/package-ipa.sh                 # Release，输出 build/VLOCR-unsigned.ipa
#   CONFIGURATION=Debug bash scripts/package-ipa.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

CONFIGURATION="${CONFIGURATION:-Release}"
OUT="${OUT:-build}"
ARCHIVE="${OUT}/VLOCR.xcarchive"
IPA="${OUT}/VLOCR-unsigned.ipa"

[ -d VLOCR.xcodeproj ] || { echo "缺少 VLOCR.xcodeproj：先跑 bash scripts/bootstrap.sh" >&2; exit 1; }

rm -rf "${ARCHIVE}" "${OUT}/Payload" "${IPA}"
mkdir -p "${OUT}"

echo "==> xcodebuild archive（未签名，${CONFIGURATION}）"
set -o pipefail
xcodebuild archive \
  -project VLOCR.xcodeproj \
  -scheme VLOCR \
  -configuration "${CONFIGURATION}" \
  -sdk iphoneos \
  -destination 'generic/platform=iOS' \
  -archivePath "${ARCHIVE}" \
  CODE_SIGNING_ALLOWED=NO \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGN_IDENTITY="" \
  DEVELOPMENT_TEAM="" \
  PROVISIONING_PROFILE_SPECIFIER="" \
  ONLY_ACTIVE_ARCH=NO \
  2>&1 | tee "${OUT}/xcodebuild.log" | tail -60

APP="${ARCHIVE}/Products/Applications/VLOCR.app"
[ -d "${APP}" ] || { echo "!! archive 里没有 VLOCR.app" >&2; exit 1; }

echo "==> 组装 Payload"
mkdir -p "${OUT}/Payload"
# ditto 保留符号链接与权限（framework 内部结构不能丢）
ditto "${APP}" "${OUT}/Payload/VLOCR.app"

echo "==> 校验产物"
lipo -archs "${OUT}/Payload/VLOCR.app/VLOCR"
if [ -d "${OUT}/Payload/VLOCR.app/Frameworks" ]; then
  ls "${OUT}/Payload/VLOCR.app/Frameworks"

  # 动态框架若没被 embed，装到手机上会 dyld 崩溃，这里提前拦住
  if [ ! -d "${OUT}/Payload/VLOCR.app/Frameworks/llama.framework" ] && \
     [ -d "${ROOT}/Frameworks/llama.xcframework" ]; then
    echo "!! 工程引用了 llama.xcframework，但 .app 里没有 llama.framework（Embed 未生效）" >&2
    exit 1
  fi

  # 动态库型 xcframework（ONNX Runtime 的 GitHub 镜像就是这种）install_name 常带版本号
  # （@rpath/libonnxruntime.1.22.0.dylib），而嵌入的文件名可能是 libonnxruntime.dylib。
  # 名字对不上会在设备上 dyld 报 "Library not loaded"，这里按 install_name 补齐副本。
  FRAMEWORKS_DIR="${OUT}/Payload/VLOCR.app/Frameworks"
  for dylib in "${FRAMEWORKS_DIR}"/*.dylib; do
    [ -e "${dylib}" ] || continue
    install_name="$(otool -D "${dylib}" 2>/dev/null | tail -1)"
    [ -n "${install_name}" ] || continue
    case "${install_name}" in
      @rpath/*|@loader_path/*|@executable_path/*) ;;
      *) continue ;;
    esac
    wanted="$(basename "${install_name}")"
    if [ ! -e "${FRAMEWORKS_DIR}/${wanted}" ]; then
      echo "    补齐 ${wanted}（install_name 指向它，但嵌入的文件名是 $(basename "${dylib}")）"
      cp "${dylib}" "${FRAMEWORKS_DIR}/${wanted}"
    fi
  done
fi

echo "==> 打包 ipa"
( cd "${OUT}" && ditto -c -k --sequesterRsrc --keepParent Payload VLOCR-unsigned.ipa )

SIZE="$(du -h "${IPA}" | cut -f1)"
echo "==> 完成：${IPA}（${SIZE}，未签名）"
echo "    Windows 侧：Sideloadly 打开这个 ipa → 填 Apple ID → Start"
