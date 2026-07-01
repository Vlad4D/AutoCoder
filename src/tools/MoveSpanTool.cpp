#include "MoveSpanTool.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "CodeStructure.h"
#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path canonicalKey(const fs::path& path) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    return canonical.empty() ? path : canonical;
}

bool wasRead(const fs::path& path, ToolContext& ctx) {
    if (!ctx.readSet) return true;
    return ctx.readSet->find(canonicalKey(path)) != ctx.readSet->end();
}

void markRead(const fs::path& path, ToolContext& ctx) {
    if (ctx.readSet) ctx.readSet->insert(canonicalKey(path));
}

void snapshot(const fs::path& path, const std::string& content, ToolContext& ctx) {
    if (!ctx.fileBeforeSnapshots) return;
    fs::path key = canonicalKey(path);
    if (ctx.fileBeforeSnapshots->find(key) == ctx.fileBeforeSnapshots->end())
        (*ctx.fileBeforeSnapshots)[key] = content;
}

int insertionBoundary(const json& args, int totalLines, std::string& error) {
    int insertLine = args.value("insert_line", 0);
    std::string position = args.value("position", "before");
    if (position != "before" && position != "after") {
        error = "position must be 'before' or 'after'";
        return -1;
    }
    if (position == "before") {
        if (insertLine < 1 || insertLine > totalLines + 1) {
            error = "insert_line out of range for position=before";
            return -1;
        }
        return insertLine - 1;
    }
    if (insertLine < 0 || insertLine > totalLines) {
        error = "insert_line out of range for position=after";
        return -1;
    }
    return insertLine;
}

}  // namespace

json MoveSpanTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "move_span"},
            {"description",
             "Move a line span from one location to another without returning the moved text. "
             "Use read_outline first to get line ranges and hashes. Requires files to have been read or outlined."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"source_path",   {{"type", "string"},  {"description", "Absolute or project-relative source file path."}}},
                    {"start_line",    {{"type", "integer"}, {"description", "1-based first line to move."}}},
                    {"end_line",      {{"type", "integer"}, {"description", "1-based last line to move, inclusive."}}},
                    {"target_path",   {{"type", "string"},  {"description", "Absolute or project-relative target file path."}}},
                    {"insert_line",   {{"type", "integer"}, {"description", "Line used as insertion anchor. For position=before, use 1..line_count+1. For position=after, use 0..line_count."}}},
                    {"position",      {{"type", "string"},  {"enum", json::array({"before", "after"})}, {"description", "Insert before or after insert_line. Defaults to before."}}},
                    {"expected_hash", {{"type", "string"},  {"description", "Optional hash of the source span from read_outline. If provided, the move fails on mismatch."}}},
                    {"dry_run",       {{"type", "boolean"}, {"description", "Validate and report what would move without writing files."}}}
                }},
                {"required", json::array({"source_path", "start_line", "end_line", "target_path", "insert_line"})}
            }}
        }}
    };
}

