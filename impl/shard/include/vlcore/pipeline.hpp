// 两阶段编排：版面 → 逐块（本地 VLM 或远程回调）→ 按阅读顺序合并 Markdown。
// 与 impl/ocr_core/vl/pipeline.py + adapter.py 的职责一一对应。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vlcore/core.hpp"
#include "vlcore/engines.hpp"

namespace vlcore {

class Pipeline {
public:
    Pipeline(std::unique_ptr<LayoutEngine> layout, std::unique_ptr<VlmEngine> vlm);

    bool has_layout() const { return layout_ != nullptr; }
    bool has_vlm() const { return vlm_ != nullptr || remote_ != nullptr; }

    /// 平台侧注册远程识别回调（例如 llama.cpp server / mlx-vlm 的 OpenAI 兼容接口）。
    void set_remote(RemoteRecognizeFn fn, void* user);

    /// 只做版面分析（远程模式下由平台侧逐块回调，仍走本类编排）。
    bool Detect(const RgbBuffer& page, std::vector<Block>* blocks, std::string* error) const;

    /// 单块识别：本地 VLM 或远程回调，二者都没有时返回 false。
    bool Recognize(const RgbBuffer& block_image, const std::string& label,
                   std::string* text, std::string* error);

    /// 整页：版面 → 逐块识别 → Markdown。单块失败不拖垮整页（该块标记为未识别）。
    bool Run(const RgbBuffer& page, const std::function<void(int, int)>& progress,
             std::string* markdown, std::vector<Block>* blocks, double* seconds,
             std::string* error);

private:
    std::unique_ptr<LayoutEngine> layout_;
    std::unique_ptr<VlmEngine> vlm_;
    RemoteRecognizeFn remote_{nullptr};
    void* remote_user_{nullptr};
    LayoutParams params_;
};

}  // namespace vlcore
