// PP-DocLayoutV3 的 ONNX Runtime 实现（iOS / Android 共用同一份）。
//
// 模型 I/O 约定（PaddleX 导出形态，tools/verify_layout_onnx.py 是同一套约定的参考实现）：
//   image        [1,3,800,800] float32  RGB，letterbox 右下补 114，(x/255-mean)/std
//   im_shape     [1,2] float32          [原图高, 原图宽]
//   scale_factor [1,2] float32          [1/r, 1/r] → 输出坐标 = 原图坐标 / scale_factor
//   检测输出     [N,7] float32          [class_id, score, x1, y1, x2, y2, order_key]
#include "vlcore/engines.hpp"

// 引擎可用性自动探测。ONNX Runtime 的 iOS 二进制有三种打包形态，头文件路径各不相同：
//   1. 静态 framework（官方 pod archive）：<onnxruntime/onnxruntime_c_api.h>
//   2. 动态 library 型 xcframework：      <onnxruntime/core/session/onnxruntime_c_api.h>
//   3. Android AAR 平铺头：               <onnxruntime_c_api.h>
// 任意一种存在即编译推理路径，否则退化为「工厂返回 nullptr」。
#if defined(VLCORE_WITH_ORT)
#define VLCORE_ORT_ENABLED 1
#elif !defined(VLCORE_WITHOUT_ORT) && \
    (__has_include(<onnxruntime/onnxruntime_c_api.h>) || \
     __has_include(<onnxruntime/core/session/onnxruntime_c_api.h>) || \
     __has_include(<onnxruntime_c_api.h>))
#define VLCORE_ORT_ENABLED 1
#endif

#if defined(VLCORE_ORT_ENABLED)
#if __has_include(<onnxruntime/onnxruntime_c_api.h>)
#include <onnxruntime/onnxruntime_c_api.h>
#elif __has_include(<onnxruntime/core/session/onnxruntime_c_api.h>)
#include <onnxruntime/core/session/onnxruntime_c_api.h>
#else
#include <onnxruntime_c_api.h>
#endif
#endif

#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vlcore {
namespace {

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

#if defined(_WIN32)
/// ONNX Runtime 在 Windows 上把 ORTCHAR_T 定义成 wchar_t，路径必须转成 UTF-16。
std::wstring ToOrtPath(const std::string& path) {
    if (path.empty()) return std::wstring();
    const int length = static_cast<int>(path.size());
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), length, nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), length, wide.data(), size);
    return wide;
}
#endif

#if defined(VLCORE_ORT_ENABLED)

bool Ok(const OrtApi* api, OrtStatus* status, std::string* error) {
    if (status == nullptr) return true;
    const char* raw = api->GetErrorMessage(status);
    SetError(error, raw != nullptr ? std::string(raw) : std::string("ONNX Runtime 调用失败"));
    api->ReleaseStatus(status);
    return false;
}

class OrtLayoutEngine final : public LayoutEngine {
public:
    explicit OrtLayoutEngine(const OrtApi* api) : api_(api) {}

    ~OrtLayoutEngine() override {
        if (session_ != nullptr) api_->ReleaseSession(session_);
        if (options_ != nullptr) api_->ReleaseSessionOptions(options_);
        if (memory_ != nullptr) api_->ReleaseMemoryInfo(memory_);
        if (env_ != nullptr) api_->ReleaseEnv(env_);
    }

    bool Init(const std::string& path, std::string* error) {
        if (!Ok(api_, api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "vlcore-layout", &env_), error)) return false;
        if (!Ok(api_, api_->CreateSessionOptions(&options_), error)) return false;
        api_->SetIntraOpNumThreads(options_, 4);
        api_->SetSessionGraphOptimizationLevel(options_, ORT_ENABLE_ALL);
#if defined(_WIN32)
        const std::wstring wide_path = ToOrtPath(path);
        if (!Ok(api_, api_->CreateSession(env_, wide_path.c_str(), options_, &session_), error)) return false;
#else
        if (!Ok(api_, api_->CreateSession(env_, path.c_str(), options_, &session_), error)) return false;
#endif
        if (!Ok(api_, api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_), error)) return false;
        return ReadIoSpec(error);
    }

    bool Run(const LayoutInput& input, int image_width, int image_height,
             std::vector<float>* rows, std::string* error) override {
        const int64_t image_dims[4] = {1, 3, input.side, input.side};
        const int64_t pair_dims[2] = {1, 2};
        float im_shape[2] = {static_cast<float>(image_height), static_cast<float>(image_width)};
        const float inverse = 1.f / (input.scale > 0.f ? input.scale : 1.f);
        float scale_factor[2] = {inverse, inverse};

        OrtValue* image_value = nullptr;
        OrtValue* shape_value = nullptr;
        OrtValue* scale_value = nullptr;
        OrtValue* output = nullptr;
        bool success = false;

        do {
            if (!Ok(api_, api_->CreateTensorWithDataAsOrtValue(
                             memory_, const_cast<float*>(input.chw.data()),
                             input.chw.size() * sizeof(float), image_dims, 4,
                             ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &image_value), error)) break;
            if (!Ok(api_, api_->CreateTensorWithDataAsOrtValue(
                             memory_, im_shape, sizeof(im_shape), pair_dims, 2,
                             ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &shape_value), error)) break;
            if (!Ok(api_, api_->CreateTensorWithDataAsOrtValue(
                             memory_, scale_factor, sizeof(scale_factor), pair_dims, 2,
                             ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &scale_value), error)) break;

            std::vector<const char*> names;
            std::vector<const OrtValue*> values;
            bool known = true;
            for (const std::string& name : input_names_) {
                names.push_back(name.c_str());
                if (name == "image") {
                    values.push_back(image_value);
                } else if (name == "im_shape") {
                    values.push_back(shape_value);
                } else if (name == "scale_factor") {
                    values.push_back(scale_value);
                } else {
                    known = false;
                    break;
                }
            }
            if (!known) {
                SetError(error, "版面模型输入名不是 PaddleX 导出形态（image/im_shape/scale_factor）。");
                break;
            }

            const char* output_name = output_name_.c_str();
            if (!Ok(api_, api_->Run(session_, nullptr, names.data(), values.data(), names.size(),
                                    &output_name, 1, &output), error)) break;

            OrtTensorTypeAndShapeInfo* info = nullptr;
            if (!Ok(api_, api_->GetTensorTypeAndShape(output, &info), error)) break;
            size_t count = 0;
            OrtStatus* count_status = api_->GetTensorShapeElementCount(info, &count);
            api_->ReleaseTensorTypeAndShapeInfo(info);
            if (!Ok(api_, count_status, error)) break;

            void* raw = nullptr;
            if (!Ok(api_, api_->GetTensorMutableData(output, &raw), error)) break;

            const float* data = static_cast<const float*>(raw);
            rows->assign(data, data + count);
            success = true;
        } while (false);

        if (image_value != nullptr) api_->ReleaseValue(image_value);
        if (shape_value != nullptr) api_->ReleaseValue(shape_value);
        if (scale_value != nullptr) api_->ReleaseValue(scale_value);
        if (output != nullptr) api_->ReleaseValue(output);
        return success;
    }

