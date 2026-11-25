// vlcore 纯逻辑实现：不依赖任何推理库，可在桌面直接编译与单测。
#include "vlcore/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vlcore {
namespace {

float Clampf(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

int Clampi(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

float IoU(const Rect& a, const Rect& b) {
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    if (x2 <= x1 || y2 <= y1) return 0.f;
    const float intersection = (x2 - x1) * (y2 - y1);
    const float uni = a.Area() + b.Area() - intersection;
    return uni > 0.f ? intersection / uni : 0.f;
}

/// 双线性采样；sx / sy 是源图像素坐标（0 起）。
void SampleBilinear(const RgbBuffer& src, float sx, float sy, uint8_t* rgb) {
    sx = Clampf(sx, 0.f, static_cast<float>(src.width - 1));
    sy = Clampf(sy, 0.f, static_cast<float>(src.height - 1));
    const int x0 = static_cast<int>(std::floor(sx));
    const int y0 = static_cast<int>(std::floor(sy));
    const int x1 = std::min(x0 + 1, src.width - 1);
    const int y1 = std::min(y0 + 1, src.height - 1);
    const float fx = sx - static_cast<float>(x0);
    const float fy = sy - static_cast<float>(y0);
    for (int c = 0; c < 3; c++) {
        const auto at = [&](int x, int y) {
            return static_cast<float>(src.pixels[(static_cast<size_t>(y) * src.width + x) * 3 + c]);
        };
        const float top = at(x0, y0) * (1.f - fx) + at(x1, y0) * fx;
        const float bottom = at(x0, y1) * (1.f - fx) + at(x1, y1) * fx;
        rgb[c] = static_cast<uint8_t>(Clampf(top * (1.f - fy) + bottom * fy + 0.5f, 0.f, 255.f));
    }
}

bool Contains(const char* const* items, size_t count, const std::string& value) {
    for (size_t i = 0; i < count; i++) {
        if (value == items[i]) return true;
    }
    return false;
}

std::string Trim(const std::string& text) {
    const auto is_space = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; };
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && is_space(static_cast<unsigned char>(text[begin]))) begin++;
    while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) end--;
    return text.substr(begin, end - begin);
}

std::string SingleLine(const std::string& text) {
    std::string out = text;
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

std::string Number(float value, int precision) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, static_cast<double>(value));
    return std::string(buffer);
}

}  // namespace

const std::array<const char*, 25> kLabels = {
    "abstract", "algorithm", "aside_text", "chart", "content",
    "display_formula", "doc_title", "figure_title", "footer", "footer_image",
    "footnote", "formula_number", "header", "header_image", "image",
    "inline_formula", "number", "paragraph_title", "reference", "reference_content",
    "seal", "table", "text", "vertical_text", "vision_footnote",
};

// ---------------------------------------------------------------- 图像

bool BuildLayoutInput(const RgbBuffer& image, LayoutInput* out) {
    if (!image.Valid() || out == nullptr) return false;

    const float raw_scale = static_cast<float>(kLayoutSide) / static_cast<float>(std::max(image.width, image.height));
    int draw_w = std::max(1, static_cast<int>(std::lround(image.width * raw_scale)));
    int draw_h = std::max(1, static_cast<int>(std::lround(image.height * raw_scale)));
    draw_w = std::min(draw_w, kLayoutSide);
    draw_h = std::min(draw_h, kLayoutSide);

    const size_t plane = static_cast<size_t>(kLayoutSide) * kLayoutSide;
    out->side = kLayoutSide;
    out->scale = static_cast<float>(draw_w) / static_cast<float>(image.width);
    out->chw.assign(plane * 3, 0.f);

    // 先把整张画布填成「补边灰」的归一化值，未覆盖区域即为右下补边。
    for (int c = 0; c < 3; c++) {
        const float pad = (static_cast<float>(kLayoutPad) / 255.f - kLayoutMean[c]) / kLayoutStd[c];
        std::fill(out->chw.begin() + static_cast<long>(c * plane),
                  out->chw.begin() + static_cast<long>((c + 1) * plane), pad);
    }

    for (int y = 0; y < draw_h; y++) {
        const float sy = (y + 0.5f) * static_cast<float>(image.height) / static_cast<float>(draw_h) - 0.5f;
        for (int x = 0; x < draw_w; x++) {
            const float sx = (x + 0.5f) * static_cast<float>(image.width) / static_cast<float>(draw_w) - 0.5f;
            uint8_t rgb[3] = {0, 0, 0};
            SampleBilinear(image, sx, sy, rgb);
            const size_t index = static_cast<size_t>(y) * kLayoutSide + x;
            for (int c = 0; c < 3; c++) {
                out->chw[c * plane + index] =
                    (static_cast<float>(rgb[c]) / 255.f - kLayoutMean[c]) / kLayoutStd[c];
            }
        }
    }
    return true;
}

