// 共享核心的桌面单测：不需要任何模型，验证标签规则、版面后处理与 Markdown 合并。
//   cmake -S impl/shard -B build-shard && cmake --build build-shard && ctest --test-dir build-shard
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "vlcore/core.hpp"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message.c_str());
        g_failures++;
    }
}

void ExpectNear(float actual, float expected, float tolerance, const std::string& message) {
    Expect(std::fabs(actual - expected) <= tolerance,
           message + "（actual=" + std::to_string(actual) + " expected=" + std::to_string(expected) + "）");
}

vlcore::RgbBuffer MakeImage(int width, int height, uint8_t value) {
    vlcore::RgbBuffer image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<size_t>(width) * height * 3, value);
    return image;
}

void TestLabelTable() {
    Expect(vlcore::kLabels.size() == 25, "标签表必须是 25 类");
    Expect(std::string(vlcore::kLabels[0]) == "abstract", "class_id 0 = abstract");
    Expect(std::string(vlcore::kLabels[6]) == "doc_title", "class_id 6 = doc_title");
    Expect(std::string(vlcore::kLabels[22]) == "text", "class_id 22 = text");
    Expect(std::string(vlcore::kLabels[24]) == "vision_footnote", "class_id 24 = vision_footnote");
}

void TestLabelRules() {
    // 官方 SKIP_ORDER_LABELS：11 类
    int skip_count = 0;
    for (const char* label : vlcore::kLabels) {
        if (vlcore::IsSkipOrder(label)) skip_count++;
    }
    Expect(skip_count == 11, "跳过排序的标签必须正好 11 个");

    Expect(vlcore::IsSkipOrder("table"), "table 不参与阅读顺序编号");
    Expect(!vlcore::IsSkipOrder("text"), "text 参与阅读顺序编号");
    Expect(vlcore::IsMarkdownIgnored("header"), "header 不进 Markdown");
    Expect(!vlcore::IsMarkdownIgnored("paragraph_title"), "paragraph_title 进 Markdown");

    // 纯插图不送 VLM；chart / seal 在本预设下关闭识别
    Expect(!vlcore::ShouldRecognize("image"), "image 不送 VLM");
    Expect(!vlcore::ShouldRecognize("header_image"), "header_image 不送 VLM");
    Expect(!vlcore::ShouldRecognize("chart"), "chart 在本预设下不送 VLM");
    Expect(vlcore::ShouldRecognize("table"), "table 要送 VLM");

    Expect(vlcore::PromptFor("table") == "Table Recognition:", "table 提示词");
    Expect(vlcore::PromptFor("display_formula") == "Formula Recognition:", "公式提示词");
    Expect(vlcore::PromptFor("formula_number") == "OCR:", "formula_number 走 OCR 提示词");
    Expect(vlcore::PromptFor("text") == "OCR:", "正文提示词");

    Expect(vlcore::RegionType("doc_title") == "heading", "doc_title → heading");
    Expect(vlcore::RegionType("image") == "image", "image → image");
    Expect(vlcore::RegionType("aside_text") == "ignore", "aside_text → ignore");
}

void TestLayoutInput() {
    const vlcore::RgbBuffer image = MakeImage(400, 200, 255);
    vlcore::LayoutInput input;
    Expect(vlcore::BuildLayoutInput(image, &input), "版面预处理应成功");
    Expect(input.side == 800, "输入边长固定 800");
    Expect(input.chw.size() == static_cast<size_t>(800) * 800 * 3, "CHW 元素数");

    // 官方 norm_type=none → 只除以 255，白色像素应为 1.0（不做 mean/std）
    ExpectNear(input.chw[0], 1.f, 1e-3f, "白色像素只做 /255");

    // keep_ratio=false → 整张画布都是内容，右下角同样是白（不是补边灰）
    const size_t corner_index = static_cast<size_t>(799) * 800 + 799;
    ExpectNear(input.chw[corner_index], 1.f, 1e-3f, "右下角无补边");

    // 横竖各自拉伸：400x200 的图，y=799 映射回原图 y≈199、x=799 → x≈399，仍是白
    const size_t bottom_left = static_cast<size_t>(799) * 800;
    ExpectNear(input.chw[bottom_left], 1.f, 1e-3f, "底部一行仍是内容");

    // 拉伸而不是 letterbox：左半黑、右半白时，x=399/400 必须是黑白分界（letterbox 会把内容挤到左上）
    vlcore::RgbBuffer split = MakeImage(800, 400, 0);
    for (int y = 0; y < split.height; y++) {
        for (int x = 400; x < split.width; x++) {
            const size_t index = (static_cast<size_t>(y) * split.width + x) * 3;
            split.pixels[index] = split.pixels[index + 1] = split.pixels[index + 2] = 255;
        }
    }
    vlcore::LayoutInput stretched;
    Expect(vlcore::BuildLayoutInput(split, &stretched), "黑右白图应预处理成功");
    const size_t plane = static_cast<size_t>(800) * 800;
    ExpectNear(stretched.chw[static_cast<size_t>(400) * 800 + 399], 0.f, 1e-3f, "分界线左侧为黑");
    ExpectNear(stretched.chw[static_cast<size_t>(400) * 800 + 400], 1.f, 1e-3f, "分界线右侧为白");
    ExpectNear(stretched.chw[plane + static_cast<size_t>(799) * 800 + 799], 1.f, 1e-3f, "G 通道右下为白");
}

