// vlcore_pc —— 在桌面上跑与手机**完全同一份** impl/shard（同一个版面预处理、同一套提示词、
// 同一个两阶段编排、同一个 llama.cpp 求值路径），用来替代「改一行 → 推 CI → 出 ipa → 装机」的验证循环。
//
// 用法（详见 README）：
//   vlcore_pc --dir pages/ --layout PP-DocLayoutV3.onnx --vlm .gguf --mmproj .gguf --out-dir out/
//   vlcore_pc --image page.png --layout PP-DocLayoutV3.onnx --detect-only --overlay page.box.png
//   vlcore_pc --image page.png --layout PP-DocLayoutV3.onnx --stub-vlm -v
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "vlcore/vlcore_c.h"

// 新版 llama.cpp 的 libmtmd 自己也编了 stb_image（`mtmd-helper.cpp` 里解图），
// 直接 STB_IMAGE_IMPLEMENTATION 会和它重复定义；用 *_STATIC 让这份只在本文件可见。
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#if defined(__has_include)
#if __has_include("stb_image_write.h")
#define VLCORE_PC_HAS_WRITE 1
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif
#endif

namespace fs = std::filesystem;

namespace {

struct Options {
    std::string image;
    std::string dir;
    std::string out_dir;
    std::string layout;
    std::string vlm;
    std::string mmproj;
    std::string out_markdown;
    std::string out_json;
    std::string out_overlay;
    std::string out_boxes;
    bool stub_vlm = false;
    bool detect_only = false;
    bool verbose = false;
    bool quiet = false;
    int threads = 4;
    int n_ctx = 8192;
};

void Usage() {
    std::printf(
        "用法: vlcore_pc (--image <图片> | --dir <目录>) --layout <版面.onnx> [选项]\n"
        "\n"
        "  模型（与手机完全一致）：\n"
        "  --vlm <权重.gguf>      视觉语言模型主权重（不传则只做版面）\n"
        "  --mmproj <投影器.gguf> 视觉投影器，与 --vlm 成对\n"
        "  --stub-vlm             假 VLM 替身：验证裁块 / 提示词 / Markdown（不需要视觉模型）\n"
        "  --detect-only          只做版面（打印块表，可与手机/后端逐条对比）\n"
        "\n"
        "  输入输出：\n"
        "  --dir <目录>           批量跑目录下所有 png/jpg/jpeg/webp/bmp\n"
        "  --out-dir <目录>       批量模式的输出目录（默认写到输入目录旁的 out/）\n"
        "  --out <file.md>        整页 Markdown\n"
        "  --json <file.json>     vlcore 原始 JSON（与手机端同一份结构）\n"
        "  --boxes <file.csv>     块表 CSV：label,score,order,x1,y1,x2,y2（便于与手机结果 diff）\n"
        "  --overlay <file.png>   把框画到原图上（需要 scripts/fetch-stb-image.sh 拉过 stb_image_write.h）\n"
        "\n"
        "  其它：\n"
        "  --threads <n>          推理线程数（默认 4）\n"
        "  --n-ctx <n>            上下文长度（默认 8192）\n"
        "  -v, --verbose          打印整页 Markdown\n"
        "  -q, --quiet            不打印核心日志（默认打到 stderr，与手机「日志」tab 同源）\n");
}

/// 假 VLM：返回能看出「提示词 + 裁块尺寸」的文本，用于验证不依赖模型的前后链路。
char* StubRemote(const uint8_t* /*rgb*/, int width, int height, const char* prompt,
                 void* /*user*/, char** /*error*/) {
    std::string text = "【stub】";
    text += prompt != nullptr ? prompt : "";
    text += " " + std::to_string(width) + "x" + std::to_string(height);
    return strdup(text.c_str());
}

/// 核心日志（版面预处理数值、llama.cpp 自己的报错、逐块耗时都在这里）→ stderr。
void LogSink(int level, const char* message, void* user) {
    const auto* options = static_cast<const Options*>(user);
    if (options != nullptr && options->quiet) return;
    static const char* kTags[] = {"debug", "info ", "warn ", "error"};
    const int index = (level >= 0 && level <= 3) ? level : 1;
    std::fprintf(stderr, "[%s] %s\n", kTags[index], message != nullptr ? message : "");
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
        else if (arg == "--dir") options->dir = next("--dir");
        else if (arg == "--out-dir") options->out_dir = next("--out-dir");
        else if (arg == "--layout") options->layout = next("--layout");
        else if (arg == "--vlm") options->vlm = next("--vlm");
        else if (arg == "--mmproj") options->mmproj = next("--mmproj");
        else if (arg == "--out") options->out_markdown = next("--out");
        else if (arg == "--json") options->out_json = next("--json");
        else if (arg == "--overlay") options->out_overlay = next("--overlay");
        else if (arg == "--boxes") options->out_boxes = next("--boxes");
        else if (arg == "--threads") options->threads = std::atoi(next("--threads"));
        else if (arg == "--n-ctx") options->n_ctx = std::atoi(next("--n-ctx"));
        else if (arg == "--stub-vlm") options->stub_vlm = true;
        else if (arg == "--detect-only") options->detect_only = true;
        else if (arg == "-v" || arg == "--verbose") options->verbose = true;
        else if (arg == "-q" || arg == "--quiet") options->quiet = true;
        else if (arg == "-h" || arg == "--help") { Usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "未知参数：%s\n", arg.c_str());
            Usage();
            return false;
        }
    }
    if (options->layout.empty() || (options->image.empty() && options->dir.empty())) {
        Usage();
        return false;
    }
    if (!options->image.empty() && !options->dir.empty()) {
        std::fprintf(stderr, "--image 与 --dir 只能二选一\n");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ JSON

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

double JsonNumber(const std::string& json, const std::string& key, double fallback) {
    const std::string needle = "\"" + key + "\":";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    return std::atof(json.c_str() + pos + needle.size());
}

bool JsonNumbers(const std::string& json, const std::string& key, double* values, int count) {
    const std::string needle = "\"" + key + "\":[";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    const char* cursor = json.c_str() + pos + needle.size();
    for (int i = 0; i < count; i++) {
        char* end = nullptr;
        const double value = std::strtod(cursor, &end);
        if (end == cursor) return false;
        values[i] = value;
        cursor = end;
        while (*cursor == ',' || *cursor == ' ') cursor++;
    }
    return true;
}

struct BlockRow {
    std::string label;
    int order{-1};
    double score{0};
    double box[4]{0, 0, 0, 0};
    std::string text;
    std::string error;
    bool recognized{false};
};

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

std::vector<BlockRow> ParseBlocks(const std::string& payload) {
    std::vector<BlockRow> rows;
    for (const std::string& item : SplitBlocks(payload)) {
        BlockRow row;
        row.label = JsonString(item, "label");
        row.order = static_cast<int>(JsonNumber(item, "order", -1));
        row.score = JsonNumber(item, "score", 0);
        JsonNumbers(item, "bbox", row.box, 4);
        row.text = JsonString(item, "text");
        row.error = JsonString(item, "error");
        row.recognized = item.find("\"recognized\":true") != std::string::npos;
        rows.push_back(row);
    }
    return rows;
}

/// 只做版面时 C 门面返回的是裸数组，这里补成与整页结果同形的包装。
std::string WrapLayoutArray(const std::string& array) {
    return "{\"markdown\":\"\",\"blocks\":" + array + ",\"seconds\":0}";
}

// ------------------------------------------------------------------ 输出

bool WriteFile(const std::string& path, const std::string& content) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
    return true;
}

