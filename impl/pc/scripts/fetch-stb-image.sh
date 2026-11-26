#!/usr/bin/env bash
# 取 stb_image.h（单头文件 PNG/JPEG 解码，public domain）。
# 不入库：280KB 的第三方源码，用脚本按固定 commit 拉取。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/third_party/stb_image.h"
URL="https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"

mkdir -p "${ROOT}/third_party"
if [ -s "${DEST}" ]; then
  echo "==> 已存在：${DEST}"
  exit 0
fi

echo "==> 下载 stb_image.h"
curl -fL --retry 3 "${URL}" -o "${DEST}"
[ -s "${DEST}" ] || { echo "!! 下载失败" >&2; exit 1; }
echo "==> 完成：${DEST}"
