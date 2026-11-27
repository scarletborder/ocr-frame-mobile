# impl/pc · 桌面端等效验证器（Windows / Ubuntu）

**目的：把「改一行 → 推 CI → 出 ipa → Sideloadly 装机 → 拍一页」换成「改一行 → 重编 → 跑一张图」。**

它编译的是**同一份 `impl/shard`**：版面预处理（拉伸 800×800 + `/255`）、坐标解码、25 类标签、
官方提示词、两阶段编排、`mtmd_helper_eval_chunks` 求值路径全部与 iOS / Android 一致；
差异只有「图片怎么解码」和「界面长什么样」。所以在这里跑出来的块、文本、日志，手机上应当一模一样。

---

## 1. 三步上手

```bash
cd impl/pc

# ① 依赖（ONNX Runtime + stb 头；真 VLM 再加 --with-llama）
bash scripts/bootstrap.sh                 # Windows Git Bash / Ubuntu 通用
# bash scripts/bootstrap.sh --with-llama  # 需要本地视觉模型时

# ② 构建（自动识别 MSVC / MinGW / GCC）
bash scripts/build.sh

# ③ 跑
./build/vlcore_pc --help
```

依赖落在 `third_party/`（已 gitignore）：`onnxruntime/{include,lib}`、`stb_image.h`、
`stb_image_write.h`、可选 `llama.cpp` + `llama-build`。CMake 会自动发现它们，
所以也可以直接 `cmake -B build -S . && cmake --build build --config Release`。

### 模型（与手机完全一致）

| 文件 | 从哪里来 |
|---|---|
| `PP-DocLayoutV3.onnx` | 官方 `PP-DocLayoutV3_infer` 静态图 → `bash scripts/export-layout-onnx.sh --model-dir <infer目录> --output models/PP-DocLayoutV3.onnx`（内部调用手机端同一个 `impl/ios/tools/export_layout_onnx.py`） |
| `PaddleOCR-VL-1.6-GGUF.gguf` + `-mmproj.gguf` | `huggingface-cli download PaddlePaddle/PaddleOCR-VL-1.6-GGUF --local-dir models/` |

放到 `impl/pc/models/`（已 gitignore），或者任何路径 —— 都用参数传。

---

## 2. 常用命令

```bash
# 只做版面（最快，用来和手机/后端对块）
./build/vlcore_pc --image page.png --layout models/PP-DocLayoutV3.onnx \
    --detect-only --overlay page.boxes.png --boxes page.boxes.csv

# 假 VLM：验证裁块尺寸、提示词选择、Markdown 合并（不需要视觉模型）
./build/vlcore_pc --image page.png --layout models/PP-DocLayoutV3.onnx --stub-vlm -v

# 完整两阶段（真 VLM）
./build/vlcore_pc --image page.png --layout models/PP-DocLayoutV3.onnx \
    --vlm models/PaddleOCR-VL-1.6-GGUF.gguf \
    --mmproj models/PaddleOCR-VL-1.6-GGUF-mmproj.gguf \
    --out page.md --json page.json

# 批量：一眼看完一批页面（每页输出 .md / .json / .boxes.csv / .boxes.png）
./build/vlcore_pc --dir pages/ --out-dir out/ --layout models/PP-DocLayoutV3.onnx \
    --vlm models/PaddleOCR-VL-1.6-GGUF.gguf --mmproj models/PaddleOCR-VL-1.6-GGUF-mmproj.gguf
```

| 参数 | 作用 |
|---|---|
| `--detect-only` | 只跑版面，不送 VLM（受 `--boxes/--overlay` 影响） |
| `--stub-vlm` | 假 VLM 替身，文本形如 `【stub】OCR: 454x108` |
| `--boxes <csv>` | 块表 `index,label,score,order,x1,y1,x2,y2,recognized`，可直接与手机结果 diff |
| `--overlay <png>` | 把框按类别上色画回原图，块序号用点阵数字标在框左上角 |
| `--dir / --out-dir` | 批量跑目录，每页四件套输出 |
| `-v / -q` | 打印整页 Markdown / 关掉核心日志 |

退出码：`0` 全部识别成功；`3` 有块未识别（仍会输出全部产物）；`1` 引擎或版面失败。

---

## 3. 怎么和手机对比

核心日志是**同一份代码**打的，所以三行就能对上：

```
[info ] 版面推理完成：用时 … s，300 条查询，最高分 0.84（cls=…，框=…，order=…，原图 1080x790）
[info ] 版面后处理：页面 1080x790，阈值 0.40，留下 N 块，用时 … s
[info ] 提示词 OCR:，图像 454x108，切分：文本 4 token + 图像 … token/… pos（n_batch=4096，n_ctx=8192）
```

- 手机上：`⋯ → 导出当前`，桌面上：直接看 stderr；
- **块数/坐标不一致** → 版面预处理或坐标还原跑偏（先看 `原图 WxH` 与 `留下 N 块`）；
- **块一致但文本不同** → 提示词、生成长度上限或 VLM 求值路径（`求值路径：mtmd_helper_eval_chunks`）不同；
- 文本被截断时先看 `生成 … token，用时 …，是否达到上限`。

---

## 4. 平台说明

| 平台 | 说明 |
|---|---|
| **Ubuntu** | `sudo apt install build-essential cmake` 后按 §1 走；VLM 走源码构建的 llama.cpp+libmtmd |
| **Windows + MSVC** | 推荐。在 “x64 Native Tools Command Prompt for VS” 里跑 `bash scripts/bootstrap.sh --with-llama` 与 `bash scripts/build.sh`（Git Bash 也从该环境启动，`cl` 才在 PATH 里） |
| **Windows + MinGW** | 版面/提示词/Markdown 完全可用；**本地 VLM 不要用**：MinGW 编出的 `libmtmd` 在 `mtmd_init_from_file` 会段错误（上游 `llama-mtmd-cli` 同样崩），`build-llama.sh` 默认拒绝，加 `--force-mingw` 才会继续 |

---

## 5. 排错

| 现象 | 处理 |
|---|---|
| `缺 third_party/stb_image.h` | `bash scripts/fetch-stb-image.sh` |
| 产物不含版面引擎 | `bash scripts/bootstrap.sh`；或手动 `-DORT_ROOT=<含 include/lib 的目录>` |
| Windows 启动报找不到 `onnxruntime.dll` | 构建时会把 DLL 拷到 exe 旁边；手动构建时自己拷，或把 `third_party/onnxruntime/lib` 加进 PATH |
| 产物不含本地 VLM | `bash scripts/bootstrap.sh --with-llama`（Windows 需 MSVC） |
| 版面块整体错位/整页一块 | 先确认用的是按官方约定导出的 ONNX（拉伸 + `/255`），见 `impl/ios/tools/verify_layout_onnx.py` |
| 提示 `求值路径：内置 EvalChunks` | llama.cpp 太旧/没带 `mtmd-helper.h`，M-RoPE 模型会每块失败 → 重跑 `build-llama.sh` |
