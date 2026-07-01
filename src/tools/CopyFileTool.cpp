#include "CopyFileTool.h"

#include <fstream>
#include <sstream>
#include <string>

#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {
bool readWholeFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}
}  // namespace

json CopyFileTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "copy_file"},
            {"description",
             "Copy a file from `source` to `destination` (both inside the project), "
             "byte-for-byte. Use this to duplicate or replace a file instead of reading "
             "its contents and re-writing them -- it works for large/binary files and "
             "clears the destination's read-only attribute. Overwrites an existing "
             "destination (revertible via checkpoint). Do NOT shell out to cp/copy/"
             "powershell/python for this."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"source",      {{"type", "string"}, {"description", "Path to the file to copy from."}}},
                    {"destination", {{"type", "string"}, {"description", "Path to copy to (overwritten if it exists)."}}}
                }},
                {"required", json::array({"source", "destination"})}
            }}
        }}
    };
}

ToolResult CopyFileTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("source"))      return ToolResult::error("missing required arg: source");
    if (!args.contains("destination")) return ToolResult::error("missing required arg: destination");

    const std::string srcArg = args["source"].get<std::string>();
    const std::string dstArg = args["destination"].get<std::string>();

    fs::path src = pathutil::resolveSafely(srcArg, ctx);
    if (src.empty()) return ToolResult::error("refusing to copy from outside project root: " + srcArg);
    fs::path dst = pathutil::resolveSafely(dstArg, ctx);
    if (dst.empty()) return ToolResult::error("refusing to copy to outside project root: " + dstArg);

    std::error_code ec;
    if (!fs::exists(src, ec)) return ToolResult::error("source does not exist: " + pathutil::toUtf8(src));
    if (!fs::is_regular_file(src, ec)) return ToolResult::error("source is not a regular file: " + pathutil::toUtf8(src));

    std::string content;
    if (!readWholeFile(src, content)) return ToolResult::error("could not read source: " + pathutil::toUtf8(src));

    const bool dstExisted = fs::exists(dst, ec) && fs::is_regular_file(dst, ec);
    std::string oldDst;
    if (dstExisted) readWholeFile(dst, oldDst);

    // Snapshot the destination for checkpoint/revert (old content, or "" if new).
    if (ctx.fileBeforeSnapshots) {
        fs::path canonical = fs::weakly_canonical(dst, ec);
        const fs::path& key = canonical.empty() ? dst : canonical;
        if (ctx.fileBeforeSnapshots->find(key) == ctx.fileBeforeSnapshots->end()) {
            (*ctx.fileBeforeSnapshots)[key] = dstExisted ? oldDst : std::string();
        }
    }

    fs::path parent = dst.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) return ToolResult::error("could not create parent dirs: " + ec.message());
    }

    pathutil::makeWritable(dst, ctx.projectRoot);

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return ToolResult::error("could not open destination for writing: " + pathutil::toUtf8(dst));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return ToolResult::error("write failed: " + pathutil::toUtf8(dst));
    out.close();

    // Mark the destination as read so subsequent edits/overwrites are allowed.
    if (ctx.readSet) {
        fs::path canonical = fs::weakly_canonical(dst, ec);
        ctx.readSet->insert(canonical.empty() ? dst : canonical);
    }

    return ToolResult::success(
        "copied " + std::to_string(content.size()) + " bytes from "
        + pathutil::toUtf8(src) + " to " + pathutil::toUtf8(dst)
        + (dstExisted ? " (overwrote existing)" : " (created)"));
}
