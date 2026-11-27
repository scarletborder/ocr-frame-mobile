// PaddleOCR-VL 的 llama.cpp（libmtmd）实现（iOS / Android 共用同一份）。
//
// 只承担第二阶段：一张已裁剪的版面块图 + 官方提示词 → 生成文本。
// chat template 与官方一致：
//   <|begin_of_sentence|>User: <__media__>{prompt}\nAssistant:\n
// 其中 <__media__> 是 mtmd 的 marker，tokenize 时替换成视觉 token。
//
// ⚠️ 适配点：libmtmd 是实验性 C API，签名会随 llama.cpp 变动；升级时只改本文件。
#include "vlcore/engines.hpp"

// 自动探测 llama.cpp 头：iOS 的 xcframework 是 <llama/…>，Android / 桌面是平铺的 <llama.h>。
#if defined(VLCORE_WITH_LLAMA)
#define VLCORE_LLAMA_ENABLED 1
#elif !defined(VLCORE_WITHOUT_LLAMA) && \
    ((__has_include(<llama/llama.h>) && __has_include(<llama/mtmd.h>)) || \
     (__has_include(<llama.h>) && __has_include(<mtmd.h>)))
#define VLCORE_LLAMA_ENABLED 1
#endif

#if defined(VLCORE_LLAMA_ENABLED)
#if __has_include(<llama/llama.h>)
#include <llama/llama.h>
#include <llama/mtmd.h>
#else
#include <llama.h>
#include <mtmd.h>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vlcore {
namespace {

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

#if defined(VLCORE_LLAMA_ENABLED)

/// llama.cpp / ggml 自己的日志也转进共享核心的日志通道。
/// 这一点很关键：真机上「整页全部提示词推理失败」的真实原因（Metal 分配失败、KV 槽位不足、
/// mmproj 与权重不匹配……）基本只出现在 llama.cpp 自己的输出里，我们兜底的文案说不清。
void LlamaLogCallback(enum ggml_log_level level, const char* text, void* /*user*/) {
    if (text == nullptr) return;
    vlcore::LogLevel mapped = vlcore::kLogDebug;
    switch (level) {
        case GGML_LOG_LEVEL_ERROR: mapped = vlcore::kLogError; break;
        case GGML_LOG_LEVEL_WARN: mapped = vlcore::kLogWarn; break;
        case GGML_LOG_LEVEL_INFO: mapped = vlcore::kLogInfo; break;
        case GGML_LOG_LEVEL_DEBUG: mapped = vlcore::kLogDebug; break;
        default: mapped = vlcore::kLogDebug; break;
    }
    std::string line(text);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return;   // llama.cpp 经常单独打一个换行，别在日志页里留空行
    vlcore::LogLine(mapped, line);
}

std::string FileName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// 距 started 的秒数（两位小数），日志里统一用它，避免每处各写一遍 snprintf。
std::string Seconds(const std::chrono::steady_clock::time_point& started) {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", elapsed);
    return std::string(buffer);
}

/// 逐 chunk 求值：文本 chunk 走 llama_decode，图像 chunk 先 mtmd_encode_chunk 再喂 embedding。
/// 等价于 mtmd-helper 的 mtmd_helper_eval_chunks，但签名固定在本文件内，不受上游 helper 变更影响。
/// 失败时把「哪一步、哪个数越界」写进 why，供上层日志与错误文案使用。
bool EvalChunks(mtmd_context* mctx, llama_context* lctx, const mtmd_input_chunks* chunks,
                llama_pos* n_past, std::string* why) {
    const int32_t n_batch = std::max<int32_t>(1, llama_n_batch(lctx));
    const int32_t n_ctx = static_cast<int32_t>(llama_n_ctx(lctx));
    const int32_t n_embd = llama_model_n_embd(llama_get_model(lctx));
    const size_t n_chunks = mtmd_input_chunks_size(chunks);

    for (size_t index = 0; index < n_chunks; index++) {
        const mtmd_input_chunk* chunk = mtmd_input_chunks_get(chunks, index);
        const enum mtmd_input_chunk_type type = mtmd_input_chunk_get_type(chunk);
        const bool is_last_chunk = (index + 1 == n_chunks);

        if (type == MTMD_INPUT_CHUNK_TYPE_TEXT) {
            size_t n_tokens = 0;
            const llama_token* tokens = mtmd_input_chunk_get_tokens_text(chunk, &n_tokens);
            if (tokens == nullptr || n_tokens == 0) continue;

            size_t offset = 0;
            while (offset < n_tokens) {
                const size_t take = std::min(static_cast<size_t>(n_batch), n_tokens - offset);
                if (*n_past + static_cast<llama_pos>(take) > static_cast<llama_pos>(n_ctx)) {
                    SetError(why, "文本 chunk 超出上下文上限（pos=" + std::to_string(*n_past) +
                                      "，本次 " + std::to_string(take) +
                                      "，n_ctx=" + std::to_string(n_ctx) + "）");
                    return false;
                }
                llama_batch batch = llama_batch_init(static_cast<int32_t>(take), 0, 1);
                for (size_t k = 0; k < take; k++) {
                    const int32_t slot = batch.n_tokens++;
                    batch.token[slot] = tokens[offset + k];
                    batch.pos[slot] = *n_past + static_cast<llama_pos>(k);
                    batch.n_seq_id[slot] = 1;
                    batch.seq_id[slot][0] = 0;
                    batch.logits[slot] =
                        (is_last_chunk && offset + take == n_tokens && k + 1 == take) ? 1 : 0;
                }
                const int32_t rc = llama_decode(lctx, batch);
                llama_batch_free(batch);
                if (rc != 0) {
                    SetError(why, "文本 chunk 解码失败（rc=" + std::to_string(rc) +
                                      "，n_tokens=" + std::to_string(take) +
                                      "，pos=" + std::to_string(*n_past) +
                                      "，n_ctx=" + std::to_string(n_ctx) + "）");
                    return false;
                }
                *n_past += static_cast<llama_pos>(take);
                offset += take;
            }
        } else if (type == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
            const size_t n_tokens = mtmd_input_chunk_get_n_tokens(chunk);
            if (mtmd_encode_chunk(mctx, chunk) != 0) {
                SetError(why, "视觉编码失败（mtmd_encode_chunk 返回非零，视觉 token=" +
                                  std::to_string(n_tokens) + "）");
                return false;
            }
            const float* embd = mtmd_get_output_embd(mctx);
            if (embd == nullptr || n_tokens == 0) {
                SetError(why, "视觉编码没有产出 embedding（视觉 token=" +
                                  std::to_string(n_tokens) + "）");
                return false;
            }
            if (n_tokens > static_cast<size_t>(n_batch)) {
                SetError(why, "单块视觉 token 超过 n_batch（" + std::to_string(n_tokens) +
                                  " > " + std::to_string(n_batch) + "）");
                return false;
            }
            if (*n_past + static_cast<llama_pos>(n_tokens) > static_cast<llama_pos>(n_ctx)) {
                SetError(why, "视觉 token 超出上下文上限（pos=" + std::to_string(*n_past) +
                                  "，视觉 " + std::to_string(n_tokens) +
                                  "，n_ctx=" + std::to_string(n_ctx) + "）");
                return false;
            }

            llama_batch batch = llama_batch_init(static_cast<int32_t>(n_tokens), n_embd, 1);
            std::memcpy(batch.embd, embd, n_tokens * static_cast<size_t>(n_embd) * sizeof(float));
            for (size_t k = 0; k < n_tokens; k++) {
                const int32_t slot = batch.n_tokens++;
                batch.pos[slot] = *n_past + static_cast<llama_pos>(k);
                batch.n_seq_id[slot] = 1;
                batch.seq_id[slot][0] = 0;
                batch.logits[slot] = (is_last_chunk && k + 1 == n_tokens) ? 1 : 0;
            }
            const int32_t rc = llama_decode(lctx, batch);
            llama_batch_free(batch);
            if (rc != 0) {
                SetError(why, "图像 chunk 解码失败（rc=" + std::to_string(rc) +
                                  "，视觉 token=" + std::to_string(n_tokens) +
                                  "，pos=" + std::to_string(*n_past) +
                                  "，n_ctx=" + std::to_string(n_ctx) + "）");
                return false;
            }
            *n_past += static_cast<llama_pos>(mtmd_input_chunk_get_n_pos(chunk));
        } else {
            SetError(why, "未知 chunk 类型（" + std::to_string(static_cast<int>(type)) + "）");
            return false;
        }
    }
    return true;
}

class MtmdEngine final : public VlmEngine {
public:
    ~MtmdEngine() override {
        if (mtmd_ != nullptr) mtmd_free(mtmd_);
        if (ctx_ != nullptr) llama_free(ctx_);
        if (model_ != nullptr) llama_model_free(model_);
    }