void PrintProgress(int done, int total, void* /*user*/) {
    std::printf("\r  识别进度 %d/%d", done, total);
    std::fflush(stdout);
    if (done == total) std::printf("\n");
}

std::string Preview(const std::string& text, size_t limit) {
    std::string out = text.substr(0, std::min(limit, text.size()));
    for (char& ch : out) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    if (text.size() > limit) out += "…";
    return out;
}

/// 块表：与手机「结果 → 版面块」同一组字段，可直接 diff。
void PrintTable(const std::vector<BlockRow>& rows) {
    std::printf("     order  label             score        bbox(x1,y1,x2,y2)          text\n");
    for (size_t i = 0; i < rows.size(); i++) {
        const BlockRow& row = rows[i];
        const std::string order = row.order >= 0 ? std::to_string(row.order) : "-";
        std::printf("  [%2zu] %-6s %-16s %.3f   (%7.1f,%7.1f,%7.1f,%7.1f)   %s\n", i, order.c_str(),
                    row.label.c_str(), row.score, row.box[0], row.box[1], row.box[2], row.box[3],
                    row.recognized ? Preview(row.text, 40).c_str()
                                   : ("← " + Preview(row.error, 60)).c_str());
    }
}

std::string TableCsv(const std::vector<BlockRow>& rows) {
    std::string csv = "index,label,score,order,x1,y1,x2,y2,recognized\n";
    char line[256];
    for (size_t i = 0; i < rows.size(); i++) {
        const BlockRow& row = rows[i];
        std::snprintf(line, sizeof(line), "%zu,%s,%.4f,%d,%.1f,%.1f,%.1f,%.1f,%d\n", i,
                      row.label.c_str(), row.score, row.order, row.box[0], row.box[1], row.box[2],
                      row.box[3], row.recognized ? 1 : 0);
        csv += line;
    }
    return csv;
}

