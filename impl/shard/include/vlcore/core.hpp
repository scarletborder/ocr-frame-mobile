// vlcore：iOS / Android 共用的 VL 线纯逻辑核心。
//
// 唯一真相在这里，平台侧不允许再复制一份：
//   · 25 类版面标签字典与「跳过排序 / 忽略 Markdown / 纯插图」集合
//   · 官方提示词（OCR: / Table Recognition: / Formula Recognition: …）
//   · letterbox + 归一化、裁块、缩放
//   · 检测后处理：反算坐标 → 阈值 → NMS → 整页伪框过滤 → 阅读顺序编号
//   · Markdown 合并规则
// 与 impl/ocr_core/vl/{blocks,markdown,options}.py 保持同源，改这里必须同步改 Python 侧。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vlcore {

/// 预设：对应 impl/proto/preset.proto::VlConfig 默认值（impl/ocr_core/vl/options.py）。
struct Preset {
    static constexpr const char* kName = "VL 线通用文档解析";
    static constexpr const char* kVersion = "1.0";
    static constexpr const char* kPipelineVersion = "v1.6";
    static constexpr const char* kShapeMode = "auto";
    static constexpr int kMaxNewTokens = 2048;
    static constexpr bool kChartRecognition = false;
    static constexpr bool kSealRecognition = false;
};

/// PP-DocLayoutV3 的 25 类标签，下标即模型 class_id（0…24）。
extern const std::array<const char*, 25> kLabels;

/// 模型输入边长与 letterbox 补边灰度（PaddleX 约定）。
constexpr int kLayoutSide = 800;
constexpr uint8_t kLayoutPad = 114;
constexpr float kLayoutMean[3] = {0.8286f, 0.8281f, 0.8282f};
constexpr float kLayoutStd[3] = {0.1889f, 0.1889f, 0.1889f};
/// 单块视觉 token 对应的像素预算上限（官方 max_pixels=1003520）。
constexpr int kMaxBlockPixels = 1003520;

struct Rect {
    float x1{0};
    float y1{0};
    float x2{0};
    float y2{0};

    float Width() const { return x2 > x1 ? x2 - x1 : 0; }
    float Height() const { return y2 > y1 ? y2 - y1 : 0; }
    float Area() const { return Width() * Height(); }
};

struct Detection {
    int class_id{0};
    float score{0};
    Rect box;
    float order_key{0};
};

struct Block {
    std::string label;
    Rect box;
    int order{-1};
    float score{0};
    std::string text;
    bool recognized{false};
};

/// 紧排 RGB（width*height*3），平台侧解码后交给核心。
struct RgbBuffer {
    std::vector<uint8_t> pixels;
    int width{0};
    int height{0};

    bool Valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    }
};

/// 版面模型输入：CHW 浮点张量 + letterbox 缩放比。
struct LayoutInput {
    std::vector<float> chw;
    int side{0};
    float scale{1};
};

struct LayoutParams {
    float score_threshold{0.4f};
    float same_class_iou{0.6f};
    float cross_class_iou{0.98f};
    int max_detections{100};
    /// 整页 image 伪框过滤：横版 / 竖版面积占比上限。
    float pseudo_page_landscape{0.82f};
    float pseudo_page_portrait{0.93f};
};

// ---- 图像 ----

/// letterbox 到 800x800（右下补 114）并归一化成 CHW。补边在右下，反算只需除以 scale。
bool BuildLayoutInput(const RgbBuffer& image, LayoutInput* out);

/// 按版面框裁块，外扩 expand_ratio 避免裁掉字形边缘；越界自动裁剪。
RgbBuffer CropRgb(const RgbBuffer& image, const Rect& box, float expand_ratio = 0.005f);

/// 超过 max_pixels 时等比缩小（送入视觉编码器前的护栏）。
RgbBuffer LimitPixels(const RgbBuffer& image, int max_pixels = kMaxBlockPixels);

// ---- 版面后处理 ----

/// rows 为模型输出 [N,7] = [class_id, score, x1, y1, x2, y2, order_key]，
/// 坐标在 letterbox 空间；返回按阅读顺序排好的块（未编号的垫后）。
std::vector<Block> DecodeLayout(const float* rows, int row_count, float scale,
                                int image_width, int image_height, const LayoutParams& params);

// ---- 标签规则（与 ocr_core/vl/blocks.py 同源）----

bool IsSkipOrder(const std::string& label);
bool IsMarkdownIgnored(const std::string& label);
bool ShouldRecognize(const std::string& label);
std::string PromptFor(const std::string& label);
std::string RegionType(const std::string& label);

// ---- 输出 ----

std::string RenderMarkdown(const std::vector<Block>& blocks);
std::string BlocksToJson(const std::vector<Block>& blocks);
std::string EscapeJson(const std::string& text);

}  // namespace vlcore
