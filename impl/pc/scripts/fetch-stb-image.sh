#!/usr/bin/env bash
# 取 stb 单头文件：stb_image.h（解码）+ stb_image_write.h（--overlay 画框导出 PNG）。
# 不入库：约 300KB 的第三方源码，用脚本按固定上游拉取。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE="https://raw.githubusercontent.com/nothings/stb/master"

mkdir -p "${ROOT}/third_party"
for header in stb_image.h stb_image_write.h; do
  dest="${ROOT}/third_party/${header}"
  if [ -s "${dest}" ]; then
    echo "==> 已存在：${dest}"
    continue
  fi
  echo "==> 下载 ${header}"
  curl -fL --retry 3 "${BASE}/${header}" -o "${dest}"
  [ -s "${dest}" ] || { echo "!! 下载失败：${header}" >&2; exit 1; }
done
echo "==> 完成：${ROOT}/third_party/stb_*.h"
