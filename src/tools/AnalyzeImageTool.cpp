#include "AnalyzeImageTool.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <QImage>
#include <QImageReader>
#include <QString>

#include "IgnoreRules.h"
#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kDefaultAsciiWidth = 48;
constexpr int kMinAsciiWidth = 16;
constexpr int kMaxAsciiWidth = 120;
constexpr int kMaxAsciiHeight = 60;
constexpr int kMaxStatSamplesPerAxis = 256;
constexpr int kDistinctColorCap = 4096;

// 10 luminance levels, dark -> bright.
constexpr const char* kAsciiRamp = " .:-=+*#%@";

std::string hexByte(unsigned char b) {
    static const char* digits = "0123456789ABCDEF";
    return {digits[b >> 4], digits[b & 0xF]};
}

std::string firstBytesHex(const fs::path& path, size_t count) {
    std::ifstream in(path, std::ios::binary);
    std::string out;
    unsigned char b = 0;
    for (size_t i = 0; i < count && in.read(reinterpret_cast<char*>(&b), 1); ++i) {
        if (!out.empty()) out += ' ';
        out += hexByte(b);
    }
    return out;
}

std::string colorHex(int r, int g, int b) {
    return "#" + hexByte(static_cast<unsigned char>(r))
               + hexByte(static_cast<unsigned char>(g))
               + hexByte(static_cast<unsigned char>(b));
}

int luminance(QRgb px) {
    return static_cast<int>(0.299 * qRed(px) + 0.587 * qGreen(px) + 0.114 * qBlue(px));
}

}  // namespace

json AnalyzeImageTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "analyze_image"},
            {"description",
             "Analyze an image file (BMP/PNG/JPG/GIF/...) and report what it contains: "
             "parse validity, dimensions, color statistics (distinct colors, uniformity, "
             "dominant colors), and a coarse ASCII luminance preview that makes shapes "
             "visible as text. Use this to VERIFY any image you generate or modify "
             "(render output, screenshots, icons) instead of assuming it is correct -- "
             "a program can exit 0 and still have produced an empty or corrupt image."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"},
                              {"description", "Absolute or project-relative path to the image."}}},
                    {"ascii_width", {{"type", "integer"},
                                     {"description", "Width of the ASCII preview in characters (16-120, default 48)."}}}
                }},
                {"required", json::array({"path"})}
            }}
        }}
    };
}