ToolResult MoveSpanTool::execute(const json& args, ToolContext& ctx) {
    for (const char* key : {"source_path", "start_line", "end_line", "target_path", "insert_line"}) {
        if (!args.contains(key)) return ToolResult::error(std::string("missing required arg: ") + key);
    }

    fs::path sourcePath = pathutil::resolveSafely(args["source_path"].get<std::string>(), ctx);
    if (sourcePath.empty()) return ToolResult::error(
        "refusing to move from outside project root: " + args["source_path"].get<std::string>());
    fs::path targetPath = pathutil::resolveSafely(args["target_path"].get<std::string>(), ctx);
    if (targetPath.empty()) return ToolResult::error(
        "refusing to move to outside project root: " + args["target_path"].get<std::string>());
    int startLine = args["start_line"].get<int>();
    int endLine = args["end_line"].get<int>();
    bool dryRun = args.value("dry_run", false);

    if (startLine < 1) return ToolResult::error("start_line must be >= 1");
    if (endLine < startLine) return ToolResult::error("end_line must be >= start_line");
    if (!fs::exists(sourcePath)) return ToolResult::error("source file does not exist: " + pathutil::toUtf8(sourcePath));
    if (!fs::exists(targetPath)) return ToolResult::error("target file does not exist: " + pathutil::toUtf8(targetPath));
    if (!wasRead(sourcePath, ctx)) return ToolResult::error("refusing to move from a file you have not read or outlined: " + pathutil::toUtf8(sourcePath));
    if (!wasRead(targetPath, ctx)) return ToolResult::error("refusing to move into a file you have not read or outlined: " + pathutil::toUtf8(targetPath));

    std::string error;
    codestruct::TextFile sourceFile;
    if (!codestruct::readTextFile(sourcePath, sourceFile, error))
        return ToolResult::error(error + ": " + pathutil::toUtf8(sourcePath));
    if (endLine > static_cast<int>(sourceFile.lines.size()))
        return ToolResult::error("source line range out of range");

    fs::path sourceKey = canonicalKey(sourcePath);
    fs::path targetKey = canonicalKey(targetPath);
    const bool sameFile = sourceKey == targetKey;

    codestruct::TextFile targetFile;
    if (sameFile) {
        targetFile = sourceFile;
    } else if (!codestruct::readTextFile(targetPath, targetFile, error)) {
        return ToolResult::error(error + ": " + pathutil::toUtf8(targetPath));
    }

    int boundary = insertionBoundary(args, static_cast<int>(targetFile.lines.size()), error);
    if (boundary < 0) return ToolResult::error(error);

    const std::string actualHash = codestruct::hashLines(sourceFile.lines, startLine, endLine);
    if (args.contains("expected_hash")) {
        const std::string expected = args["expected_hash"].get<std::string>();
        if (!expected.empty() && expected != actualHash) {
            return ToolResult::error("source span hash mismatch: expected " + expected + ", got " + actualHash);
        }
    }

    if (sameFile && boundary >= startLine - 1 && boundary <= endLine)
        return ToolResult::error("target insertion point falls inside the moved span");

    const int spanLen = endLine - startLine + 1;
    std::vector<std::string> moved(
        sourceFile.lines.begin() + (startLine - 1),
        sourceFile.lines.begin() + endLine);

    if (dryRun) {
        std::ostringstream out;
        out << "would move " << spanLen << " line(s) from "
            << pathutil::toUtf8(sourcePath) << ":" << startLine << "-" << endLine
            << " to " << pathutil::toUtf8(targetPath)
            << " at boundary " << boundary
            << " hash=" << actualHash;
        return ToolResult::success(out.str());
    }

    snapshot(sourcePath, sourceFile.content, ctx);
    if (!sameFile) snapshot(targetPath, targetFile.content, ctx);

    sourceFile.lines.erase(sourceFile.lines.begin() + (startLine - 1),
                           sourceFile.lines.begin() + endLine);
    if (sameFile) {
        if (boundary > endLine) boundary -= spanLen;
        sourceFile.lines.insert(sourceFile.lines.begin() + boundary, moved.begin(), moved.end());
        pathutil::makeWritable(sourcePath, ctx.projectRoot);
        if (!codestruct::writeTextFile(sourcePath, sourceFile, error))
            return ToolResult::error(error + ": " + pathutil::toUtf8(sourcePath));
    } else {
        targetFile.lines.insert(targetFile.lines.begin() + boundary, moved.begin(), moved.end());
        pathutil::makeWritable(sourcePath, ctx.projectRoot);
        pathutil::makeWritable(targetPath, ctx.projectRoot);
        if (!codestruct::writeTextFile(sourcePath, sourceFile, error))
            return ToolResult::error(error + ": " + pathutil::toUtf8(sourcePath));
        if (!codestruct::writeTextFile(targetPath, targetFile, error))
            return ToolResult::error(error + ": " + pathutil::toUtf8(targetPath));
    }

    markRead(sourcePath, ctx);
    markRead(targetPath, ctx);

    std::ostringstream out;
    out << "moved " << spanLen << " line(s) from "
        << pathutil::toUtf8(sourcePath) << ":" << startLine << "-" << endLine
        << " to " << pathutil::toUtf8(targetPath)
        << " at boundary " << boundary
        << " hash=" << actualHash;
    return ToolResult::success(out.str());
}
