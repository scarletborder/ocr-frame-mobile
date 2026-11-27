# OCR Frame Mobile · VL 线端侧最小部署

把 **PaddleOCR-VL 完整两阶段产线**（版面分析 + 视觉语言模型）搬到手机上的最小可运行示例，
iOS 与 Android 共用同一份 C++ 核心。

> 本仓库是从主项目（私有）拆出的**公开子仓库**，只包含移动端 demo、共享核心和文档；
> 不含后端、前端、proto 契约与任何模型权重。

## 1. 目录

```
.
├── .github/workflows/ios-ipa.yml   macOS runner 产出未签名 ipa（Windows 用户走这条）
├── impl/
│   ├── shard/                      共享核心（C++17）：VL 线规则的唯一端侧真相
│   ├── ios/                        SwiftUI + ObjC++ 门面
│   └── android/                    Jetpack Compose + JNI
└── docs/
    ├── 01-capabilities.md          能力边界（做什么 / 不做什么 / 精度与性能边界）
    ├── 02-features.md              特性清单与两端差异
    └── 03-ui-ux.md                 UI/UX 设计稿（线框图、组件规格、状态矩阵、RTL）
```

## 2. 快速开始

### iOS（Windows 也能装机）

```bash
# 产物是未签名 ipa，签名与安装在 Windows 用 Sideloadly + iTunes 完成
# 没有 Mac → 推到 GitHub，跑 Actions → iOS IPA (unsigned)，下载 artifact
# 有 Mac →
cd impl/ios
bash scripts/fetch-onnxruntime.sh && bash scripts/build-llama-xcframework.sh
bash scripts/bootstrap.sh && bash scripts/package-ipa.sh   # → build/VLOCR-unsigned.ipa
```

完整步骤（Sideloadly、7 天证书、iTunes 传模型、验收清单）：[`impl/ios/DEVICE-TEST.md`](impl/ios/DEVICE-TEST.md)。

### Android

```bash
# CI：push 即构建（或 Actions 手动 Run workflow）→ 下载 artifact VLOCR-apk
# 本机：
cd impl/android
bash scripts/fetch-onnxruntime-android.sh     # ONNX Runtime 头文件 + arm64 so
bash scripts/build-llama-android.sh           # llama.cpp 源码（NDK 直接编译）
gradle assembleRelease                        # release 变体 + debug 证书签名，可直接 sideload
```

详见 [`impl/android/README.md`](impl/android/README.md)。

### 模型（两端一致，需自备）

| 用途 | 文件 | 来源 |
|---|---|---|
| 版面分析 | `PP-DocLayoutV3.onnx` | 官方 `PP-DocLayoutV3_infer` 静态图 → `bash impl/pc/scripts/export-layout-onnx.sh`（内部就是 `impl/ios/tools/export_layout_onnx.py`）。**必须用这条路径导出的模型**：端侧预处理严格按官方 `inference.yml`（拉伸 800×800 + 仅 `/255`），社区那些 `/255 + ImageNet mean/std` 的导出会让整页只剩几条候选、正文被并成大图 |
| 视觉语言模型 | `PaddleOCR-VL-1.6-GGUF.gguf` + `-mmproj.gguf` | `PaddlePaddle/PaddleOCR-VL-1.6-GGUF`（HF / ModelScope） |

模型权重**不入库**，安装后由用户导入（iOS：设置页或 iTunes 文件共享；Android：设置页选择器）。

### 桌面端等效验证（Windows / Ubuntu）

改一行就要出 ipa 才能看效果太慢：`impl/pc` 编译的是**同一份 `impl/shard`**、加载**同一组模型**，
所以桌面上跑出来的块、文本、日志与手机一致。

```bash
cd impl/pc
bash scripts/bootstrap.sh      # ONNX Runtime + stb 头（真 VLM 再加 --with-llama）
bash scripts/build.sh
./build/vlcore_pc --image page.png --layout models/PP-DocLayoutV3.onnx \
    --vlm models/PaddleOCR-VL-1.6-GGUF.gguf --mmproj models/PaddleOCR-VL-1.6-GGUF-mmproj.gguf \
    --overlay page.boxes.png --boxes page.boxes.csv
```

详见 [`impl/pc/README.md`](impl/pc/README.md)。

## 3. 共享核心

`impl/shard` 里放着两端共用的**全部 VL 线规则**：25 类版面标签字典、官方提示词、版面预处理
（拉伸 800×800 + /255）、检测后处理（阈值 / NMS / 整页伪框 / 阅读顺序）、Markdown 合并、
两阶段编排，以及两个推理引擎（ONNX Runtime / llama.cpp+mtmd）。

平台侧只剩：图片解码、引擎链接方式、模型文件管理、UI。详见 [`impl/shard/README.md`](impl/shard/README.md)。

> 这些规则与主项目 `impl/ocr_core/vl/{blocks,markdown,options,pipeline}.py` 同源，
> 改动必须两侧同步（对照表见 shard README §4）。

## 4. 文档

| 文档 | 内容 |
|---|---|
| [docs/01-capabilities.md](docs/01-capabilities.md) | 能力边界：输入、输出、明确不支持、精度/性能/隐私边界 |
| [docs/02-features.md](docs/02-features.md) | 特性清单、两端能力矩阵、预设参数 |
| [docs/03-ui-ux.md](docs/03-ui-ux.md) | 信息架构、线框图、组件与排版规格、状态矩阵、RTL |
| [impl/shard/README.md](impl/shard/README.md) | 共享边界、C API、编译矩阵 |
| [impl/pc/README.md](impl/pc/README.md) | 桌面端等效验证器：依赖/构建/与手机对日志的方法 |
| [impl/ios/README.md](impl/ios/README.md) | iOS 技术栈、目录、模型准备 |
| [impl/ios/DEVICE-TEST.md](impl/ios/DEVICE-TEST.md) | 真机安装 Runbook（Windows + Sideloadly） |
| [impl/android/README.md](impl/android/README.md) | Android 构建、模型导入、排错 |

## 5. 不包含

- 模型权重、`.xcframework`、`.so`、`llama.cpp` 源码（由 `scripts/` 拉取，已在 `.gitignore`）
- 后端 / 前端 / proto 契约 / 导出与校对能力
- 任何形式的保存、项目、账号、云同步

## 6. 第三方与许可

| 组件 | 许可 | 是否入库 |
|---|---|---|
| **PaddleOCR-VL** 权重（`PaddlePaddle/PaddleOCR-VL-1.6-GGUF`） | Apache-2.0 | 否，用户自取 |
| **PP-DocLayoutV3**（PaddleX 模型） | Apache-2.0 | 否，用户自取 |
| **llama.cpp / libmtmd** | MIT | 否，脚本克隆 |
| **ONNX Runtime** | MIT | 否，脚本下载 |

本仓库代码的许可由仓库所有者决定（GitHub 公开仓库若未声明 `LICENSE`，默认保留所有权利）。
若计划长期公开维护，建议补一个 `LICENSE`（MIT 或 Apache-2.0）。
