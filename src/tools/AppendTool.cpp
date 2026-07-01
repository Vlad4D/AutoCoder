#include "AppendTool.h"

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

bool readWholeFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

}  // namespace

json AppendTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "append"},
            {"description",
             "Append content to a file, creating parent directories as needed. "
             "Use after write for later chunks of large generated files. Existing "
             "files must be read or created first in this conversation. Use "
             "expected_offset to guard chunk order when needed."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Absolute or project-relative path."}}},
                    {"content", {{"type", "string"}, {"description", "Text to append exactly as provided."}}},
                    {"expected_offset", {
                        {"type", "integer"},
                        {"description", "Optional byte offset that must match the current file size before appending."}
                    }}
                }},
                {"required", json::array({"path", "content"})}
            }}
        }}
    };
}

ToolResult AppendTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("path")) return ToolResult::error("missing required arg: path");
    if (!args.contains("content")) return ToolResult::error("missing required arg: content");

    const std::string pathArg = args["path"].get<std::string>();
    const std::string content = args["content"].get<std::string>();

    fs::path path = pathutil::resolveSafely(pathArg, ctx);
    if (path.empty()) return ToolResult::error(
        "refusing to append outside project root: " + pathArg);
    std::error_code ec;
    bool fileExists = fs::exists(path, ec);
    if (ec) return ToolResult::error("could not stat file: " + ec.message());

    uintmax_t currentSize = 0;
    if (fileExists) {
        currentSize = fs::file_size(path, ec);
        if (ec) return ToolResult::error("could not get file size: " + ec.message());
    }

    if (args.contains("expected_offset") && !args["expected_offset"].is_null()) {
        uintmax_t expected = args["expected_offset"].get<uintmax_t>();
        if (expected != currentSize) {
            return ToolResult::error(
                "expected_offset mismatch for " + pathutil::toUtf8(path)
                + ": expected " + std::to_string(expected)
                + " bytes, file is " + std::to_string(currentSize) + " bytes");
        }
    }

    // Enforce read-before-write only for existing files.
    if (fileExists) {
        if (!ctx.readSet) {
            return ToolResult::error(
                "internal: append needs a readSet to enforce read-before-write");
        }
        fs::path canonical = fs::weakly_canonical(path, ec);
        const fs::path& key = canonical.empty() ? path : canonical;
        if (ctx.readSet->find(key) == ctx.readSet->end()) {
            // For a new file, allow append only if the path is in readSet (from a prior write/read).
            return ToolResult::error(
                "refusing to append to a file you have not read in this conversation: "
                + pathutil::toUtf8(path) + ". Call `read` on it first.");
        }
    }

    std::string oldContent;
    if (fileExists && !readWholeFile(path, oldContent)) {
        return ToolResult::error("could not open for reading: " + pathutil::toUtf8(path));
    }

    fs::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec2;
        fs::create_directories(parent, ec2);
        if (ec2) return ToolResult::error("could not create parent dirs: " + ec2.message());
    }

    if (ctx.fileBeforeSnapshots) {
        fs::path canonical = fs::weakly_canonical(path, ec);
        const fs::path& key = canonical.empty() ? path : canonical;
        if (ctx.fileBeforeSnapshots->find(key) == ctx.fileBeforeSnapshots->end()) {
            (*ctx.fileBeforeSnapshots)[key] = fileExists ? oldContent : std::string();
        }
    }

    pathutil::makeWritable(path, ctx.projectRoot);

    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return ToolResult::error("could not open for appending: " + pathutil::toUtf8(path));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return ToolResult::error("append failed: " + pathutil::toUtf8(path));
    out.close();

    uintmax_t totalSize = currentSize + static_cast<uintmax_t>(content.size());

    if (ctx.readSet) {
        fs::path canonical = fs::weakly_canonical(path, ec);
        ctx.readSet->insert(canonical.empty() ? path : canonical);
    }

    return ToolResult::success(
        "appended " + std::to_string(content.size()) + " bytes to "
        + pathutil::toUtf8(path) + " (total_bytes=" + std::to_string(totalSize)
        + ", +" + std::to_string(logicalLineCount(content)) + " line(s))");
}
