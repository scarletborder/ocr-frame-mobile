# impl/android · VL 线最小部署示例（Android）

与 `impl/ios` 功能等价，且**共用同一份 VL 线核心** `impl/shard`：

- 底部导航只有两个 tab：**识别**（大相机按钮 / 相册多选 / 文件）、**设置**（选两个模型）；
- 预设写死为当前 VL 线做法（`vl_general@1.0`、产线 `v1.6`、`max_new_tokens=2048`）；
- 一次选一张或多张图 → 逐页识别 → 展示整页 Markdown 与逐块明细；
- 不做保存、项目、校对、导出。

## 1. 技术栈

| 环节 | 选型 |
|---|---|
| 版面分析 | PP-DocLayoutV3 → ONNX → **ONNX Runtime C API**（`onnxruntime-android` AAR 里的 headers + `libonnxruntime.so`） |
| VLM | PaddleOCR-VL-1.6 **GGUF** → **llama.cpp + libmtmd**（NDK 直接编译源码） |
| 共享逻辑 | `impl/shard`（C++17，通过 JNI 暴露 `runJSON`） |
| UI | Jetpack Compose（Material 3） |
| 相机 / 相册 / 文件 | `ActivityResultContracts.TakePicture` / `PickMultipleVisualMedia` / `OpenDocument` |

## 2. 目录

```
impl/android/
├── settings.gradle.kts / build.gradle.kts / gradle.properties
├── scripts/
│   ├── fetch-onnxruntime-android.sh   解出 ORT 头文件与 arm64 的 .so
│   └── build-llama-android.sh         克隆 llama.cpp 源码（由 NDK 编译）
└── app/
    ├── build.gradle.kts
    └── src/main/
        ├── AndroidManifest.xml
        ├── cpp/
        │   ├── CMakeLists.txt         add_subdirectory(impl/shard) + 两个引擎
        │   └── vlcore_jni.cpp         JNI 胶水（进度回调 / 远程回调）
        ├── res/                       主题、FileProvider 路径
        └── java/ai/ocrframe/vlocr/
            ├── MainActivity.kt
            ├── VlEngine.kt            JNI 门面
            ├── BitmapTools.kt         解码 / EXIF 旋转 / 像素上限 / RGB
            ├── ModelStore.kt          两个模型槽位（复制进 files/models）
            ├── EngineHost.kt          引擎缓存 + 远程回调注册
            ├── RemoteClient.kt        OpenAI 兼容 /v1/chat/completions
            ├── PageResult.kt          结果模型 + JSON 解析
            ├── RecognizeViewModel.kt
            └── ui/                    AppRoot / Recognize / Settings / PageDetail
```

## 3. 环境

- JDK 17、Gradle 8.9+、Android SDK `platform 35`、NDK 27.x（CI 使用 runner 预装版本）、CMake 3.22+（CI 使用 3.31.5）
- 真机 arm64（0.9B 权重 + ONNX Runtime 只在真机上有意义），minSdk 26

## 4. 构建

### 4.1 CI（推荐，不需要本机装 Android 工具链）

`.github/workflows/android-apk.yml`，push 到 `main`/`master` 或手动 `Run workflow`：

1. `with_llama = true`（默认）：编本地 VLM，首次 10–20 分钟，之后命中缓存；
2. 下载 artifact **VLOCR-apk** → `app-release.apk`。

```bash
adb install -r app-release.apk
# 或直接把 apk 传到手机点安装（需允许「未知来源」）
```

### 4.2 本机

```bash
cd impl/android

# ① 依赖
bash scripts/fetch-onnxruntime-android.sh   # 头文件 + so（同时放进 jniLibs 供打包）
bash scripts/build-llama-android.sh         # 拉 llama.cpp 源码（首次 Gradle 构建时编译）

# ② 构建
gradle assembleRelease      # ← 发布用这个：native -O3，0.9B 推理才跑得动
adb install -r app/build/outputs/apk/release/app-release.apk
```

关于签名的两点：

- `release` 变体用 **debug 证书**签名（`signingConfigs.getByName("debug")`），
  这样公开仓库里不需要任何 keystore/密钥，产物可以直接 sideload；正式分发再换自己的 keystore。