    bool Init(const std::string& model_path, const std::string& mmproj_path,
              int n_ctx, int n_threads, std::string* error) {
        static std::once_flag backend_once;
        std::call_once(backend_once, []() {
            // llama.cpp 的日志先进核心日志通道，再初始化后端（后端初始化本身的报错也要能看到）。
            llama_log_set(LlamaLogCallback, nullptr);
            llama_backend_init();
        });

        LogLine(kLogInfo, "加载 VLM 权重：" + FileName(model_path));
        const auto load_started = std::chrono::steady_clock::now();

        llama_model_params model_params = llama_model_default_params();
        model_params.n_gpu_layers = 999;   // 有 GPU 后端时全部卸载，否则 llama.cpp 自行回落 CPU
        model_ = llama_model_load_from_file(model_path.c_str(), model_params);
        if (model_ == nullptr) {
            SetError(error, "视觉语言模型加载失败（文件损坏或架构不受当前 llama.cpp 支持）。");
            return false;
        }
        LogLine(kLogInfo, "权重加载完成：用时 " + Seconds(load_started) + " s");

        llama_context_params ctx_params = llama_context_default_params();
        ctx_params.n_ctx = static_cast<uint32_t>(std::max(2048, n_ctx));
        // 动态分辨率视觉编码器单块可产出上千 token（image_max_tokens=2048），
        // 再叠加提示词 token，n_batch 必须留出余量，否则 llama_decode 直接报错。
        ctx_params.n_batch = 4096;
        ctx_params.n_ubatch = 512;
        ctx_params.n_threads = std::max(1, n_threads);
        ctx_params.n_threads_batch = std::max(1, n_threads);
        ctx_ = llama_init_from_model(model_, ctx_params);
        if (ctx_ == nullptr) {
            SetError(error, "推理上下文创建失败（内存不足？请关闭其他应用后重试）。");
            return false;
        }
        vocab_ = llama_model_get_vocab(model_);
        LogLine(kLogInfo, "推理上下文就绪：n_ctx=" + std::to_string(llama_n_ctx(ctx_)) +
                              "，n_batch=" + std::to_string(llama_n_batch(ctx_)) +
                              "，threads=" + std::to_string(n_threads) +
                              "，n_gpu_layers=" + std::to_string(model_params.n_gpu_layers));

        mmproj_path_ = mmproj_path;
        threads_ = std::max(1, n_threads);
        return InitMtmd(true, error);
    }