RgbBuffer CropRgb(const RgbBuffer& image, const Rect& box, float expand_ratio) {
    RgbBuffer out;
    if (!image.Valid()) return out;

    const float dx = std::max(1.f, box.Width() * expand_ratio);
    const float dy = std::max(1.f, box.Height() * expand_ratio);
    const int x1 = Clampi(static_cast<int>(std::floor(box.x1 - dx)), 0, image.width);
    const int y1 = Clampi(static_cast<int>(std::floor(box.y1 - dy)), 0, image.height);
    const int x2 = Clampi(static_cast<int>(std::ceil(box.x2 + dx)), 0, image.width);
    const int y2 = Clampi(static_cast<int>(std::ceil(box.y2 + dy)), 0, image.height);
    const int width = x2 - x1;
    const int height = y2 - y1;
    if (width < 4 || height < 4) return out;

    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; y++) {
        const uint8_t* source = image.pixels.data() +
            (static_cast<size_t>(y1 + y) * image.width + x1) * 3;
        std::copy_n(source, static_cast<size_t>(width) * 3,
                    out.pixels.data() + static_cast<size_t>(y) * width * 3);
    }
    return out;
}

RgbBuffer LimitPixels(const RgbBuffer& image, int max_pixels) {
    if (!image.Valid()) return RgbBuffer{};
    const long long total = static_cast<long long>(image.width) * image.height;
    if (total <= max_pixels) return image;

    const double factor = std::sqrt(static_cast<double>(max_pixels) / static_cast<double>(total));
    const int width = std::max(1, static_cast<int>(std::lround(image.width * factor)));
    const int height = std::max(1, static_cast<int>(std::lround(image.height * factor)));

    RgbBuffer out;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; y++) {
        const float sy = (y + 0.5f) * static_cast<float>(image.height) / static_cast<float>(height) - 0.5f;
        for (int x = 0; x < width; x++) {
            const float sx = (x + 0.5f) * static_cast<float>(image.width) / static_cast<float>(width) - 0.5f;
            SampleBilinear(image, sx, sy, out.pixels.data() + (static_cast<size_t>(y) * width + x) * 3);
        }
    }
    return out;
}

// ---------------------------------------------------------------- 版面后处理

std::vector<Block> DecodeLayout(const float* rows, int row_count, float scale,
                                int image_width, int image_height, const LayoutParams& params) {
    std::vector<Block> result;
    if (rows == nullptr || row_count <= 0 || scale <= 0.f) return result;

    const float page_area = static_cast<float>(image_width) * static_cast<float>(image_height);
    const bool portrait = image_height >= image_width;
    const float pseudo_limit = portrait ? params.pseudo_page_portrait : params.pseudo_page_landscape;

    struct Candidate {
        std::string label;
        float score{0};
        Rect box;
        float order_key{0};
        int index{0};
    };

    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<size_t>(row_count));
    for (int i = 0; i < row_count; i++) {
        const float* row = rows + static_cast<size_t>(i) * 7;
        const float score = row[1];
        if (score < params.score_threshold) continue;

        int class_id = static_cast<int>(std::lround(row[0]));
        if (class_id < 0 || class_id >= static_cast<int>(kLabels.size())) class_id = 22;  // text

        Rect box;
        box.x1 = Clampf(std::min(row[2], row[4]) / scale, 0.f, static_cast<float>(image_width));
        box.y1 = Clampf(std::min(row[3], row[5]) / scale, 0.f, static_cast<float>(image_height));
        box.x2 = Clampf(std::max(row[2], row[4]) / scale, 0.f, static_cast<float>(image_width));
        box.y2 = Clampf(std::max(row[3], row[5]) / scale, 0.f, static_cast<float>(image_height));
        if (box.Width() < 2.f || box.Height() < 2.f) continue;

        const std::string label = kLabels[class_id];
        // 整页被误判成一张插图时丢弃，否则后续裁块会得到整页图。
        if (label == "image" && page_area > 0.f && box.Area() / page_area > pseudo_limit) continue;

        candidates.push_back(Candidate{label, score, box, row[6], i});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.index < b.index;
    });

    std::vector<Candidate> kept;
    for (const Candidate& candidate : candidates) {
        if (static_cast<int>(kept.size()) >= params.max_detections) break;
        bool blocked = false;
        for (const Candidate& other : kept) {
            const float threshold = other.label == candidate.label ? params.same_class_iou
                                                                   : params.cross_class_iou;
            if (IoU(other.box, candidate.box) > threshold) {
                blocked = true;
                break;
            }
        }
        if (!blocked) kept.push_back(candidate);
    }

    std::vector<int> orderable;
    orderable.reserve(kept.size());
    for (int i = 0; i < static_cast<int>(kept.size()); i++) {
        if (!IsSkipOrder(kept[static_cast<size_t>(i)].label)) orderable.push_back(i);
    }
    std::sort(orderable.begin(), orderable.end(), [&](int a, int b) {
        const Candidate& left = kept[static_cast<size_t>(a)];
        const Candidate& right = kept[static_cast<size_t>(b)];
        if (left.order_key != right.order_key) return left.order_key < right.order_key;
        if (left.score != right.score) return left.score > right.score;
        return a < b;
    });

    std::vector<int> rank(kept.size(), -1);
    for (int i = 0; i < static_cast<int>(orderable.size()); i++) {
        rank[static_cast<size_t>(orderable[static_cast<size_t>(i)])] = i + 1;
    }

    result.reserve(kept.size());
    for (int i = 0; i < static_cast<int>(kept.size()); i++) {
        Block block;
        block.label = kept[static_cast<size_t>(i)].label;
        block.box = kept[static_cast<size_t>(i)].box;
        block.score = kept[static_cast<size_t>(i)].score;
        block.order = rank[static_cast<size_t>(i)];
        result.push_back(std::move(block));
    }

    std::stable_sort(result.begin(), result.end(), [](const Block& a, const Block& b) {
        if (a.order > 0 && b.order > 0) return a.order < b.order;
        if (a.order > 0) return true;
        if (b.order > 0) return false;
        if (a.box.y1 != b.box.y1) return a.box.y1 < b.box.y1;
        return a.box.x1 < b.box.x1;
    });
    return result;
}