void TestCrop() {
    const vlcore::RgbBuffer image = MakeImage(100, 80, 10);
    const vlcore::Rect box{20.f, 10.f, 60.f, 50.f};
    const vlcore::RgbBuffer crop = vlcore::CropRgb(image, box);
    Expect(crop.Valid(), "裁块结果有效");
    Expect(crop.width >= 40 && crop.width <= 44, "裁块宽度含少量外扩");
    Expect(crop.height >= 40 && crop.height <= 44, "裁块高度含少量外扩");

    const vlcore::RgbBuffer outside = vlcore::CropRgb(image, vlcore::Rect{200.f, 200.f, 260.f, 260.f});
    Expect(!outside.Valid(), "完全越界的框返回空");
}

void TestDecodeLayout() {
    // 模型输出坐标已是原图像素（800x800 页面）
    std::vector<float> rows = {
        // doc_title，order_key=1
        6.f, 0.90f, 100.f, 50.f, 700.f, 120.f, 1.f,
        // 正文，order_key=2
        22.f, 0.80f, 100.f, 150.f, 700.f, 400.f, 2.f,
        // 与上一块同类的重复框 → NMS 丢弃
        22.f, 0.70f, 102.f, 152.f, 698.f, 398.f, 2.f,
        // 整页 image 伪框 → 丢弃
        14.f, 0.95f, 0.f, 0.f, 800.f, 800.f, 0.f,
        // 页脚，跳过排序
        8.f, 0.60f, 100.f, 700.f, 700.f, 780.f, 0.f,
        // 低分 → 阈值丢弃
        22.f, 0.10f, 100.f, 500.f, 700.f, 600.f, 3.f,
    };
    const vlcore::LayoutParams params;
    const std::vector<vlcore::Block> blocks =
        vlcore::DecodeLayout(rows.data(), static_cast<int>(rows.size() / 7), 800, 800, params);

    Expect(blocks.size() == 3, "NMS/阈值/伪框过滤后剩 3 块");
    if (blocks.size() == 3) {
        Expect(blocks[0].label == "doc_title" && blocks[0].order == 1, "第一块是标题 order=1");
        Expect(blocks[1].label == "text" && blocks[1].order == 2, "第二块是正文 order=2");
        Expect(blocks[2].label == "footer" && blocks[2].order == -1, "页脚不编号且垫后");
        ExpectNear(blocks[0].box.x1, 100.f, 0.5f, "坐标原样保留（不再除缩放比）");
    }
}

/// 官方 filter_boxes：行内公式被正文框住要去掉，碎框与 reference 也要去掉。
void TestOverlapFilter() {
    std::vector<float> rows = {
        // 正文块（order_key=1）
        22.f, 0.90f, 100.f, 100.f, 700.f, 200.f, 1.f,
        // 行内公式，完全嵌在正文里（小框重叠率=1.0）→ 必须丢弃
        15.f, 0.85f, 300.f, 120.f, 360.f, 150.f, 2.f,
        // 另一段正文（不重叠）
        22.f, 0.80f, 100.f, 300.f, 400.f, 400.f, 3.f,
        // 碎框（高 3px）→ 丢弃
        22.f, 0.70f, 500.f, 500.f, 600.f, 503.f, 4.f,
        // reference 区 → 丢弃（与官方一致）
        18.f, 0.65f, 100.f, 600.f, 700.f, 700.f, 5.f,
    };
    const vlcore::LayoutParams params;
    const std::vector<vlcore::Block> blocks =
        vlcore::DecodeLayout(rows.data(), static_cast<int>(rows.size() / 7), 800, 800, params);

    Expect(blocks.size() == 2, "嵌套行内公式/碎框/reference 都被过滤，只剩 2 段正文");
    if (blocks.size() == 2) {
        Expect(blocks[0].label == "text" && blocks[0].order == 1, "第一段正文 order=1");
        Expect(blocks[1].label == "text" && blocks[1].order == 2, "第二段正文 order=2");
    }
}