- 不要用 `assembleDebug` 做真机测试：debug 变体 native 是 `-O0`，0.9B 模型会慢到不可用。

> 仓库里没有 `gradle-wrapper.jar`（二进制不入库）：Android Studio 打开工程会自动补齐，
> 或本机装 Gradle 8.9+ 后执行 `gradle wrapper` 生成；CI 直接由 `gradle/actions/setup-gradle` 提供。

只跑链路、先不编译 llama.cpp：跳过 `build-llama-android.sh`，CMake 会警告「不含本地 VLM」，
App 里改用「远程服务」模式即可（设置页可切换）。

### 4.3 一个容易踩的坑：ONNX Runtime 的 .so 必须进 APK

`libonnxruntime.so` 是作为 imported shared library 链接的，AGP **不会**自动打包它。
`fetch-onnxruntime-android.sh` 会把它额外复制到 `app/src/main/jniLibs/arm64-v8a/`，
否则运行时报 `dlopen failed: library "libonnxruntime.so" not found`。

## 5. 模型准备

与 iOS 完全一致，见 `impl/ios/README.md` §5：

- 版面：PP-DocLayoutV3 静态图 → `paddle2onnx` → `.onnx`（用 `impl/ios/tools/export_layout_onnx.py`，
  并用 `verify_layout_onnx.py` 核对 I/O 与坐标）；
- VLM：`PaddlePaddle/PaddleOCR-VL-1.6-GGUF` 的 `*.gguf` 与 `*-mmproj.gguf`。

导入方式二选一：

```bash
# A. adb 直接推进 App 私有目录
adb push PP-DocLayoutV3.onnx /data/local/tmp/
adb push PaddleOCR-VL-1.6-GGUF.gguf /data/local/tmp/
adb push PaddleOCR-VL-1.6-GGUF-mmproj.gguf /data/local/tmp/
# 然后在设置页用「文件」选择器选中（选择器能看到 /data/local/tmp）

# B. 先把文件拷到手机「下载」目录，再在设置页选择
```

选择器会把文件复制进 `files/models/`（App 私有目录），只持久化文件名。

## 6. 与 iOS 的差异

| 项 | iOS | Android |
|---|---|---|
| 共享核心 | 直接编进 Xcode target | `add_subdirectory` 编进 `libvlocr.so` |
| 引擎绑定 | ObjC++ 调 C 门面 | JNI 调同一套 C 门面 |
| 图片解码 | CoreGraphics | `BitmapFactory` + `ExifInterface` |
| 远程识别回调 | `NSURLSession`（同步） | `HttpURLConnection`（同步） |
| 文件持久化 | Documents/Models + 文件共享 | `files/models` + SAF 复制 |

其余（预设、提示词、后处理、Markdown、UI 结构）两端一致，规则改动只需改 `impl/shard`。

## 7. 已知限制

1. 表格输出为 OTSL 原文，未做 `convert_otsl_to_html`；公式保留 `\(...\)` 原文。
2. 未启用方向分类 / 去弯曲 / 图表 / 印章识别（与预设默认一致）。
3. 逐块串行推理，无 batch；0.9B 模型在真机上的单页耗时需实测。
4. 只编译 `arm64-v8a`。
5. 结果不落库，退出即丢。

## 8. 排错

| 现象 | 处理 |
|---|---|
| 设置页「版面分析引擎 未编译」 | 没跑 `scripts/fetch-onnxruntime-android.sh`，或头文件没解出来 |
| 「VLM 引擎 未编译」 | 没跑 `scripts/build-llama-android.sh`（或 `cpp/llama.cpp` 不存在）；可先用远程模式 |
| 构建报 `mtmd` 目标不存在 | llama.cpp 版本过旧，`LLAMA_BUILD_MTMD` 未支持；换 master |
| 选择模型后立刻闪退/报 OOM | 权重太大或 `largeHeap` 不足；先确认设备内存 ≥ 8GB |
| 识别结果整体错位 | 版面 ONNX 不是 PaddleX 三输入形态，回到 `impl/ios/tools/verify_layout_onnx.py` 核对 |
