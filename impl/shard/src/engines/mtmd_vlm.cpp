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
#include <cmath>
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

/// 逐 chunk 求值：文本 chunk 走 llama_decode，图像 chunk 先 mtmd_encode_chunk 再喂 embedding。
/// 等价于 mtmd-helper 的 mtmd_helper_eval_chunks，但签名固定在本文件内，不受上游 helper 变更影响。
bool EvalChunks(mtmd_context* mctx, llama_context* lctx, const mtmd_input_chunks* chunks,
                llama_pos* n_past) {
    const int32_t n_batch = std::max<int32_t>(1, llama_n_batch(lctx));
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
                if (rc != 0) return false;
                *n_past += static_cast<llama_pos>(take);
                offset += take;
            }
        } else if (type == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
            if (mtmd_encode_chunk(mctx, chunk) != 0) return false;
            const float* embd = mtmd_get_output_embd(mctx);
            const size_t n_tokens = mtmd_input_chunk_get_n_tokens(chunk);
            if (embd == nullptr || n_tokens == 0) return false;
            if (n_tokens > static_cast<size_t>(n_batch)) return false;

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
            if (rc != 0) return false;
            *n_past += static_cast<llama_pos>(mtmd_input_chunk_get_n_pos(chunk));
        } else {
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
        std::call_once(backend_once, []() { llama_backend_init(); });

        llama_model_params model_params = llama_model_default_params();
        model_params.n_gpu_layers = 999;   // 有 GPU 后端时全部卸载，否则 llama.cpp 自行回落 CPU
        model_ = llama_model_load_from_file(model_path.c_str(), model_params);
        if (model_ == nullptr) {
            SetError(error, "视觉语言模型加载失败（文件损坏或架构不受当前 llama.cpp 支持）。");
            return false;
        }

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

        mtmd_ = mtmd_init_from_file(mmproj_path_.c_str(), model_, params);
        if (mtmd_ == nullptr) {
            SetError(error, "视觉投影器加载失败，请确认 -mmproj.gguf 与主权重版本一致。");
            return false;
        }
        if (!mtmd_support_vision(mtmd_)) {
            SetError(error, "所选模型不支持图像输入。");
            return false;
        }
        using_gpu_ = use_gpu;
        return true;
    }

    bool Generate(const RgbBuffer& image, const std::string& prompt, int max_tokens,
                  std::string* text, std::string* error) override {
        if (!image.Valid()) {
            SetError(error, "待识别图像为空。");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

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

            const auto evaluate = [&](llama_pos* n_past) {
                llama_memory_clear(llama_get_memory(ctx_), true);
                *n_past = 0;
                return EvalChunks(mtmd_, ctx_, chunks, n_past);
            };

            llama_pos n_past = 0;
            bool evaluated = evaluate(&n_past);
            if (!evaluated && using_gpu_) {
                // GPU 视觉编码失败 → 退回 CPU 重建 mtmd 再试一次（chunks 只含 token 序列，可复用）
                std::string retry_error;
                if (InitMtmd(false, &retry_error)) {
                    evaluated = evaluate(&n_past);
                }
            }
            if (!evaluated) {
                SetError(error, using_gpu_ ? "视觉编码/提示词推理失败（GPU 与 CPU 都失败，可能是显存不足或模型不匹配）。"
                                           : "提示词推理失败（上下文长度或显存不足）。");
                break;
            }

            llama_sampler_chain_params sampler_params = llama_sampler_chain_default_params();
            sampler_params.no_perf = true;
            llama_sampler* sampler = llama_sampler_chain_init(sampler_params);
            llama_sampler_chain_add(sampler, llama_sampler_init_greedy());   // 官方默认 do_sample=False

            std::string output;
            for (int step = 0; step < std::max(1, max_tokens); step++) {
                const llama_token token = llama_sampler_sample(sampler, ctx_, -1);
                if (llama_vocab_is_eog(vocab_, token)) break;

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
                if (rc != 0) break;
                n_past += 1;
            }

            llama_sampler_free(sampler);
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
