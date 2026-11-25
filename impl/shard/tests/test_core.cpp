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
    Expect(vlcore::BuildLayoutInput(image, &input), "letterbox 应成功");
    Expect(input.side == 800, "输入边长固定 800");
    ExpectNear(input.scale, 2.0f, 1e-4f, "400 宽放大到 800 → scale=2");
    Expect(input.chw.size() == static_cast<size_t>(800) * 800 * 3, "CHW 元素数");

    // 白色像素归一化后应为 (1 - mean) / std
    const float expected = (1.f - 0.8286f) / 0.1889f;
    ExpectNear(input.chw[0], expected, 1e-3f, "白色像素归一化");

    // 补边区域（右下）应为补边灰
    const size_t pad_index = static_cast<size_t>(799) * 800 + 799;
    const float pad_expected = (114.f / 255.f - 0.8286f) / 0.1889f;
    ExpectNear(input.chw[pad_index], pad_expected, 1e-3f, "右下补边为 114 灰");
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
    // 800x800 竖版页面，scale=1 → 输出坐标即原图坐标
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
        vlcore::DecodeLayout(rows.data(), static_cast<int>(rows.size() / 7), 1.f, 800, 800, params);

    Expect(blocks.size() == 3, "NMS/阈值/伪框过滤后剩 3 块");
    if (blocks.size() == 3) {
        Expect(blocks[0].label == "doc_title" && blocks[0].order == 1, "第一块是标题 order=1");
        Expect(blocks[1].label == "text" && blocks[1].order == 2, "第二块是正文 order=2");
        Expect(blocks[2].label == "footer" && blocks[2].order == -1, "页脚不编号且垫后");
        ExpectNear(blocks[0].box.x1, 100.f, 0.5f, "坐标按 scale 还原");
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

}  // namespace

int main() {
    TestLabelTable();
    TestLabelRules();
    TestLayoutInput();
    TestCrop();
    TestDecodeLayout();
    TestMarkdown();
    TestJson();

    if (g_failures == 0) {
        std::printf("PASS: 全部共享核心用例通过\n");
        return 0;
    }
    std::printf("FAILED: %d 个断言失败\n", g_failures);
    return 1;
}