#if defined(VLCORE_PC_HAS_WRITE)
// 3x5 数字点阵：把块序号画在框左上角，肉眼就能和终端表格对上。
const char* kDigitGlyph[10][5] = {
    {"###", "#.#", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", ".#.", "###"},
    {"###", "..#", "###", "#..", "###"}, {"###", "..#", "###", "..#", "###"},
    {"#.#", "#.#", "###", "..#", "..#"}, {"###", "#..", "###", "..#", "###"},
    {"###", "#..", "###", "#.#", "###"}, {"###", "..#", "..#", "..#", "..#"},
    {"###", "#.#", "###", "#.#", "###"}, {"###", "#.#", "###", "..#", "###"},
};

void PutPixel(std::vector<uint8_t>& rgb, int width, int height, int x, int y,
              const uint8_t color[3]) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const size_t index = (static_cast<size_t>(y) * width + x) * 3;
    rgb[index] = color[0];
    rgb[index + 1] = color[1];
    rgb[index + 2] = color[2];
}

void DrawRect(std::vector<uint8_t>& rgb, int width, int height, const BlockRow& row,
              const uint8_t color[3], int thickness) {
    const int x1 = static_cast<int>(row.box[0]);
    const int y1 = static_cast<int>(row.box[1]);
    const int x2 = static_cast<int>(row.box[2]);
    const int y2 = static_cast<int>(row.box[3]);
    for (int t = 0; t < thickness; t++) {
        for (int x = x1; x <= x2; x++) {
            PutPixel(rgb, width, height, x, y1 + t, color);
            PutPixel(rgb, width, height, x, y2 - t, color);
        }
        for (int y = y1; y <= y2; y++) {
            PutPixel(rgb, width, height, x1 + t, y, color);
            PutPixel(rgb, width, height, x2 - t, y, color);
        }
    }
}

void DrawIndex(std::vector<uint8_t>& rgb, int width, int height, int x, int y, int value,
               const uint8_t color[3], int scale) {
    std::string digits = std::to_string(value);
    int cursor = x + 2;
    for (char ch : digits) {
        const int glyph = ch - '0';
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if (kDigitGlyph[glyph][row][col] != '#') continue;
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        PutPixel(rgb, width, height, cursor + col * scale + dx,
                                 y + 2 + row * scale + dy, color);
                    }
                }
            }
        }
        cursor += 4 * scale;
    }
}

