#!/usr/bin/env bash
# 一条命令准备 iOS demo 工程：
#   1. 拉 onnxruntime.xcframework（版面分析）
#   2. 构建 llama.xcframework（VLM，含 libmtmd）
#   3. xcodegen 生成 VLOCR.xcodeproj
#
# 用法（不需要可执行位，直接 bash 调用）：
#   bash scripts/bootstrap.sh              # 全量：本地版面 + 本地 VLM
#   bash scripts/bootstrap.sh --no-llama   # 只拉 onnxruntime，先跑「远程服务」模式
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
mkdir -p Frameworks

SKIP_LLAMA=0
for arg in "$@"; do
  case "${arg}" in
    --no-llama) SKIP_LLAMA=1 ;;
    *) echo "未知参数：${arg}" >&2; exit 1 ;;
  esac
done

if [ ! -d Frameworks/onnxruntime.xcframework ]; then
  bash scripts/fetch-onnxruntime.sh
fi

if [ "${SKIP_LLAMA}" -eq 0 ] && [ ! -d Frameworks/llama.xcframework ]; then
  bash scripts/build-llama-xcframework.sh
fi

command -v xcodegen >/dev/null || {
  echo "缺少 xcodegen：brew install xcodegen" >&2
  exit 1
}

if [ -d Frameworks/llama.xcframework ]; then
  xcodegen generate
  echo "==> 已生成 VLOCR.xcodeproj（含本地 VLM 引擎）"
else
  # project.yml 里有一条 llama 依赖，缺框架时 XcodeGen 会直接报错；
  # 这里剥掉那三行生成一个临时 spec，保证「先跑链路」的路径可用。
  awk '
    /framework: Frameworks\/llama.xcframework/ { skip = 2; next }
    skip > 0 { skip--; next }
    { print }
  ' project.yml > project.no-llama.yml
  xcodegen generate --spec project.no-llama.yml
  echo "!! 未构建 Frameworks/llama.xcframework：本地 VLM 不可用，请在 App 设置里改用「远程服务」模式。" >&2
  echo "==> 已生成 VLOCR.xcodeproj（仅版面引擎）"
fi

cat <<'NEXT'

下一步：
  1. open VLOCR.xcodeproj
  2. Signing & Capabilities 里选自己的 Team（免费 Apple ID 也可以，7 天有效期）
  3. 真机（iPhone 17）插上，选中设备后 Cmd+R
  4. 首次运行后到「设置」里选两个模型（详见 impl/ios/DEVICE-TEST.md）
NEXT
