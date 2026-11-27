// 两个推理引擎的抽象。实现由编译开关决定：
//   VLCORE_WITH_ORT    → src/engines/ort_layout.cpp（ONNX Runtime C API）
//   VLCORE_WITH_LLAMA  → src/engines/mtmd_vlm.cpp（llama.cpp + libmtmd）
// 未开启时工厂返回 nullptr 并给出可读错误，核心逻辑仍可编译、可单测。
#pragma once

#include <string>
#include <vector>

#include "vlcore/core.hpp"

namespace vlcore {

class LayoutEngine {
public:
    virtual ~LayoutEngine() = default;

    /// rows 输出 N*7 = [class_id, score, x1, y1, x2, y2, order_key]（坐标已是原图像素）。
    virtual bool Run(const LayoutInput& input, int image_width, int image_height,
                     std::vector<float>* rows, std::string* error) = 0;
};

class VlmEngine {
public:
    virtual ~VlmEngine() = default;

    /// 单张已裁剪块图 + 提示词 → 生成文本（贪心解码）。
    virtual bool Generate(const RgbBuffer& image, const std::string& prompt, int max_tokens,
                          std::string* text, std::string* error) = 0;
};

/// 远程识别回调：平台侧把 (rgb, prompt) 发给自己选择的服务，返回 malloc 的文本（核心负责 free）。
using RemoteRecognizeFn = char* (*)(const uint8_t* rgb, int width, int height,
                                    const char* prompt, void* user, char** error);

LayoutEngine* CreateOrtLayoutEngine(const std::string& model_path, std::string* error);
VlmEngine* CreateMtmdEngine(const std::string& model_path, const std::string& mmproj_path,
                            int n_ctx, int n_threads, std::string* error);

/// 该引擎是否被编译进本次构建（UI 用来区分「没选模型」与「没链接库」）。
bool LayoutEngineBuilt();
bool VlmEngineBuilt();

}  // namespace vlcore
