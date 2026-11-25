# impl/ios · VL 线最小部署示例（iPhone）

在 iPhone 上把 **PaddleOCR-VL 完整两阶段产线**（版面分析 + 视觉语言模型）跑通的最小 demo：

- 底部导航只有两个 tab：**识别**（大相机按钮 / 相册 / 文件）、**设置**（选两个模型）；
- 预设**写死**为当前 VL 线做法（`vl_general@1.0`，产线 `v1.6`）；
- 一次选一张或多张图 → 逐页识别 → 展示整页 Markdown 与逐块明细；
- **不做**保存、项目、校对、导出。

> **VL 线规则不在本目录**：标签字典、提示词、版面后处理、阅读顺序、Markdown 合并、
> 两阶段编排与两个推理引擎都在 `impl/shard`，与 `impl/android` 共用同一份 C++ 源码。
> 本目录只有：图片解码、引擎链接、模型文件管理、SwiftUI 界面。

## 1. 技术选型

| 环节 | 选型 | 理由 |
|---|---|---|
| 版面分析 | PP-DocLayoutV3 → ONNX → **ONNX Runtime C API** | 官方只有 Paddle 静态图与 HF safetensors，iOS 无 Paddle；paddle2onnx 对 V3 误差约 1.57% |
| VLM 识别 | **PaddleOCR-VL-1.6 GGUF → llama.cpp + libmtmd** | 官方提供 GGUF；llama.cpp 的 `build-xcframework.sh` 默认把 `libmtmd` 合进 framework |
| 共享核心 | **impl/shard（C++17）** | 与 Android 同一份实现，规则不会两端漂移 |
| 备选 | OpenAI 兼容远程服务 | 未构建 llama.xcframework 时验证整条链路（共享核心的远程回调） |

> 为什么不是 `transformers`：iOS 没有 Python / transformers 运行时。官方对 0.9B 组件给出的端侧
> 等价路径就是 GGUF（llama.cpp）与 MLX-VLM，且明确「必须跑完整两阶段」。

## 2. 目录结构

```
impl/ios/
├── project.yml                      XcodeGen 工程定义（含 ../shard/src 源码）
├── scripts/
│   ├── bootstrap.sh                 一键：拉依赖 + 生成 xcodeproj
│   ├── fetch-onnxruntime.sh         下载 onnxruntime.xcframework（C API）
│   ├── build-llama-xcframework.sh   构建 llama.xcframework（含 libmtmd）
│   └── package-ipa.sh               打未签名 ipa（给 Windows 侧 Sideloadly 签名）
├── tools/
│   ├── export_layout_onnx.py        PP-DocLayoutV3 静态图 → ONNX
│   └── verify_layout_onnx.py        桌面端复跑同一套预处理/后处理，核对坐标
└── VLOCR/
    ├── App/                         App 入口与 TabView
    ├── Bridge/
    │   ├── VLOCREngine.h/.mm        平台胶水：CGImage → RGB、远程 HTTP 回调
    │   └── VLOCRBridging.h          Swift 桥接头
    ├── Core/
    │   ├── Models.swift             展示用值对象 + 预设信息（来自共享核心）
    │   ├── VLPipeline.swift         调引擎 + 解析 JSON
    │   ├── ModelStore.swift         两个模型槽位 + 引擎缓存
    │   └── ImageTools.swift         方向归一化 / 缩略图
    └── Features/
        ├── Recognize/               相机、相册、文件、结果页
        └── Settings/                两个模型选择 + 运行方式
```

## 3. 环境要求

- macOS + Xcode 16 或更新（`iOS 17.0+`，真机 iPhone 17 验证）
- `brew install xcodegen cmake`
- 磁盘：llama.cpp 构建产物 + 两个模型约 2.5 GB

## 4. 构建与安装（两条路线）

签名/安装可以在 **Windows（Sideloadly + iTunes）** 完成，因此只需要一个**未签名 `.ipa`**：

```bash
# ① 依赖（首次构建 llama.cpp 约 10–20 分钟）
bash scripts/fetch-onnxruntime.sh
bash scripts/build-llama-xcframework.sh

# ② 生成工程 + 打未签名 ipa
bash scripts/bootstrap.sh
bash scripts/package-ipa.sh          # → build/VLOCR-unsigned.ipa
```

- **没有 Mac**：`.github/workflows/ios-ipa.yml` 会在 macOS runner 上产出同一个 ipa，
  下载后交给 Sideloadly。
- **有 Mac 且想直装**：`open VLOCR.xcodeproj` → 填 `project.yml` 的 `DEVELOPMENT_TEAM` → `Cmd+R`。

完整步骤（Sideloadly 签名、7 天证书、iTunes 传模型、验收清单、排错）见
[`DEVICE-TEST.md`](./DEVICE-TEST.md)。

