# iOS 安装与测试 Runbook（Windows + Sideloadly 主路线）

签名与安装都在 **Windows** 上完成（Sideloadly + iTunes），所以只需要拿到一个
**未签名 `.ipa`**。构建必须发生在 macOS 上，因此有两条产出 ipa 的路子：

| 路线 | 需要什么 | 适用 |
|---|---|---|
| **A. GitHub Actions（推荐）** | 一个 GitHub 仓库（公开仓库免费；私有仓库 macOS 分钟按 10 倍计费） | 手边没有 Mac |
| **B. 借一台 Mac** | 任意 macOS + Xcode 命令行工具，不需要证书、不需要登录开发者账号 | 有 Mac / 同事有 Mac |

两条路线最终产物都是 `VLOCR-unsigned.ipa`，之后流程完全一样（见 §3）。

---

## 1. 路线 A：用 CI 产出未签名 ipa

工作流已就位：`.github/workflows/ios-ipa.yml`。

1. 把仓库推到 GitHub（或 fork 后推）。
2. Actions → **iOS IPA (unsigned)** → `Run workflow`：
   - `with_llama = true`：产物包含设备内 VLM（llama.cpp），**首次 10–20 分钟**，之后命中缓存；
   - `with_llama = false`：只含版面引擎，产物小、构建快，只能配「远程服务」模式（§4）。
3. 跑完在 run 页面底部下载 artifact **VLOCR-unsigned-ipa**，解压得到 `VLOCR-unsigned.ipa`。

工作流做的事（也可在本地 Mac 复现）：

```bash
bash impl/ios/scripts/fetch-onnxruntime.sh            # 版面引擎
bash impl/ios/scripts/build-llama-xcframework.sh      # 本地 VLM（LLAMA_PLATFORMS=ios-device）
cd impl/ios && xcodegen generate                      # 生成工程
bash impl/ios/scripts/package-ipa.sh                  # xcodebuild archive（不签名）+ 打 ipa
```

> 想强制重建 llama 引擎（例如上游 `mtmd` API 变了）：把工作流里的
> `LLAMA_CACHE_KEY` 改个后缀再跑；换 ONNX Runtime 来源/形态时改 `ORT_CACHE_KEY`。

## 2. 路线 B：借一台 Mac 出 ipa

```bash
# 一次性
brew install xcodegen cmake

cd impl/ios
bash scripts/fetch-onnxruntime.sh
bash scripts/build-llama-xcframework.sh
bash scripts/bootstrap.sh          # 生成 VLOCR.xcodeproj
bash scripts/package-ipa.sh        # → build/VLOCR-unsigned.ipa
```

全程不需要 Apple ID、不需要证书、不需要 `DEVELOPMENT_TEAM`（脚本显式传
`CODE_SIGNING_ALLOWED=NO`）。把 `build/VLOCR-unsigned.ipa` 拷回 Windows 即可。

> `fetch-onnxruntime.sh` 会按顺序试三个来源：自定义 URL → 官方 pod-archive →
> GitHub 上的 1.22.0 镜像（钉 SHA256）。官方域名 `download.onnxruntime.ai` 在
> GitHub Actions 上返回的证书与域名不匹配，必然落到镜像；脚本会打印实际来源与包形态
> （`static-framework` 或 `dynamic`）。内网/离线兜底：
> `ONNXRUNTIME_XCFRAMEWORK_URL=... ONNXRUNTIME_XCFRAMEWORK_SHA256=... bash scripts/fetch-onnxruntime.sh`。

> 顺带说明：如果这台 Mac 就想直接装机，也可以填 `project.yml` 的 `DEVELOPMENT_TEAM`
> 后用 Xcode `Cmd+R`，与本路线不冲突。

## 3. Windows：Sideloadly 签名 + iTunes 安装