/// 按类别给颜色，和手机叠加层一眼能对上的分组（正文/标题/表格/图/公式/其它）。
void LabelColor(const std::string& label, uint8_t color[3]) {
    if (label == "table") {
        color[0] = 0; color[1] = 160; color[2] = 80;
    } else if (label == "doc_title" || label == "paragraph_title") {
        color[0] = 240; color[1] = 130; color[2] = 0;
    } else if (label == "image" || label == "chart" || label == "header_image" ||
               label == "footer_image") {
        color[0] = 130; color[1] = 130; color[2] = 130;
    } else if (label == "display_formula" || label == "inline_formula" ||
               label == "formula_number") {
        color[0] = 150; color[1] = 60; color[2] = 200;
    } else if (label == "text" || label == "content" || label == "abstract" ||
               label == "aside_text" || label == "vertical_text") {
        color[0] = 0; color[1] = 110; color[2] = 230;
    } else {
        color[0] = 220; color[1] = 40; color[2] = 40;
    }
}

bool WriteOverlay(const std::string& path, const std::vector<uint8_t>& rgb, int width, int height,
                  const std::vector<BlockRow>& rows) {
    std::vector<uint8_t> canvas = rgb;
    const int scale = width > 1600 ? 2 : 3;
    for (size_t i = 0; i < rows.size(); i++) {
        uint8_t color[3] = {220, 40, 40};
        LabelColor(rows[i].label, color);
        DrawRect(canvas, width, height, rows[i], color, width > 1200 ? 3 : 2);
        DrawIndex(canvas, width, height, static_cast<int>(rows[i].box[0]),
                  static_cast<int>(rows[i].box[1]), static_cast<int>(i), color, scale);
    }
    return stbi_write_png(path.c_str(), width, height, 3, canvas.data(), width * 3) != 0;
}
#endif  // VLCORE_PC_HAS_WRITE

// ------------------------------------------------------------------ 单页

struct PageResult {
    bool ok{false};
    std::string payload;
    std::string markdown;
    std::vector<BlockRow> rows;
    double seconds{0};
    int failed{0};
};

bool ProcessPage(vlcore_engine_t* engine, const Options& options, const std::string& image_path,
                 const std::string& base_out, PageResult* result) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(image_path.c_str(), &width, &height, &channels, 3);
    if (pixels == nullptr) {
        std::fprintf(stderr, "!! 图片读取失败：%s（%s）\n", image_path.c_str(), stbi_failure_reason());
        return false;
    }
    std::printf("==> %s  %dx%d\n", image_path.c_str(), width, height);

    char* error = nullptr;
    const auto started = std::chrono::steady_clock::now();
    char* payload = nullptr;
    if (options.detect_only) {
        char* layout_json = vlcore_layout_json(engine, pixels, width, height, &error);
        if (layout_json != nullptr) {
            payload = strdup(WrapLayoutArray(layout_json).c_str());
            vlcore_free_string(layout_json);
        }
    } else {
        payload = vlcore_run(engine, pixels, width, height, PrintProgress, nullptr, &error);
    }
    result->seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

#if defined(VLCORE_PC_HAS_WRITE)
    const std::vector<uint8_t> snapshot(pixels, pixels + static_cast<size_t>(width) * height * 3);
#else
    const std::vector<uint8_t> snapshot;
#endif
    stbi_image_free(pixels);

    if (payload == nullptr) {
        std::fprintf(stderr, "!! 失败：%s\n", error != nullptr ? error : "未知错误");
        if (error != nullptr) vlcore_free_string(error);
        return false;
    }

    result->payload = payload;
    vlcore_free_string(payload);
    result->markdown = JsonString(result->payload, "markdown");
    result->rows = ParseBlocks(result->payload);
    for (const BlockRow& row : result->rows) {
        // 只做版面时所有块都没有 recognized 字段，不能算失败。
        if (!options.detect_only && !row.recognized) result->failed += 1;
    }
    result->ok = true;

    std::printf("==> %s，用时 %.2fs，版面块 %zu 个，未识别 %d 个\n",
                options.detect_only ? "版面完成" : "识别完成", result->seconds,
                result->rows.size(), result->failed);
    PrintTable(result->rows);

    const auto save = [&](const std::string& path, const std::string& content) {
        if (path.empty()) return;
        std::printf(WriteFile(path, content) ? "==> 已写 %s\n" : "!! 写文件失败：%s\n", path.c_str());
    };
    if (!base_out.empty()) {
        if (!result->markdown.empty()) save(base_out + ".md", result->markdown);
        save(base_out + ".json", result->payload);
        save(base_out + ".boxes.csv", TableCsv(result->rows));
#if defined(VLCORE_PC_HAS_WRITE)
        if (WriteOverlay(base_out + ".boxes.png", snapshot, width, height, result->rows)) {
            std::printf("==> 已写 %s.boxes.png\n", base_out.c_str());
        }
#endif
    }
    if (!options.out_markdown.empty()) save(options.out_markdown, result->markdown);
    if (!options.out_json.empty()) save(options.out_json, result->payload);
    if (!options.out_boxes.empty()) save(options.out_boxes, TableCsv(result->rows));
