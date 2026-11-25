#include "vlcore/pipeline.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace vlcore {
namespace {

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

}  // namespace

Pipeline::Pipeline(std::unique_ptr<LayoutEngine> layout, std::unique_ptr<VlmEngine> vlm)
    : layout_(std::move(layout)), vlm_(std::move(vlm)) {}

void Pipeline::set_remote(RemoteRecognizeFn fn, void* user) {
    remote_ = fn;
    remote_user_ = user;
}

bool Pipeline::Detect(const RgbBuffer& page, std::vector<Block>* blocks, std::string* error) const {
    if (layout_ == nullptr) {
        SetError(error, "尚未加载版面分析模型。");
        return false;
    }
    if (!page.Valid()) {
        SetError(error, "待识别图片无效（无法解码）。");
        return false;
    }

    LayoutInput input;
    if (!BuildLayoutInput(page, &input)) {
        SetError(error, "版面分析预处理失败。");
        return false;
    }

    std::vector<float> rows;
    std::string layout_error;
    if (!layout_->Run(input, page.width, page.height, &rows, &layout_error)) {
        SetError(error, layout_error.empty() ? "版面分析推理失败。" : layout_error);
        return false;
    }

    const int row_count = static_cast<int>(rows.size() / 7);
    *blocks = DecodeLayout(rows.data(), row_count, input.scale, page.width, page.height, params_);
    if (blocks->empty()) {
        SetError(error, "没有检测到任何版面区域，请换一张更清晰的图片。");
        return false;
    }
    return true;
}

bool Pipeline::Recognize(const RgbBuffer& block_image, const std::string& label,
                         std::string* text, std::string* error) {
    if (!block_image.Valid()) {
        SetError(error, "版面块图像为空。");
        return false;
    }

    const RgbBuffer limited = LimitPixels(block_image);
    const std::string prompt = PromptFor(label);

    if (vlm_ != nullptr) {
        std::string vlm_error;
        if (!vlm_->Generate(limited, prompt, Preset::kMaxNewTokens, text, &vlm_error)) {
            SetError(error, vlm_error.empty() ? "视觉语言模型识别失败。" : vlm_error);
            return false;
        }
        return true;
    }

    if (remote_ != nullptr) {
        char* message = nullptr;
        char* result = remote_(limited.pixels.data(), limited.width, limited.height,
                               prompt.c_str(), remote_user_, &message);
        if (result == nullptr) {
            SetError(error, message != nullptr ? std::string(message) : std::string("远程识别失败。"));
            if (message != nullptr) std::free(message);
            return false;
        }
        *text = result;
        std::free(result);
        return true;
    }

    SetError(error, "尚未加载视觉语言模型，也没有配置远程识别服务。");
    return false;
}

bool Pipeline::Run(const RgbBuffer& page, const std::function<void(int, int)>& progress,
                   std::string* markdown, std::vector<Block>* blocks, double* seconds,
                   std::string* error) {
    const auto started = std::chrono::steady_clock::now();

    std::vector<Block> detected;
    if (!Detect(page, &detected, error)) return false;

    const int total = static_cast<int>(detected.size());
    int done = 0;
    for (Block& block : detected) {
        done += 1;
        if (progress) progress(done, total);

        if (!ShouldRecognize(block.label)) {
            block.recognized = false;
            continue;
        }
        const RgbBuffer crop = CropRgb(page, block.box);
        if (!crop.Valid()) {
            block.recognized = false;
            continue;
        }

        std::string text;
        std::string block_error;
        if (Recognize(crop, block.label, &text, &block_error)) {
            block.text = text;
            block.recognized = true;
        } else {
            // 单块失败只标记未识别，保留标签与几何，避免整页失败。
            block.recognized = false;
        }
    }

    if (markdown != nullptr) *markdown = RenderMarkdown(detected);
    if (blocks != nullptr) *blocks = detected;
    if (seconds != nullptr) {
        const auto finished = std::chrono::steady_clock::now();
        *seconds = std::chrono::duration<double>(finished - started).count();
    }
    return true;
}

}  // namespace vlcore