private:
    /// 记录输入顺序；输出挑 [N,7] 那个（fetch_name_0），避免把指针网络的 [300,200,200] 当检测结果。
    bool ReadIoSpec(std::string* error) {
        OrtAllocator* allocator = nullptr;
        if (!Ok(api_, api_->GetAllocatorWithDefaultOptions(&allocator), error)) return false;

        size_t input_count = 0;
        if (!Ok(api_, api_->SessionGetInputCount(session_, &input_count), error)) return false;
        for (size_t i = 0; i < input_count; i++) {
            char* name = nullptr;
            if (!Ok(api_, api_->SessionGetInputName(session_, i, allocator, &name), error)) return false;
            input_names_.emplace_back(name);
            allocator->Free(allocator, name);
        }

        size_t output_count = 0;
        if (!Ok(api_, api_->SessionGetOutputCount(session_, &output_count), error)) return false;
        for (size_t i = 0; i < output_count; i++) {
            char* name = nullptr;
            if (!Ok(api_, api_->SessionGetOutputName(session_, i, allocator, &name), error)) return false;

            bool matched = false;
            OrtTypeInfo* type_info = nullptr;
            if (api_->SessionGetOutputTypeInfo(session_, i, &type_info) == nullptr && type_info != nullptr) {
                const OrtTensorTypeAndShapeInfo* tensor_info = nullptr;
                if (api_->CastTypeInfoToTensorInfo(type_info, &tensor_info) == nullptr && tensor_info != nullptr) {
                    size_t dims_count = 0;
                    if (api_->GetDimensionsCount(tensor_info, &dims_count) == nullptr && dims_count == 2) {
                        int64_t dims[2] = {0, 0};
                        if (api_->GetDimensions(tensor_info, dims, 2) == nullptr) {
                            matched = dims[1] == 7;
                        }
                    }
                }
                api_->ReleaseTypeInfo(type_info);
            }

            if (matched || (i + 1 == output_count && output_name_.empty())) {
                output_name_ = name;
            }
            allocator->Free(allocator, name);
            if (matched) break;
        }

        if (output_name_.empty()) {
            SetError(error, "版面模型没有可用的检测输出（期望 [N,7]）。");
            return false;
        }
        return true;
    }

    const OrtApi* api_{nullptr};
    OrtEnv* env_{nullptr};
    OrtSessionOptions* options_{nullptr};
    OrtSession* session_{nullptr};
    OrtMemoryInfo* memory_{nullptr};
    std::vector<std::string> input_names_;
    std::string output_name_;
};

#endif  // VLCORE_ORT_ENABLED

}  // namespace

bool LayoutEngineBuilt() {
#if defined(VLCORE_ORT_ENABLED)
    return true;
#else
    return false;
#endif
}

LayoutEngine* CreateOrtLayoutEngine(const std::string& model_path, std::string* error) {
#if !defined(VLCORE_ORT_ENABLED)
    (void)model_path;
    SetError(error, "当前构建未编译 ONNX Runtime 版面引擎（VLCORE_WITH_ORT）。");
    return nullptr;
#else
    if (model_path.empty()) {
        SetError(error, "尚未选择版面分析模型。");
        return nullptr;
    }
    const OrtApi* api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (api == nullptr) {
        SetError(error, "ONNX Runtime 版本不兼容（GetApi 失败）。");
        return nullptr;
    }
    auto* engine = new OrtLayoutEngine(api);
    std::string init_error;
    if (!engine->Init(model_path, &init_error)) {
        SetError(error, init_error.empty() ? "版面分析模型加载失败。" : init_error);
        delete engine;
        return nullptr;
    }
    return engine;
#endif
}

}  // namespace vlcore