void TestMarkdown() {
    std::vector<vlcore::Block> blocks;
    {
        vlcore::Block block;
        block.label = "doc_title";
        block.text = "  文献标题  ";
        block.recognized = true;
        block.order = 1;
        blocks.push_back(block);
    }
    {
        vlcore::Block block;
        block.label = "text";
        block.text = "第一段\n第二段";
        block.recognized = true;
        block.order = 2;
        blocks.push_back(block);
    }
    {
        vlcore::Block block;
        block.label = "footer";
        block.text = "页脚";
        block.recognized = true;
        block.order = -1;
        blocks.push_back(block);
    }
    {
        vlcore::Block block;
        block.label = "image";
        block.order = 3;
        block.recognized = false;
        blocks.push_back(block);
    }

    const std::string markdown = vlcore::RenderMarkdown(blocks);
    Expect(markdown.find("# 文献标题") != std::string::npos, "标题加 # 前缀并去空白");
    Expect(markdown.find("第一段\n第二段") != std::string::npos, "正文保留换行");
    Expect(markdown.find("页脚") == std::string::npos, "页脚不进 Markdown");
    Expect(markdown.find("![](block-3)") != std::string::npos, "插图输出占位");
}

void TestJson() {
    std::vector<vlcore::Block> blocks;
    vlcore::Block block;
    block.label = "text";
    block.text = "引号\"与换行\n";
    block.order = 1;
    block.score = 0.8765f;
    block.recognized = true;
    blocks.push_back(block);

    const std::string json = vlcore::BlocksToJson(blocks);
    Expect(json.find("\\\"与换行\\n") != std::string::npos, "JSON 转义引号与换行");
    Expect(json.find("\"order\":1") != std::string::npos, "JSON 含 order");
    Expect(vlcore::EscapeJson("\t") == "\\t", "EscapeJson 制表符");
}

struct LogCapture {
    std::vector<int> levels;
    std::vector<std::string> lines;
    int reentry = 0;
};

void CaptureSink(int level, const char* message, void* user) {
    auto* capture = static_cast<LogCapture*>(user);
    if (capture == nullptr || message == nullptr) return;
    capture->levels.push_back(level);
    capture->lines.push_back(message);
    // sink 内部再打一行日志：验证 LogLine 不会在持锁状态下回调（否则这里就死锁了）。
    if (capture->reentry < 1) {
        capture->reentry += 1;
        vlcore::LogLine(vlcore::kLogDebug, "重入");
    }
}

void TestLogSink() {
    Expect(!vlcore::LogActive(), "默认没有 sink");

    LogCapture capture;
    vlcore::SetLogSink(CaptureSink, &capture);
    Expect(vlcore::LogActive(), "安装后 sink 生效");

    vlcore::LogLine(vlcore::kLogWarn, "第一行");
    Expect(capture.lines.size() == 2, "sink 收到日志（含一次重入）");
    Expect(!capture.levels.empty() && capture.levels.front() == vlcore::kLogWarn,
           "级别按原值透传");
    Expect(!capture.lines.empty() && capture.lines.front() == "第一行", "正文原样透传");

    vlcore::SetLogSink(nullptr, nullptr);
    Expect(!vlcore::LogActive(), "卸载后 sink 失效");
    const size_t before = capture.lines.size();
    vlcore::LogLine(vlcore::kLogError, "卸载后的日志");
    Expect(capture.lines.size() == before, "卸载后不再回调");
}

}  // namespace

int main() {
    TestLabelTable();
    TestLabelRules();
    TestLayoutInput();
    TestCrop();
    TestDecodeLayout();
    TestOverlapFilter();
    TestMarkdown();
    TestJson();
    TestLogSink();

    if (g_failures == 0) {
        std::printf("PASS: 全部共享核心用例通过\n");
        return 0;
    }
    std::printf("FAILED: %d 个断言失败\n", g_failures);
    return 1;
}