// ---------------------------------------------------------------- 标签规则

bool IsSkipOrder(const std::string& label) {
    static const char* kSkip[] = {
        "figure_title", "vision_footnote", "image", "chart", "table",
        "header", "header_image", "footer", "footer_image", "footnote", "aside_text",
    };
    return Contains(kSkip, sizeof(kSkip) / sizeof(kSkip[0]), label);
}

bool IsMarkdownIgnored(const std::string& label) {
    static const char* kIgnore[] = {
        "number", "footnote", "header", "header_image", "footer", "footer_image", "aside_text",
    };
    return Contains(kIgnore, sizeof(kIgnore) / sizeof(kIgnore[0]), label);
}

bool ShouldRecognize(const std::string& label) {
    // 官方 IMAGE_LABELS 不送 VLM；chart / seal 在本预设下关闭识别，退化为插图。
    if (label == "image" || label == "header_image" || label == "footer_image") return false;
    if (label == "chart" && !Preset::kChartRecognition) return false;
    if (label == "seal" && !Preset::kSealRecognition) return false;
    return true;
}

std::string PromptFor(const std::string& label) {
    if (label == "table") return "Table Recognition:";
    if (label == "chart" && Preset::kChartRecognition) return "Chart Recognition:";
    if (label == "seal" && Preset::kSealRecognition) return "Seal Recognition:";
    if (label == "display_formula" || label == "inline_formula") return "Formula Recognition:";
    return "OCR:";
}

std::string RegionType(const std::string& label) {
    if (label == "doc_title") return "heading";
    if (label == "paragraph_title") return "subheading";
    if (label == "header") return "header";
    if (label == "footer") return "footer";
    if (label == "footnote" || label == "vision_footnote") return "footnote";
    if (label == "number") return "page_number";
    if (label == "image" || label == "chart" || label == "header_image" || label == "footer_image") return "image";
    if (label == "figure_title") return "caption";
    if (label == "table") return "table";
    if (label == "seal") return "stamp";
    if (label == "aside_text") return "ignore";
    return "text";
}

// ---------------------------------------------------------------- 输出

std::string RenderMarkdown(const std::vector<Block>& blocks) {
    std::string out;
    for (const Block& block : blocks) {
        if (IsMarkdownIgnored(block.label)) continue;

        if (!block.recognized) {
            if (!out.empty()) out += "\n\n";
            out += "![](block-" + std::to_string(block.order > 0 ? block.order : 0) + ")";
            continue;
        }

        const std::string text = Trim(block.text);
        if (text.empty()) continue;
        if (!out.empty()) out += "\n\n";
        if (block.label == "doc_title") {
            out += "# " + SingleLine(text);
        } else if (block.label == "paragraph_title") {
            out += "## " + SingleLine(text);
        } else {
            // 表格（OTSL 原文）、公式（LaTeX）、正文原样保留，禁止折叠换行
            out += text;
        }
    }
    return out;
}

std::string EscapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                    out += buffer;
                } else {
                    out += static_cast<char>(ch);   // UTF-8 原样透传
                }
        }
    }
    return out;
}

std::string BlocksToJson(const std::vector<Block>& blocks) {
    std::string out = "[";
    for (size_t i = 0; i < blocks.size(); i++) {
        const Block& block = blocks[i];
        if (i > 0) out += ",";
        out += "{\"label\":\"" + EscapeJson(block.label) + "\"";
        out += ",\"order\":" + std::to_string(block.order);
        out += ",\"score\":" + Number(block.score, 4);
        out += ",\"bbox\":[" + Number(block.box.x1, 1) + "," + Number(block.box.y1, 1) + "," +
               Number(block.box.x2, 1) + "," + Number(block.box.y2, 1) + "]";
        out += ",\"text\":\"" + EscapeJson(block.text) + "\"";
        out += ",\"recognized\":";
        out += block.recognized ? "true" : "false";
        out += "}";
    }
    out += "]";
    return out;
}

}  // namespace vlcore
