# impl/pc · 桌面最小示例（同一份 VL 线核心）

手机上只能看到「未识别」四个字，这里能看到**为什么**：每块的标签、阅读顺序、裁块尺寸、
是否送模型、失败原因、以及合并后的 Markdown。

它与 iOS / Android 编译的是**同一份 `impl/shard`**，所以在这里复现的问题在手机上一模一样。

## 1. 能做什么

| 模式 | 命令 | 验证什么 |
|---|---|---|
| 只版面 | `--layout model.onnx` | 版面检测、后处理、阅读顺序（不需要 VLM） |
| 假 VLM | `--layout … --stub-vlm` | 裁块、提示词选择、Markdown 合并（不需要模型） |
| 真 VLM | `--layout … --vlm …gguf --mmproj …gguf` | 完整两阶段 |

输出示例（`--stub-vlm`）：

```
  [ 0] text             已识别  «【stub】OCR: 318x63»
  [ 7] table            已识别  «【stub】Table Recognition: 720x177»
==> 版面块 8 个，未识别 0 个
```

输出示例（缺 VLM 时，正是手机上「Markdown 全是 ![](block-N)」的现场）：

```
  [ 0] text             未识别  ← 尚未加载视觉语言模型，也没有配置远程识别服务。
==> 版面块 8 个，未识别 8 个
```

> 修复后：**只有真正的插图类**才会在 Markdown 里输出 `![](block-N)`；
> 文本块识别失败时不再产出占位图（之前会把「文字没识别出来」误报成「这里是一张图」）。

## 2. 依赖

| 组件 | 取法 |
|---|---|
| ONNX Runtime（版面） | 任意来源的 `onnxruntime_c_api.h` + 动态库（`onnxruntime.dll` / `libonnxruntime.so` / `.dylib`） |
| llama.cpp（VLM，可选） | 源码 + 构建产物（`libllama` / `libmtmd` / `libggml*`） |
| stb_image.h | `bash scripts/fetch-stb-image.sh`（不入库） |

模型与手机完全一致：`PP-DocLayoutV3.onnx` + `PaddleOCR-VL-1.6-GGUF.gguf` + `-mmproj.gguf`。

## 3. 构建

### macOS / Linux

```bash
# llama.cpp（可选，只要 VLM 才需要）
git clone --depth 1 https://github.com/ggml-org/llama.cpp ~/llama.cpp
cmake -B ~/llama-build -S ~/llama.cpp -DCMAKE_BUILD_TYPE=Release \
      -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF \
      -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_MTMD=ON -DGGML_OPENMP=OFF
cmake --build ~/llama-build --target llama mtmd -j

# 本体
bash scripts/fetch-stb-image.sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
      -DORT_ROOT=/path/to/ort \
      -DLLAMA_DIR=$HOME/llama.cpp -DLLAMA_LIB_DIR=$HOME/llama-build
cmake --build build -j
```

### Windows（MinGW）

同上，生成器换成 `-G "MinGW Makefiles"`。已知问题见 §5。

## 4. 跑

```bash
./build/vlcore_pc --image page.png --layout PP-DocLayoutV3.onnx --stub-vlm -v
./build/vlcore_pc --image page.png --layout PP-DocLayoutV3.onnx \
    --vlm PaddleOCR-VL-1.6-GGUF.gguf --mmproj PaddleOCR-VL-1.6-GGUF-mmproj.gguf \
    --out page.md --json page.json
```

退出码：`0` 全部识别成功；`3` 有块失败（仍会输出 Markdown 与 JSON）；`1` 引擎/版面失败。

## 5. 已知问题（Windows + MinGW）

用 MinGW 构建的 llama.cpp 在本仓库的验证环境里，`mtmd_init_from_file`（视觉投影器初始化）
会在打印 `--- vision hparams ---` 之后段错误；官方 `llama-mtmd-cli` 同样异常，
因此判断为 **MinGW + 该视觉塔的上游问题**，与本仓库代码无关：

- Windows 上建议：`--stub-vlm`（验证版面/提示词/Markdown）或改用 MSVC 工具链；
- 完整 VLM 链路请在 macOS / Linux 上跑（同一份源码，行为一致）。

版面（ONNX Runtime）在 Windows 上正常工作。