ToolResult AnalyzeImageTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("path")) return ToolResult::error("missing required arg: path");
    const std::string pathArg = args["path"].get<std::string>();

    fs::path path = pathutil::resolveSafely(pathArg, ctx);
    if (path.empty()) return ToolResult::error(
        "refusing to read outside project root: " + pathArg);
    if (pathutil::IgnoreRules::load(ctx.projectRoot).isIgnored(path)) {
        return ToolResult::error(
            "refusing to read ignored file (matches .gitignore/.autocoderignore or a "
            "built-in secret pattern; its contents are not sent to the LLM): " + pathArg);
    }
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return ToolResult::error("file does not exist: " + pathutil::toUtf8(path));
    }
    const uintmax_t fileBytes = fs::file_size(path, ec);

    int asciiWidth = kDefaultAsciiWidth;
    if (args.contains("ascii_width") && args["ascii_width"].is_number_integer()) {
        asciiWidth = std::clamp(args["ascii_width"].get<int>(),
                                kMinAsciiWidth, kMaxAsciiWidth);
    }

    const QString qpath = QString::fromUtf8(pathutil::toUtf8(path).c_str());
    QImageReader reader(qpath);
    const std::string detectedFormat = reader.format().toStdString();
    QImage img = reader.read();
    if (img.isNull()) {
        return ToolResult::error(
            "not a valid image (failed to parse): " + pathutil::toUtf8(path)
            + "\nreader error: " + reader.errorString().toStdString()
            + "\nfile size: " + std::to_string(fileBytes) + " bytes"
            + "\nfirst 16 bytes: " + firstBytesHex(path, 16)
            + "\nIf you wrote this file with hand-built headers, check for struct "
              "padding (e.g. BMP headers need #pragma pack(1)) and field offsets.");
    }

    const int w = img.width();
    const int h = img.height();
    const bool hasAlpha = img.hasAlphaChannel();
    img = img.convertToFormat(QImage::Format_ARGB32);
    if (img.isNull()) return ToolResult::error("could not convert image to RGB for analysis");

    // ---- Color statistics over a bounded sample grid ----
    const int stepX = std::max(1, w / kMaxStatSamplesPerAxis);
    const int stepY = std::max(1, h / kMaxStatSamplesPerAxis);
    std::unordered_map<uint32_t, long long> buckets;   // 5 bits/channel quantization
    std::set<uint32_t> distinct;
    bool distinctCapped = false;
    long long samples = 0, sumR = 0, sumG = 0, sumB = 0;

    for (int y = 0; y < h; y += stepY) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; x += stepX) {
            const QRgb px = line[x];
            ++samples;
            sumR += qRed(px); sumG += qGreen(px); sumB += qBlue(px);
            const uint32_t exact = static_cast<uint32_t>(px) & 0x00FFFFFFu;
            if (!distinctCapped) {
                distinct.insert(exact);
                if (static_cast<int>(distinct.size()) > kDistinctColorCap)
                    distinctCapped = true;
            }
            const uint32_t q = ((static_cast<uint32_t>(qRed(px))   >> 3) << 10)
                             | ((static_cast<uint32_t>(qGreen(px)) >> 3) << 5)
                             |  (static_cast<uint32_t>(qBlue(px))  >> 3);
            ++buckets[q];
        }
    }
    if (samples == 0) return ToolResult::error("image has no pixels");

    std::vector<std::pair<uint32_t, long long>> top(buckets.begin(), buckets.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    const bool uniform = distinct.size() == 1 && !distinctCapped;

    // ---- ASCII luminance preview ----
    int asciiW = std::min(asciiWidth, w);
    // Terminal cells are ~2x taller than wide; halve the row count to keep
    // the aspect ratio recognizable.
    int asciiH = std::max(1, static_cast<int>(
        static_cast<double>(h) / w * asciiW * 0.5 + 0.5));
    asciiH = std::min({asciiH, kMaxAsciiHeight, h});

    std::ostringstream art;
    for (int cy = 0; cy < asciiH; ++cy) {
        for (int cx = 0; cx < asciiW; ++cx) {
            // Average luminance over a small sample grid inside the cell.
            const int x0 = static_cast<int>(static_cast<double>(cx) * w / asciiW);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<double>(cx + 1) * w / asciiW));
            const int y0 = static_cast<int>(static_cast<double>(cy) * h / asciiH);
            const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<double>(cy + 1) * h / asciiH));
            const int sx = std::max(1, (x1 - x0) / 3);
            const int sy = std::max(1, (y1 - y0) / 3);
            long long lum = 0, n = 0;
            for (int y = y0; y < y1 && y < h; y += sy) {
                const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                for (int x = x0; x < x1 && x < w; x += sx) {
                    lum += luminance(line[x]);
                    ++n;
                }
            }
            const int level = n ? static_cast<int>(lum / n * 9 / 255) : 0;
            art << kAsciiRamp[std::clamp(level, 0, 9)];
        }
        art << "\n";
    }

    // ---- Assemble report ----
    std::ostringstream o;
    o << "Image: " << pathutil::toUtf8(path) << "\n"
      << "format: " << (detectedFormat.empty() ? "unknown" : detectedFormat)
      << ", size: " << w << "x" << h
      << ", depth: " << img.depth() << "-bit"
      << ", alpha: " << (hasAlpha ? "yes" : "no")
      << ", file: " << fileBytes << " bytes\n";
    o << "distinct colors (sampled): "
      << (distinctCapped ? std::to_string(kDistinctColorCap) + "+"
                         : std::to_string(distinct.size())) << "\n";
    o << "uniform: " << (uniform ? "yes (every sampled pixel is the same color "
                                   "-- if this was a render, nothing was drawn)"
                                 : "no") << "\n";
    o << "mean RGB: (" << sumR / samples << ", " << sumG / samples << ", "
      << sumB / samples << ")\n";
    o << "dominant colors:";
    const size_t nTop = std::min<size_t>(5, top.size());
    for (size_t i = 0; i < nTop; ++i) {
        const uint32_t q = top[i].first;
        const int r = static_cast<int>(((q >> 10) & 0x1F) << 3) + 4;
        const int g = static_cast<int>(((q >> 5)  & 0x1F) << 3) + 4;
        const int b = static_cast<int>((q & 0x1F) << 3) + 4;
        const double pct = 100.0 * static_cast<double>(top[i].second) / static_cast<double>(samples);
        o << (i ? "," : "") << " " << colorHex(std::min(r, 255), std::min(g, 255), std::min(b, 255))
          << " " << std::fixed << std::setprecision(1) << pct << "%";
    }
    o << "\n";
    o << "ASCII preview (" << asciiW << "x" << asciiH
      << " chars, dark=' ' bright='@', terminal aspect corrected):\n"
      << art.str();

    return ToolResult::success(o.str());
}
