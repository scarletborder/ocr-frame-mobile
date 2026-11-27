# impl/shard · iOS / Android 共用的 VL 线核心

两个移动端 demo 共用同一份 C++17 源码，**VL 线规则只在这里定义一次**：

| 共享内容 | 平台侧还剩什么 |
|---|---|
| 25 类版面标签字典、跳过排序 / 忽略 Markdown / 纯插图集合 | 图片解码（CGImage / Bitmap → RGB） |
| 官方提示词（`OCR:` / `Table Recognition:` / `Formula Recognition:` …） | 推理库的链接方式（xcframework / CMake） |
| letterbox + 归一化、裁块、像素上限 | UI |
| 检测后处理：反算坐标 → 阈值 → NMS → 整页伪框 → 阅读顺序编号 | 模型文件选择与持久化 |
| 两阶段编排（版面 → 逐块 VLM → Markdown） | 远程服务的 HTTP 实现 |
| 两个推理引擎（ONNX Runtime / llama.cpp+mtmd） | — |

## 1. 目录

```
impl/shard/
├── include/vlcore/
│   ├── core.hpp        纯逻辑接口：标签规则、图像、后处理、Markdown
│   ├── engines.hpp     两个推理引擎的抽象 + 工厂 + 是否编译进构建
│   ├── pipeline.hpp    两阶段编排
│   └── vlcore_c.h      C 门面（iOS 用 ObjC++、Android 用 JNI 调用它）
├── src/
│   ├── core.cpp        标签表、letterbox/裁块/缩放、NMS+阅读顺序、Markdown、JSON
│   ├── pipeline.cpp    编排：单块失败不拖垮整页
│   ├── vlcore_c.cpp    C 门面实现（JSON 结果 + malloc 字符串约定）
│   └── engines/
│       ├── ort_layout.cpp  PP-DocLayoutV3（ONNX Runtime C API）
│       └── mtmd_vlm.cpp    PaddleOCR-VL（llama.cpp + libmtmd）
└── tests/test_core.cpp 桌面单测（无模型即可跑）
```

引擎是否参与编译由 `__has_include` 自动探测（`VLCORE_WITH_ORT` / `VLCORE_WITHOUT_ORT` 等宏可强制覆盖）：
头文件存在才编译真实推理，否则工厂返回 `nullptr` 并给出可读错误——这样两端在没拉依赖时也能编译通过。

## 2. 编译

```bash
# 桌面：只编译纯逻辑 + 单测（不依赖任何模型/推理库）
cmake -S impl/shard -B build-shard
cmake --build build-shard
ctest --test-dir build-shard --output-on-failure
# 或直接：g++ -std=c++17 -I impl/shard/include impl/shard/src/*.cpp \
#          impl/shard/src/engines/*.cpp impl/shard/tests/test_core.cpp -o t && ./t
```

| 平台 | 接入方式 |
|---|---|
| iOS | `impl/ios/project.yml` 直接把 `../shard/src` 编进 App，`HEADER_SEARCH_PATHS` 指向 `../shard/include`；引擎由两个 xcframework 提供 |
| Android | `app/src/main/cpp/CMakeLists.txt` 用 `add_subdirectory(${SHARD_DIR})`，引擎由 ONNX Runtime AAR + llama.cpp 源码提供 |

## 3. C 门面（`vlcore_c.h`）

```c
vlcore_engine_t *vlcore_create(layout_onnx, vlm_gguf, mmproj_gguf, n_ctx, threads, &err);
char *vlcore_run(engine, rgb, w, h, progress, user, &err);   // 整页 JSON
char *vlcore_layout_json(engine, rgb, w, h, &err);           // 只做版面
char *vlcore_recognize_block(engine, rgb, w, h, label, &err); // 单块
char *vlcore_prompt_for_label(label);
void  vlcore_set_remote(engine, fn, user);                   // 远程识别回调
void  vlcore_set_log(fn, user);                              // 全局日志 sink（NULL 卸载）
void  vlcore_log(level, message);                            // 平台侧补一行日志
int   vlcore_layout_engine_built(void);
```

日志约定：`level` 0/1/2/3 = 调试/信息/警告/错误（与 `vlcore::LogLevel` 同值）。装了 sink 之后，
核心编排（版面后处理、逐块耗时与失败原因）与 llama.cpp / ggml 自己的输出会一起走这条通道；
**没装 sink 时是空实现**，不产生额外开销。iOS 把它接到 App 内「日志」页，Android 可接 logcat。

约定：返回的 `char*` 由调用方 `vlcore_free_string`；`error` 也是 malloc 的 UTF-8 文本；
输入图像是紧排 RGB（`width*height*3`）。整页结果：

```json
{"markdown":"…","blocks":[{"label":"text","order":1,"score":0.91,
  "bbox":[100,50,700,120],"text":"…","recognized":true}],"seconds":12.3}
```

## 4. 与后端 Python 的同源关系

| 本仓库 | 后端 |
|---|---|
| `core.cpp::kLabels` / `IsSkipOrder` / `RegionType` | `impl/ocr_core/vl/blocks.py::REGION_TYPES` / `SKIP_ORDER_LABELS` |
| `core.cpp::PromptFor` | `paddlex/inference/pipelines/paddleocr_vl/pipeline.py` 的 prompt 选择 |
| `core.cpp::RenderMarkdown` | `impl/ocr_core/vl/markdown.py` + `markdown_ignore_labels` |
| `core.hpp::Preset` | `impl/proto/preset.proto::VlConfig` + `ocr_core/vl/options.py` |
| `pipeline.cpp` | `ocr_core/vl/pipeline.py` + `adapter.py` |

改这些规则时**必须同时改 Python 侧**，并跑 `impl/shard` 的单测与后端 `impl/ocr_core/tests/test_vl.py`。