1. 安装 [Sideloadly](https://sideloadly.io/) 与 iTunes（Windows 版；Sideloadly 需要它的
   设备驱动与 Apple 服务组件）。
2. iPhone 用数据线连电脑，手机上点「信任此电脑」。
3. 手机开启 **设置 → 隐私与安全性 → 开发者模式**（iOS 16+ 必须），按提示重启。
4. Sideloadly：
   - `IPA` 选 `VLOCR-unsigned.ipa`；
   - `Apple ID` 填你的账号（免费账号即可）；
   - 点 **Start**，输入密码（如开了双重认证，用 App 专用密码）；
   - 日志出现 `Done.` 即安装完成。
5. 手机首次打开会提示不受信任：**设置 → 通用 → VPN 与设备管理 → 开发者 App → 信任**。

免费 Apple ID 的限制（会直接影响你）：

| 限制 | 影响 | 应对 |
|---|---|---|
| 证书 **7 天**过期 | 到期后 App 打不开 | 重新用 Sideloadly 装一次（覆盖安装，`Documents` 里的模型通常保留，不用重传） |
| 同时最多 3 个自签 App | 超了要先删 | — |
| 每周 10 个 App ID | 反复换 bundle id 会耗尽 | 尽量固定 `ai.ocrframe.vlocr`；被占用时再改 |

> 模型**不**打进 ipa（ipa 只有几十 MB），所以签名/安装很快；模型单独传（§4）。

## 4. 把模型放进手机（Windows / iTunes）

App 已开启 `UIFileSharingEnabled`，iTunes 里能看到它的 Documents。

1. **先启动一次 App**（否则 iTunes 里不显示该 App）。
2. iTunes → 设备 → **文件共享** → 选中「VL 识别」→ 把三个文件拖进右侧列表：
   - `PP-DocLayoutV3.onnx`
   - `PaddleOCR-VL-1.6-GGUF.gguf`
   - `PaddleOCR-VL-1.6-GGUF-mmproj.gguf`
3. 回到 App：iTunes 只能把文件放进 Documents **根目录**，App 启动时/点设置页
   「重新扫描 Documents 目录」会自动把 `.onnx` / `.gguf` 收进 `Models/` 并填好槽位。

备用路径（不用 iTunes）：把文件 AirDrop / 存到「文件」App，然后在设置页用「文件」选择器
逐个选，App 会复制进 `Models/`。

模型准备（在任意机器上做一次，产物拷到手机）：

- 版面：`PP-DocLayoutV3_infer.tar` → `python3 impl/ios/tools/export_layout_onnx.py` →
  `PP-DocLayoutV3.onnx`；上机前用 `impl/ios/tools/verify_layout_onnx.py` 核对 I/O 与坐标。
- VLM：`huggingface-cli download PaddlePaddle/PaddleOCR-VL-1.6-GGUF --local-dir ~/models/vlm`。

## 5. 冒烟验收

| # | 检查点 | 期望 |
|---|---|---|
| 1 | 设置页「预设」 | `VL 线通用文档解析 1.0` · 产线 `v1.6` · `2048 tokens` |
| 2 | 设置页「引擎」 | 版面分析引擎 / VLM 引擎都「已链接」（`with_llama=false` 时 VLM 显示未链接，属正常） |
| 3 | 设置页「模型」 | 三行都显示文件名，没有「未选择」 |
| 4 | 识别页顶部 | 「模型已就绪」 |
| 5 | 拍一页 | 进度从「正在分析版面…」→「已处理 k/n 个版面块」 |
| 6 | 结果页 Markdown | 标题带 `#`、正文分段、表格/公式保留原文 |
| 7 | 结果页「版面块」 | `label` 是 `doc_title`/`text`/`table` 等，`order` 从 1 连续 |
| 8 | 耗时 | 单页几十秒量级（0.9B 端侧模型，块越多越慢） |

抓日志（Windows）：iTunes 不行，用 `Console`（需 Mac）或装 `3uTools` 看崩溃日志；
更简单的是先在 App 里看错误文案——UI 已把引擎错误原样显示在识别页红框里。

## 6. 快速路径：只验链路，不带本地 VLM

CI 里把 `with_llama` 关掉（或 Mac 上 `bash scripts/bootstrap.sh --no-llama`），
装好后在设置页把「运行方式」切到**远程服务**，地址填 `http://<电脑IP>:8118/v1`：

```bash
llama-server -m PaddleOCR-VL-1.6-GGUF.gguf --mmproj PaddleOCR-VL-1.6-GGUF-mmproj.gguf \
  --host 0.0.0.0 --port 8118
```

这条路径能验证：版面模型、裁块、提示词、Markdown 合并、UI 全链路，
只剩「设备内 llama.cpp」没验。链路绿了再回去编译 xcframework。

> 远程模式走明文 HTTP，Info.plist 已开 `NSAllowsArbitraryLoads`（仅内网调试用）。

## 7. 排错

| 现象 | 原因 / 处理 |
|---|---|
| Actions 里 llama 步骤超时/失败 | 先看日志尾部；实在不行关掉 `with_llama` 走远程模式，别卡在编译上 |
| `package-ipa.sh` 报 `缺少 VLOCR.xcodeproj` | 先 `xcodegen generate`（或 `bootstrap.sh`） |
| 报 `Embed 未生效：.app 里没有 llama.framework` | 工程生成时 llama 已存在但 embed 没生效；删 `VLOCR.xcodeproj` 重新 `xcodegen generate` |
| Sideloadly 报 `Unable to sign` / `Invalid IPA` | ipa 不是 arm64 真机包，或没解压对；确认 `Payload/VLOCR.app/VLOCR` 的 `lipo -archs` 是 `arm64` |
| Sideloadly 装完点图标闪退 | ①没信任证书 ②没开开发者模式 ③框架签名失败（换 `with_llama=false` 验证是不是 llama.framework 引起） |
| App 打开是空白/立刻退出 | 同上，多半是签名不完整；重新用 Sideloadly 装一次 |
| 设置页显示「引擎 未链接」 | 该 ipa 是不含对应引擎的构建（`with_llama=false`），不是 bug |
| 设置页三行都「未选择」 | 模型没传进 App；iTunes 传完后点「重新扫描 Documents 目录」 |
| 识别报「视觉语言模型加载失败」 | GGUF 架构 `paddleocr` 需要较新的 llama.cpp；改 `LLAMA_CACHE_KEY` 重建 |
| 识别报「版面模型输入名不是 PaddleX 导出形态」 | ONNX 不是 `image/im_shape/scale_factor` 三输入，重新导出 |
| 版面框整体错位 | 用 `tools/verify_layout_onnx.py` 在桌面复核；桌面也错位才是模型/导出问题 |
| 7 天后打不开 | 免费证书过期，重新签名安装 |

## 8. 首次真机最可能先炸的地方

1. **Swift/ObjC++ 桥接编译错误**（`impl/shard` 是新 C++ 代码，尚未在真机编译过）。
   先用 `with_llama=false` 出一版小 ipa，能把「桥接 + 版面」单独验证。
2. **llama.cpp `mtmd` 签名漂移**：只影响 `impl/shard/src/engines/mtmd_vlm.cpp`。
3. **ORT 头文件探测失败**：不会编译报错，而是设置页「引擎未链接」——
   所以设置页那两行是第一个要看的信号。
