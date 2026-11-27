#include "vlcore/vlcore_c.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vlcore/core.hpp"
#include "vlcore/pipeline.hpp"

struct vlcore_engine {
    std::unique_ptr<vlcore::Pipeline> pipeline;
};

namespace {

char* Duplicate(const std::string& text) {
    char* buffer = static_cast<char*>(std::malloc(text.size() + 1));
    if (buffer == nullptr) return nullptr;
    std::memcpy(buffer, text.c_str(), text.size() + 1);
    return buffer;
}

void SetError(char** error, const std::string& message) {
    if (error != nullptr) *error = Duplicate(message);
}

std::string Safe(const char* text) {
    return text != nullptr ? std::string(text) : std::string();
}

/// 日志里只留文件名：完整沙盒路径对排查没用，还会把日志页刷满。
std::string BaseName(const char* path) {
    const std::string text = Safe(path);
    if (text.empty()) return "（未选择）";
    const size_t slash = text.find_last_of("/\\");
    return slash == std::string::npos ? text : text.substr(slash + 1);
}

bool ToBuffer(const uint8_t* rgb, int width, int height, vlcore::RgbBuffer* out) {
    if (rgb == nullptr || width <= 0 || height <= 0) return false;
    const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    out->width = width;
    out->height = height;
    out->pixels.assign(rgb, rgb + size);
    return out->Valid();
}

std::string RunResultJson(const std::string& markdown, const std::vector<vlcore::Block>& blocks,
                          double seconds) {
    char seconds_buffer[32];
    std::snprintf(seconds_buffer, sizeof(seconds_buffer), "%.3f", seconds);
    std::string out = "{\"markdown\":\"";
    out += vlcore::EscapeJson(markdown);
    out += "\",\"blocks\":";
    out += vlcore::BlocksToJson(blocks);
    out += ",\"seconds\":";
    out += seconds_buffer;
    out += "}";
    return out;
}

}  // namespace

