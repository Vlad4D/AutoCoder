#include "TestHarness.h"

#include <set>
#include <string>

#include <QImage>
#include <QString>

#include <nlohmann/json.hpp>

#include "tools/AnalyzeImageTool.h"
#include "tools/PathUtil.h"
#include "tools/ToolContext.h"

// analyze_image: pixel-less image verification for the agent. Images are
// generated with QImage setPixel only (no QPainter, so a plain
// QCoreApplication suffices); BMP/PNG codecs are built into QtGui.

using namespace test;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {
QString qpath(const fs::path& p) { return QString::fromStdString(pathutil::toUtf8(p)); }

void saveSquareOnDark(const fs::path& p) {
    QImage img(64, 64, QImage::Format_RGB32);
    img.fill(qRgb(10, 10, 20));                       // dark background
    for (int y = 16; y < 48; ++y)
        for (int x = 16; x < 48; ++x)
            img.setPixel(x, y, qRgb(255, 200, 40));   // bright square
    img.save(qpath(p));
}
}  // namespace

void suite_image() {
    const fs::path root = freshDir("image");
    std::set<fs::path> readSet;
    ToolContext ctx;
    ctx.projectRoot = root;
    ctx.readSet = &readSet;

    AnalyzeImageTool tool;

    // ---- a bright square on dark background: shape must be perceivable ----
    saveSquareOnDark(root / "shape.bmp");
    {
        ToolResult r = tool.execute(json{{"path", "shape.bmp"}}, ctx);
        CHECK_MSG(r.ok, "valid BMP is analyzed");
        CHECK_MSG(r.content.find("size: 64x64") != std::string::npos, "dimensions reported");
        CHECK_MSG(r.content.find("uniform: no") != std::string::npos, "non-uniform detected");
        CHECK_MSG(r.content.find("dominant colors:") != std::string::npos, "dominant colors listed");
        CHECK_MSG(r.content.find("ASCII preview") != std::string::npos, "ASCII preview present");
        // The bright square must appear as dense ramp characters, the dark
        // background as light ones.
        CHECK_MSG(r.content.find('@') != std::string::npos ||
                  r.content.find('%') != std::string::npos ||
                  r.content.find('#') != std::string::npos,
                  "bright region renders as dense characters");
    }

    // ---- uniform image flagged loudly (the 'nothing was drawn' case) ----
    {
        QImage img(96, 96, QImage::Format_RGB32);
        img.fill(qRgb(13, 13, 26));
        img.save(qpath(root / "flat.png"));
        ToolResult r = tool.execute(json{{"path", "flat.png"}}, ctx);
        CHECK_MSG(r.ok, "uniform image still analyzes");
        CHECK_MSG(r.content.find("uniform: yes") != std::string::npos, "uniformity detected");
        CHECK_MSG(r.content.find("distinct colors (sampled): 1") != std::string::npos,
                  "single distinct color reported");
    }

    // ---- corrupt file: clear diagnosis, not a crash ----
    {
        writeFile(root / "broken.bmp", "BM\x00\x00garbage that is not a bitmap");
        ToolResult r = tool.execute(json{{"path", "broken.bmp"}}, ctx);
        CHECK_MSG(!r.ok, "corrupt image is rejected");
        CHECK_MSG(r.content.find("not a valid image") != std::string::npos, "clear parse error");
        CHECK_MSG(r.content.find("first 16 bytes:") != std::string::npos, "header hex dump included");
    }

    // ---- missing / out-of-root paths ----
    CHECK(!tool.execute(json{{"path", "nope.png"}}, ctx).ok);
    CHECK(!tool.execute(json{{"path", "../escape.png"}}, ctx).ok);
}
