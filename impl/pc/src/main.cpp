// vlcore_pc —— 在桌面上跑与手机完全相同的 VL 线（同一份 impl/shard）。
//
// 用途：手机上看不到的东西（每块的失败原因、裁块尺寸、Markdown 合并结果）在这里一次看清。
//
// 用法：
//   vlcore_pc --image page.png --layout PP-DocLayoutV3.onnx \
//             --vlm PaddleOCR-VL-1.6-GGUF.gguf --mmproj ...-mmproj.gguf
//   vlcore_pc --image page.png --layout PP-DocLayoutV3.onnx --stub-vlm    # 只验版面/顺序/Markdown
//   vlcore_pc --image page.png --layout PP-DocLayoutV3.onnx               # 只做版面
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vlcore/vlcore_c.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

struct Options {
    std::string image;
    std::string layout;
    std::string vlm;
    std::string mmproj;
    std::string out_markdown;
    std::string out_json;
    bool stub_vlm = false;
    bool verbose = false;
    int threads = 4;
    int n_ctx = 8192;
};

void Usage() {
    std::printf(
        "用法: vlcore_pc --image <图片> --layout <版面.onnx> [选项]\n"
        "\n"
        "  --vlm <权重.gguf>      视觉语言模型主权重（不传则只做版面）\n"
        "  --mmproj <投影器.gguf> 视觉投影器，与 --vlm 成对\n"
        "  --stub-vlm             用假 VLM 替身：验证裁块 / 提示词 / Markdown 通路（不需要模型）\n"
        "  --out <file.md>        把整页 Markdown 写文件\n"
        "  --json <file.json>     把 vlcore 原始 JSON 写文件\n"
        "  --threads <n>          推理线程数（默认 4）\n"
        "  --n-ctx <n>            上下文长度（默认 8192）\n"
        "  -v, --verbose          额外打印整页 Markdown\n");
}

/// 假 VLM：返回能看出「提示词 + 裁块尺寸」的文本，用于验证不依赖模型的链路。
char* StubRemote(const uint8_t* /*rgb*/, int width, int height, const char* prompt,
                 void* /*user*/, char** /*error*/) {
    std::string text = "【stub】";
    text += prompt != nullptr ? prompt : "";
    text += " " + std::to_string(width) + "x" + std::to_string(height);
    return strdup(text.c_str());
}

bool ParseArgs(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 缺少参数值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--image") options->image = next("--image");
        else if (arg == "--layout") options->layout = next("--layout");
        else if (arg == "--vlm") options->vlm = next("--vlm");
        else if (arg == "--mmproj") options->mmproj = next("--mmproj");
        else if (arg == "--out") options->out_markdown = next("--out");
        else if (arg == "--json") options->out_json = next("--json");
        else if (arg == "--threads") options->threads = std::atoi(next("--threads"));
        else if (arg == "--n-ctx") options->n_ctx = std::atoi(next("--n-ctx"));
        else if (arg == "--stub-vlm") options->stub_vlm = true;
        else if (arg == "-v" || arg == "--verbose") options->verbose = true;
        else if (arg == "-h" || arg == "--help") { Usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "未知参数：%s\n", arg.c_str());
            Usage();
            return false;
        }
    }
    if (options->image.empty() || options->layout.empty()) {
        Usage();
        return false;
    }
    return true;
}

/// 极简 JSON 取值：只解析 vlcore 自己产出的结构（字符串已转义）。
std::string JsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string out;
    for (; pos < json.size(); pos++) {
        const char ch = json[pos];
        if (ch == '"') break;
        if (ch == '\\' && pos + 1 < json.size()) {
            const char esc = json[++pos];
            switch (esc) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': pos += 4; out += '?'; break;
                default: out += esc;
            }
            continue;
        }
        out += ch;
    }
    return out;
}

void PrintProgress(int done, int total, void* /*user*/) {
    std::printf("\r  识别进度 %d/%d", done, total);
    std::fflush(stdout);
    if (done == total) std::printf("\n");
}

/// 把 "blocks":[…] 切成一个个块对象（每块都以 {"label":" 开头）。
std::vector<std::string> SplitBlocks(const std::string& payload) {
    std::vector<std::string> items;
    const size_t begin = payload.find("\"blocks\":[");
    if (begin == std::string::npos) return items;
    const size_t end = payload.find("],\"seconds\"");
    if (end == std::string::npos || end <= begin) return items;
    const std::string array = payload.substr(begin, end - begin);

    const std::string marker = "{\"label\":\"";
    std::vector<size_t> starts;
    for (size_t pos = array.find(marker); pos != std::string::npos; pos = array.find(marker, pos + 1)) {
        starts.push_back(pos);
    }
    for (size_t i = 0; i < starts.size(); i++) {
        const size_t from = starts[i];
        const size_t to = (i + 1 < starts.size()) ? starts[i + 1] : array.size();
        items.push_back(array.substr(from, to - from));
    }
    return items;
}