#if defined(VLCORE_PC_HAS_WRITE)
    if (!options.out_overlay.empty()) {
        if (WriteOverlay(options.out_overlay, snapshot, width, height, result->rows)) {
            std::printf("==> 已写 %s\n", options.out_overlay.c_str());
        } else {
            std::fprintf(stderr, "!! 叠加图写入失败：%s\n", options.out_overlay.c_str());
        }
    }
#else
    if (!options.out_overlay.empty()) {
        std::fprintf(stderr, "!! 未编译叠加图（缺 stb_image_write.h）：bash scripts/fetch-stb-image.sh\n");
    }
#endif
    if (options.verbose && !result->markdown.empty()) {
        std::printf("\n---- Markdown ----\n%s\n", result->markdown.c_str());
    }
    return true;
}

bool IsImageFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseArgs(argc, argv, &options)) return 2;

    vlcore_set_log(LogSink, &options);

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
        return 1;
    }

    std::printf("==> 引擎：版面=%s  VLM=%s  预设=%s %s（产线 %s）\n",
                vlcore_layout_engine_built() ? "已编译" : "未编译",
                vlcore_vlm_engine_built() ? "已编译" : "未编译",
                vlcore_preset_name(), vlcore_preset_version(), vlcore_pipeline_version());
    if (options.detect_only) {
        std::printf("==> 只做版面（不送 VLM）\n");
    } else if (options.stub_vlm) {
        vlcore_set_remote(engine, StubRemote, nullptr);
        std::printf("==> 使用假 VLM 替身（只验证版面 / 提示词 / Markdown 通路）\n");
    } else if (!use_vlm) {
        std::printf("==> 未指定 --vlm：逐块识别会全部失败（这是手机端「Markdown 全是占位图」的现场）\n");
    }

    int exit_code = 0;

    if (!options.dir.empty()) {
        fs::path out_dir = options.out_dir.empty() ? fs::path(options.dir) / "vlcore_pc-out"
                                                   : fs::path(options.out_dir);
        std::error_code ec;
        fs::create_directories(out_dir, ec);

        std::vector<fs::path> pages;
        for (const auto& entry : fs::directory_iterator(options.dir, ec)) {
            if (entry.is_regular_file() && IsImageFile(entry.path())) pages.push_back(entry.path());
        }
        std::sort(pages.begin(), pages.end());
        if (pages.empty()) {
            std::fprintf(stderr, "!! %s 下没有图片\n", options.dir.c_str());
            vlcore_destroy(engine);
            return 1;
        }

        int failed_pages = 0;
        for (const fs::path& page : pages) {
            const std::string base = (out_dir / page.stem()).string();
            PageResult result;
            if (!ProcessPage(engine, options, page.string(), base, &result)) {
                failed_pages += 1;
                continue;
            }
            if (result.failed > 0) failed_pages += 1;
        }
        std::printf("==> 批量完成：%zu 页，%d 页有未识别块；输出目录 %s\n", pages.size(),
                    failed_pages, out_dir.string().c_str());
        exit_code = failed_pages == 0 ? 0 : 3;
    } else {
        PageResult result;
        if (!ProcessPage(engine, options, options.image, std::string(), &result)) {
            vlcore_destroy(engine);
            return 1;
        }
        exit_code = result.failed == 0 ? 0 : 3;
    }

    vlcore_destroy(engine);
    return exit_code;
}