    /// 视觉编码器后端可以重建：Metal 对某些视觉塔（如 PaddleOCR-VL 的 NaViT）支持不全时，
    /// encode 会整块失败，此时退回 CPU 重跑一次，而不是把整页所有块都判成未识别。
    bool InitMtmd(bool use_gpu, std::string* error) {
        if (mtmd_ != nullptr) {
            mtmd_free(mtmd_);
            mtmd_ = nullptr;
        }
        mtmd_context_params params = mtmd_context_params_default();
        params.use_gpu = use_gpu;
        params.print_timings = false;
        params.n_threads = threads_;
        params.warmup = use_gpu;   // CPU 回退时跳过预热，避免额外耗时
        // 移动端把单块视觉 token 上限定在 2048，避免单块编码时间和内存失控。
        params.image_max_tokens = 2048;

        LogLine(kLogInfo, std::string("加载视觉投影器：") + FileName(mmproj_path_) +
                              "，后端=" + (use_gpu ? "GPU" : "CPU") +
                              "，warmup=" + (use_gpu ? "是" : "否"));
        const auto started = std::chrono::steady_clock::now();
        mtmd_ = mtmd_init_from_file(mmproj_path_.c_str(), model_, params);
        if (mtmd_ == nullptr) {
            SetError(error, "视觉投影器加载失败，请确认 -mmproj.gguf 与主权重版本一致。");
            return false;
        }
        if (!mtmd_support_vision(mtmd_)) {
            SetError(error, "所选模型不支持图像输入。");
            return false;
        }
        LogLine(kLogInfo, std::string("视觉投影器就绪（") + (use_gpu ? "GPU" : "CPU") +
                              "）：用时 " + Seconds(started) + " s");
        using_gpu_ = use_gpu;
        return true;
    }