## 5. 模型准备

### 5.1 版面分析（`.onnx`）

```bash
# 取 PaddleX 官方静态图推理模型（与 impl/backend/app/services/models_manifest.yaml 同一路径规则；
# 具体文件名以 PaddleX 模型列表为准）
curl -L -o PP-DocLayoutV3_infer.tar \
  https://paddle-model-ecology.bj.bcebos.com/paddlex/official_inference_model/paddle3.0.0/PP-DocLayoutV3_infer.tar
tar -xf PP-DocLayoutV3_infer.tar

pip install paddle2onnx onnx onnxruntime pillow
python tools/export_layout_onnx.py --model-dir ~/models/PP-DocLayoutV3_infer \
    --output ~/models/PP-DocLayoutV3.onnx

# 校验：打印 [N,7] 检测结果与阅读顺序（与 App 内预处理完全同源）
python tools/verify_layout_onnx.py --model ~/models/PP-DocLayoutV3.onnx --image page.png
```

要求（`impl/shard/src/engines/ort_layout.cpp` 注释同源）：

| 项 | 约定 |
|---|---|
| 输入 | `image [1,3,800,800]` float32（RGB、letterbox、右下补 114）、`im_shape [1,2]=[h,w]`、`scale_factor [1,2]=[1/r,1/r]` |
| 归一化 | `mean=[0.8286,0.8281,0.8282]`、`std=[0.1889,0.1889,0.1889]` |
| 输出 | `[300,7]` = `[class_id, score, x1, y1, x2, y2, order]`，坐标在 letterbox 空间 |

### 5.2 视觉语言模型（`.gguf` + `-mmproj.gguf`）

```bash
pip install "huggingface_hub[cli]"
huggingface-cli download PaddlePaddle/PaddleOCR-VL-1.6-GGUF \
    --local-dir ~/models/PaddleOCR-VL-1.6-GGUF
#   PaddleOCR-VL-1.6-GGUF.gguf          主权重
#   PaddleOCR-VL-1.6-GGUF-mmproj.gguf   视觉投影器（必须成对）
```

### 5.3 拷进手机

App 已开启 `UIFileSharingEnabled`：

- **Finder**：设备 → 文件共享 → 「VL 识别」→ 把两个模型拖进 `Models/`；
- **文件 App**：AirDrop 到「我的 iPhone」，再在 App 设置里逐个选择（会复制进 `Documents/Models/`）。

设置页两行分别选 **版面分析模型**（`.onnx`）与 **视觉语言识别模型**（`.gguf`），
选完主权重后会自动在同目录找 `*mmproj*.gguf`，找不到时用第三行「视觉投影器」手动选。

## 6. 与后端的对应关系

规则本体在 `impl/shard`，对照表见 `impl/shard/README.md` §4。
iOS 侧只剩三件事：`Bridge/VLOCREngine.mm`（解码 + HTTP）、`Core/VLPipeline.swift`（JSON → 模型）、UI。

## 7. 已知限制（demo 边界）

1. **表格**输出为 OTSL 原文，未做官方 `convert_otsl_to_html`；公式保留 `\(...\)` 原文。
2. 未启用方向分类 / 去弯曲 / 图表 / 印章识别（与预设默认一致）。
3. 逐块串行推理，无 batch、无三线程流水线；0.9B 模型在 A19 上的单页耗时需实测。
4. 版面模型跑 ONNX Runtime CPU EP；`coreml_provider_factory.h` 存在时可自行加 CoreML EP。
5. 结果不落库、不做项目与校对，关闭页面即丢；整页超过 1200 万像素会先降采样。
6. `llama.xcframework` / `onnxruntime.xcframework` 不入库，由 `scripts/` 生成。
7. 设置页会显示模型**文件名**。这是管理员「主动选择并替换模型」的场景，与 AGENTS §6.4 模型管理例外同性质。

## 8. 排错

| 现象 | 处理 |
|---|---|
| 设置页「版面分析引擎 未链接」 | 没跑 `scripts/fetch-onnxruntime.sh`，或 `Frameworks/onnxruntime.xcframework` 没进工程 |
| 「VLM 引擎 未链接」 | 没构建 `llama.xcframework`；可先用「远程服务」模式跑通链路 |
| 版面分析报「输入名不是 PaddleX 导出形态」 | 导出的 ONNX 不是 `image/im_shape/scale_factor` 三输入，回到 5.1 重新导出 |
| 版面框整体错位 | 检查 `scale_factor` 是否传 `1/r`；用 `tools/verify_layout_onnx.py` 对照 |
| 识别结果大段重复 | 模型不吐 EOS，属已知行为；可缩短该块生成长度或跳过 |
| 模型加载 OOM | 关闭其他 App；0.9B 权重 + 800×800 版面模型在 8 GB 设备上余量不大 |