bool WriteFile(const std::string& path, const std::string& content) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseArgs(argc, argv, &options)) return 2;

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(options.image.c_str(), &width, &height, &channels, 3);
    if (pixels == nullptr) {
        std::fprintf(stderr, "图片读取失败：%s（%s）\n", options.image.c_str(), stbi_failure_reason());
        return 1;
    }
    std::printf("==> 图片 %s  %dx%d（原始 %d 通道）\n", options.image.c_str(), width, height, channels);

    char* error = nullptr;
    const bool use_vlm = !options.vlm.empty() && !options.stub_vlm;
    vlcore_engine_t* engine = vlcore_create(
        options.layout.c_str(),
        use_vlm ? options.vlm.c_str() : nullptr,
        (use_vlm && !options.mmproj.empty()) ? options.mmproj.c_str() : nullptr,
        options.n_ctx, options.threads, &error);
    if (engine == nullptr) {
        std::fprintf(stderr, "!! 引擎初始化失败：%s\n", error != nullptr ? error : "未知错误");
        if (error != nullptr) vlcore_free_string(error);
        stbi_image_free(pixels);
        return 1;
    }

    std::printf("==> 引擎：版面=%s  VLM=%s  预设=%s %s（产线 %s）\n",
                vlcore_layout_engine_built() ? "已编译" : "未编译",
                vlcore_vlm_engine_built() ? "已编译" : "未编译",
                vlcore_preset_name(), vlcore_preset_version(), vlcore_pipeline_version());
    if (options.stub_vlm) {
        vlcore_set_remote(engine, StubRemote, nullptr);
        std::printf("==> 使用假 VLM 替身（只验证版面 / 提示词 / Markdown 通路）\n");
    }

    const auto started = std::chrono::steady_clock::now();
    char* json = vlcore_run(engine, pixels, width, height, PrintProgress, nullptr, &error);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    stbi_image_free(pixels);

    if (json == nullptr) {
        std::fprintf(stderr, "!! 识别失败：%s\n", error != nullptr ? error : "未知错误");
        if (error != nullptr) vlcore_free_string(error);
        vlcore_destroy(engine);
        return 1;
    }

    const std::string payload(json);
    vlcore_free_string(json);
    const std::string markdown = JsonString(payload, "markdown");

    std::printf("==> 完成，用时 %.2fs，Markdown %zu 字符\n", elapsed, markdown.size());

    const std::vector<std::string> blocks = SplitBlocks(payload);
    int failed = 0;
    for (size_t i = 0; i < blocks.size(); i++) {
        const std::string& item = blocks[i];
        const std::string label = JsonString(item, "label");
        const std::string text = JsonString(item, "text");
        const std::string block_error = JsonString(item, "error");
        const bool recognized = item.find("\"recognized\":true") != std::string::npos;
        if (!recognized) failed += 1;

        std::printf("  [%2zu] %-16s %s", i, label.c_str(), recognized ? "已识别" : "未识别");
        if (!recognized && !block_error.empty()) std::printf("  ← %s", block_error.c_str());
        if (!text.empty()) {
            std::string preview = text.substr(0, 48);
            for (char& ch : preview) if (ch == '\n') ch = ' ';
            std::printf("  «%s»", preview.c_str());
        }
        std::printf("\n");
    }
    std::printf("==> 版面块 %zu 个，未识别 %d 个\n", blocks.size(), failed);

    if (!options.out_markdown.empty()) {
        std::printf(WriteFile(options.out_markdown, markdown) ? "==> Markdown → %s\n"
                                                             : "!! 写文件失败：%s\n",
                    options.out_markdown.c_str());
    }
    if (!options.out_json.empty()) {
        std::printf(WriteFile(options.out_json, payload) ? "==> JSON → %s\n" : "!! 写文件失败：%s\n",
                    options.out_json.c_str());
    }
    if (options.verbose) {
        std::printf("\n---- Markdown ----\n%s\n", markdown.c_str());
    }

    vlcore_destroy(engine);
    return failed == 0 ? 0 : 3;
}