    bool Generate(const RgbBuffer& image, const std::string& prompt, int max_tokens,
                  std::string* text, std::string* error) override {
        if (!image.Valid()) {
            SetError(error, "待识别图像为空。");
            return false;
        }
        // 上一次重建视觉投影器失败后 mtmd_ 会留在 nullptr：此时直接给出可读原因，
        // 不能让 mtmd_tokenize(nullptr, …) 变成未定义行为。
        if (mtmd_ == nullptr) {
            SetError(error, "视觉投影器未加载（上次重建失败），请重启 App 或重新选择模型。");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto block_started = std::chrono::steady_clock::now();

        const std::string marker = mtmd_default_marker();
        const std::string message = "User: " + marker + prompt + "\nAssistant:\n";

        mtmd_input_text input_text{};
        input_text.text = message.c_str();
        input_text.text_len = message.size();
        input_text.add_special = true;    // 由 tokenizer 补 <|begin_of_sentence|>
        input_text.parse_special = true;

        mtmd_bitmap* bitmap = mtmd_bitmap_init(static_cast<uint32_t>(image.width),
                                               static_cast<uint32_t>(image.height),
                                               image.pixels.data());
        mtmd_input_chunks* chunks = mtmd_input_chunks_init();
        if (bitmap == nullptr || chunks == nullptr) {
            if (bitmap != nullptr) mtmd_bitmap_free(bitmap);
            if (chunks != nullptr) mtmd_input_chunks_free(chunks);
            SetError(error, "图像 token 化准备失败。");
            return false;
        }

        bool success = false;
        do {
            const mtmd_bitmap* bitmaps[1] = {bitmap};
            if (mtmd_tokenize(mtmd_, chunks, &input_text, bitmaps, 1) != 0) {
                SetError(error, "图像编码失败（图像占位符与图片数量不匹配）。");
                break;
            }

            // token 化后的构成是判断「是不是 token 超了」的唯一依据，直接记进日志。
            std::string chunk_summary;
            for (size_t i = 0; i < mtmd_input_chunks_size(chunks); i++) {
                const mtmd_input_chunk* chunk = mtmd_input_chunks_get(chunks, i);
                const bool is_image =
                    mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_IMAGE;
                if (!chunk_summary.empty()) chunk_summary += " + ";
                chunk_summary += is_image ? "图像 " : "文本 ";
                chunk_summary += std::to_string(mtmd_input_chunk_get_n_tokens(chunk)) + " token";
            }
            LogLine(kLogInfo, "提示词 " + prompt + "，图像 " + std::to_string(image.width) + "x" +
                                  std::to_string(image.height) + "，切分：" + chunk_summary +
                                  "（n_batch=" + std::to_string(llama_n_batch(ctx_)) +
                                  "，n_ctx=" + std::to_string(llama_n_ctx(ctx_)) + "）");

            const auto evaluate = [&](llama_pos* n_past, std::string* why) {
                llama_memory_clear(llama_get_memory(ctx_), true);
                *n_past = 0;
                return EvalChunks(mtmd_, ctx_, chunks, n_past, why);
            };

            llama_pos n_past = 0;
            std::string eval_why;
            bool evaluated = evaluate(&n_past, &eval_why);
            if (!evaluated && using_gpu_) {
                // GPU 视觉编码失败 → 退回 CPU 重建 mtmd 再试一次（chunks 只含 token 序列，可复用）
                LogLine(kLogWarn, "GPU 侧求值失败，重建视觉投影器并退回 CPU 重试一次：" + eval_why);
                std::string retry_error;
                if (InitMtmd(false, &retry_error)) {
                    eval_why.clear();
                    evaluated = evaluate(&n_past, &eval_why);
                } else {
                    eval_why += "；CPU 回退也失败：" + retry_error;
                }
            }
            if (!evaluated) {
                LogLine(kLogError, "块求值失败：" + eval_why);
                SetError(error, "提示词推理失败（" + eval_why + "）");
                break;
            }
            LogLine(kLogInfo, "提示词求值完成：pos=" + std::to_string(n_past) + "，用时 " +
                                  Seconds(block_started) + " s（含视觉编码）");

            llama_sampler_chain_params sampler_params = llama_sampler_chain_default_params();
            sampler_params.no_perf = true;
            llama_sampler* sampler = llama_sampler_chain_init(sampler_params);
            llama_sampler_chain_add(sampler, llama_sampler_init_greedy());   // 官方默认 do_sample=False

            std::string output;
            int generated = 0;
            bool hit_limit = false;
            bool decode_failed = false;
            const auto generate_started = std::chrono::steady_clock::now();
            for (int step = 0; step < std::max(1, max_tokens); step++) {
                const llama_token token = llama_sampler_sample(sampler, ctx_, -1);
                if (llama_vocab_is_eog(vocab_, token)) break;
                if (step + 1 == std::max(1, max_tokens)) hit_limit = true;

                char piece[256];
                const int32_t n = llama_token_to_piece(vocab_, token, piece,
                                                       static_cast<int32_t>(sizeof(piece)), 0, true);
                if (n > 0) output.append(piece, static_cast<size_t>(n));

                llama_batch batch = llama_batch_init(1, 0, 1);
                batch.token[0] = token;
                batch.pos[0] = n_past;
                batch.n_seq_id[0] = 1;
                batch.seq_id[0][0] = 0;
                batch.logits[0] = 1;
                batch.n_tokens = 1;
                const int32_t rc = llama_decode(ctx_, batch);
                llama_batch_free(batch);
                generated += 1;
                if (rc != 0) {
                    decode_failed = true;
                    LogLine(kLogWarn, "生成过程中 llama_decode 返回 " + std::to_string(rc) +
                                          "，第 " + std::to_string(generated) +
                                          " 个 token 后中断（pos=" + std::to_string(n_past) + "）");
                    break;
                }
                n_past += 1;
            }

            llama_sampler_free(sampler);

            const double generate_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - generate_started)
                    .count();
            const double speed = generate_seconds > 0.001 ? generated / generate_seconds : 0.0;
            char speed_text[32];
            std::snprintf(speed_text, sizeof(speed_text), "%.1f", speed);
            LogLine(hit_limit ? kLogWarn : kLogInfo,
                    "生成 " + std::to_string(generated) + " token，用时 " + Seconds(generate_started) +
                        " s（" + speed_text + " tok/s），输出 " + std::to_string(output.size()) +
                        " 字节，整块 " + Seconds(block_started) + " s" +
                        (hit_limit ? "；已达生成长度上限且未输出结束符，结果可能被截断" : ""));
            if (decode_failed) {
                LogLine(kLogWarn, "该块输出可能不完整（生成阶段解码中断）。");
            }

            if (text != nullptr) *text = output;
            success = true;
        } while (false);

        mtmd_input_chunks_free(chunks);
        mtmd_bitmap_free(bitmap);
        return success;
    }

private:
    llama_model* model_{nullptr};
    llama_context* ctx_{nullptr};
    mtmd_context* mtmd_{nullptr};
    const llama_vocab* vocab_{nullptr};
    std::string mmproj_path_;
    int threads_{4};
    bool using_gpu_{false};
    std::mutex mutex_;
};

#endif  // VLCORE_LLAMA_ENABLED

}  // namespace

bool VlmEngineBuilt() {
#if defined(VLCORE_LLAMA_ENABLED)
    return true;
#else
    return false;
#endif
}

VlmEngine* CreateMtmdEngine(const std::string& model_path, const std::string& mmproj_path,
                            int n_ctx, int n_threads, std::string* error) {
#if !defined(VLCORE_LLAMA_ENABLED)
    (void)model_path;
    (void)mmproj_path;
    (void)n_ctx;
    (void)n_threads;
    SetError(error, "当前构建未编译 llama.cpp 视觉引擎（VLCORE_WITH_LLAMA）。");
    return nullptr;
#else
    if (model_path.empty()) {
        SetError(error, "尚未选择视觉语言模型（.gguf）。");
        return nullptr;
    }
    if (mmproj_path.empty()) {
        SetError(error, "缺少视觉投影器（*-mmproj.gguf）。");
        return nullptr;
    }
    auto* engine = new MtmdEngine();
    std::string init_error;
    if (!engine->Init(model_path, mmproj_path, n_ctx, n_threads, &init_error)) {
        SetError(error, init_error.empty() ? "视觉语言模型加载失败。" : init_error);
        delete engine;
        return nullptr;
    }
    return engine;
#endif
}

}  // namespace vlcore