extern "C" {

vlcore_engine_t* vlcore_create(const char* layout_onnx_path, const char* vlm_gguf_path,
                               const char* mmproj_gguf_path, int n_ctx, int n_threads,
                               char** error) {
    std::string message;

    vlcore::LogLine(vlcore::kLogInfo,
                    "引擎创建：n_ctx=" + std::to_string(n_ctx) +
                        "，threads=" + std::to_string(n_threads) +
                        "，版面=" + BaseName(layout_onnx_path) +
                        "，VLM=" + BaseName(vlm_gguf_path) +
                        "，投影器=" + BaseName(mmproj_gguf_path));

    std::unique_ptr<vlcore::LayoutEngine> layout;
    if (layout_onnx_path != nullptr && layout_onnx_path[0] != '\0') {
        layout.reset(vlcore::CreateOrtLayoutEngine(layout_onnx_path, &message));
        if (layout == nullptr) {
            vlcore::LogLine(vlcore::kLogError, "版面引擎创建失败：" +
                                                   (message.empty() ? "未知原因" : message));
            SetError(error, message.empty() ? "版面分析模型加载失败。" : message);
            return nullptr;
        }
        vlcore::LogLine(vlcore::kLogInfo, "版面引擎已就绪。");
    }

    std::unique_ptr<vlcore::VlmEngine> vlm;
    if (vlm_gguf_path != nullptr && vlm_gguf_path[0] != '\0') {
        vlm.reset(vlcore::CreateMtmdEngine(vlm_gguf_path, Safe(mmproj_gguf_path),
                                           n_ctx, n_threads, &message));
        if (vlm == nullptr) {
            vlcore::LogLine(vlcore::kLogError, "VLM 引擎创建失败：" +
                                                   (message.empty() ? "未知原因" : message));
            SetError(error, message.empty() ? "视觉语言模型加载失败。" : message);
            return nullptr;
        }
        vlcore::LogLine(vlcore::kLogInfo, "VLM 引擎已就绪。");
    }

    auto* engine = new vlcore_engine();
    engine->pipeline = std::make_unique<vlcore::Pipeline>(std::move(layout), std::move(vlm));
    return engine;
}

void vlcore_destroy(vlcore_engine_t* engine) {
    delete engine;
}

void vlcore_set_remote(vlcore_engine_t* engine, vlcore_remote_fn fn, void* user) {
    if (engine == nullptr || engine->pipeline == nullptr) return;
    engine->pipeline->set_remote(fn, user);
}

int vlcore_has_layout(const vlcore_engine_t* engine) {
    return engine != nullptr && engine->pipeline != nullptr && engine->pipeline->has_layout();
}

int vlcore_has_vlm(const vlcore_engine_t* engine) {
    return engine != nullptr && engine->pipeline != nullptr && engine->pipeline->has_vlm();
}

char* vlcore_run(vlcore_engine_t* engine, const uint8_t* rgb, int width, int height,
                 vlcore_progress_fn progress, void* user, char** error) {
    if (engine == nullptr || engine->pipeline == nullptr) {
        SetError(error, "识别引擎未初始化。");
        return nullptr;
    }
    vlcore::RgbBuffer page;
    if (!ToBuffer(rgb, width, height, &page)) {
        SetError(error, "待识别图片无效（无法解码）。");
        return nullptr;
    }

    std::string markdown;
    std::vector<vlcore::Block> blocks;
    double seconds = 0.0;
    std::string message;

    vlcore::LogLine(vlcore::kLogInfo,
                    "开始整页识别：" + std::to_string(width) + "x" + std::to_string(height));

    const std::function<void(int, int)> on_progress = [progress, user](int done, int total) {
        if (progress != nullptr) progress(done, total, user);
    };

    if (!engine->pipeline->Run(page, on_progress, &markdown, &blocks, &seconds, &message)) {
        vlcore::LogLine(vlcore::kLogError, "整页识别失败：" +
                                               (message.empty() ? "未知原因" : message));
        SetError(error, message.empty() ? "识别失败。" : message);
        return nullptr;
    }

    char seconds_text[32];
    std::snprintf(seconds_text, sizeof(seconds_text), "%.2f", seconds);
    vlcore::LogLine(vlcore::kLogInfo,
                    "整页识别完成：用时 " + std::string(seconds_text) + " s，" +
                        std::to_string(blocks.size()) + " 个版面块");
    return Duplicate(RunResultJson(markdown, blocks, seconds));
}

char* vlcore_layout_json(vlcore_engine_t* engine, const uint8_t* rgb, int width, int height,
                         char** error) {
    if (engine == nullptr || engine->pipeline == nullptr) {
        SetError(error, "识别引擎未初始化。");
        return nullptr;
    }
    vlcore::RgbBuffer page;
    if (!ToBuffer(rgb, width, height, &page)) {
        SetError(error, "待识别图片无效（无法解码）。");
        return nullptr;
    }

    std::vector<vlcore::Block> blocks;
    std::string message;
    if (!engine->pipeline->Detect(page, &blocks, &message)) {
        SetError(error, message.empty() ? "版面分析失败。" : message);
        return nullptr;
    }
    return Duplicate(vlcore::BlocksToJson(blocks));
}

char* vlcore_recognize_block(vlcore_engine_t* engine, const uint8_t* rgb, int width, int height,
                             const char* label, char** error) {
    if (engine == nullptr || engine->pipeline == nullptr) {
        SetError(error, "识别引擎未初始化。");
        return nullptr;
    }
    vlcore::RgbBuffer block;
    if (!ToBuffer(rgb, width, height, &block)) {
        SetError(error, "版面块图像无效。");
        return nullptr;
    }

    std::string text;
    std::string message;
    if (!engine->pipeline->Recognize(block, Safe(label), &text, &message)) {
        SetError(error, message.empty() ? "单块识别失败。" : message);
        return nullptr;
    }
    return Duplicate(text);
}

char* vlcore_prompt_for_label(const char* label) {
    return Duplicate(vlcore::PromptFor(Safe(label)));
}

char* vlcore_region_type(const char* label) {
    return Duplicate(vlcore::RegionType(Safe(label)));
}

int vlcore_layout_engine_built(void) {
    return vlcore::LayoutEngineBuilt() ? 1 : 0;
}

int vlcore_vlm_engine_built(void) {
    return vlcore::VlmEngineBuilt() ? 1 : 0;
}

const char* vlcore_preset_name(void) {
    return vlcore::Preset::kName;
}

const char* vlcore_preset_version(void) {
    return vlcore::Preset::kVersion;
}

const char* vlcore_pipeline_version(void) {
    return vlcore::Preset::kPipelineVersion;
}

int vlcore_max_new_tokens(void) {
    return vlcore::Preset::kMaxNewTokens;
}

void vlcore_set_log(vlcore_log_fn fn, void* user) {
    if (fn == nullptr) {
        vlcore::SetLogSink(nullptr, nullptr);
        return;
    }
    // 两个回调类型签名一致（int, const char*, void*），都走 C ABI，可直接转换。
    vlcore::SetLogSink(reinterpret_cast<vlcore::LogSink>(fn), user);
}

void vlcore_log(int level, const char* message) {
    const int clamped = level < 0 ? 0 : (level > 3 ? 3 : level);
    vlcore::LogLine(static_cast<vlcore::LogLevel>(clamped), Safe(message));
}

void vlcore_free_string(char* text) {
    std::free(text);
}

}  // extern "C"
