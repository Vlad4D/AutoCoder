#include "WriteTool.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

int logicalLineCount(const std::string& text) {
    if (text.empty()) return 0;
    return static_cast<int>(std::count(text.begin(), text.end(), '\n'))
        + (text.back() == '\n' ? 0 : 1);
}

}  // namespace

json WriteTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "write"},
            {"description",
             "Write a file, creating parent directories as needed. Overwrites existing "
             "files, which must be read first in this conversation. For large generated "
             "files, write the first chunk and continue with append."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path",    {{"type", "string"}, {"description", "Absolute or project-relative path."}}},
                    {"content", {{"type", "string"}, {"description", "Full file content. Will replace any existing file."}}}
                }},
                {"required", json::array({"path", "content"})}
            }}
        }}
    };
}

ToolResult WriteTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("path"))    return ToolResult::error("missing required arg: path");
    if (!args.contains("content")) return ToolResult::error("missing required arg: content");

    const std::string pathArg = args["path"].get<std::string>();
    const std::string content = args["content"].get<std::string>();

    fs::path path = pathutil::resolveSafely(pathArg, ctx);
    if (path.empty()) return ToolResult::error(
        "refusing to write outside project root: " + pathArg);
    std::error_code ec;
    bool fileExists = fs::exists(path, ec);
    std::string oldContent;

    // Enforce read-before-write only for existing files.
    if (fileExists) {
        if (!ctx.readSet) {
            return ToolResult::error(
                "internal: write needs a readSet to enforce read-before-write");
        }
        std::error_code ec2;
        fs::path canonical = fs::weakly_canonical(path, ec2);
        const fs::path& key = canonical.empty() ? path : canonical;
        if (ctx.readSet->find(key) == ctx.readSet->end()) {
            return ToolResult::error(
                "refusing to overwrite a file you have not read in this conversation: "
                + pathutil::toUtf8(path) + ". Call `read` on it first.");
        }
    }

    if (fileExists) {
        std::ifstream oldIn(path, std::ios::binary);
        if (oldIn) {
            std::ostringstream oldBuf;
            oldBuf << oldIn.rdbuf();
            oldContent = oldBuf.str();
        }
    }

    fs::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec3;
        fs::create_directories(parent, ec3);
        if (ec3) return ToolResult::error("could not create parent dirs: " + ec3.message());
    }

    // Snapshot old content for checkpoint before overwriting.
    if (ctx.fileBeforeSnapshots) {
        fs::path canonical = fs::weakly_canonical(path, ec);
        const fs::path& key = canonical.empty() ? path : canonical;
        if (ctx.fileBeforeSnapshots->find(key) == ctx.fileBeforeSnapshots->end()) {
            if (fileExists) {
                (*ctx.fileBeforeSnapshots)[key] = oldContent;
            } else {
                // New file -- record empty string as "before".
                (*ctx.fileBeforeSnapshots)[key] = std::string();
            }
        }
    }

    pathutil::makeWritable(path, ctx.projectRoot);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return ToolResult::error("could not open for writing: " + pathutil::toUtf8(path));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return ToolResult::error("write failed: " + pathutil::toUtf8(path));

    if (ctx.readSet) {
        fs::path canonical = fs::weakly_canonical(path, ec);
        ctx.readSet->insert(canonical.empty() ? path : canonical);
    }

    const std::string summary = "wrote " + std::to_string(content.size())
                              + " bytes to " + pathutil::toUtf8(path)
                              + " (total_bytes=" + std::to_string(content.size()) + ")";

    // When overwriting an existing file, embed old+new content so the UI can
    // render a real diff (showing removed lines too), like edit/replace_lines.
    // Capped to keep the result from bloating the conversation on huge files.
    constexpr size_t kDiffEmbedCap = 64 * 1024;
    if (fileExists && oldContent != content
        && oldContent.size() <= kDiffEmbedCap && content.size() <= kDiffEmbedCap) {
        std::string oldBlock = oldContent;
        if (!oldBlock.empty() && oldBlock.back() != '\n') oldBlock += '\n';
        std::string newBlock = content;
        if (!newBlock.empty() && newBlock.back() != '\n') newBlock += '\n';
        return ToolResult::success(
            "=== OLD CONTENT ===\n" + oldBlock
            + "=== NEW CONTENT ===\n" + newBlock
            + "=== RESULT ===\n" + summary);
    }

    // New file (or unchanged / too large): report the line count added.
    return ToolResult::success(
        summary + " (+" + std::to_string(logicalLineCount(content))
        + " -" + std::to_string(logicalLineCount(oldContent)) + ")");
}
