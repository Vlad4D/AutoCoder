#include "MoveFuncTool.h"

#include <filesystem>
#include <optional>
#include <sstream>

#include "CodeStructure.h"
#include "MoveSpanTool.h"
#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool isMovableKind(const std::string& kind) {
    return kind == "function" || kind == "method";
}

std::optional<codestruct::OutlineEntry> findMovableSymbol(
    const std::vector<codestruct::OutlineEntry>& entries,
    const std::string& symbol,
    std::string* error) {
    std::vector<codestruct::OutlineEntry> funcs;
    for (const auto& e : entries) {
        if (isMovableKind(e.kind)) funcs.push_back(e);
    }
    return codestruct::findSymbol(funcs, symbol, error);
}

}  // namespace

json MoveFuncTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "move_func"},
            {"description",
             "Move a function or method by symbol name without returning the moved code. "
             "Use read_outline first if a symbol may be ambiguous."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"symbol",              {{"type", "string"},  {"description", "Function or method name/signature fragment to move."}}},
                    {"source_path",         {{"type", "string"},  {"description", "Absolute or project-relative source file path."}}},
                    {"target_path",         {{"type", "string"},  {"description", "Absolute or project-relative target file path."}}},
                    {"insert_after_symbol", {{"type", "string"},  {"description", "Optional target symbol after which the function should be inserted."}}},
                    {"insert_line",         {{"type", "integer"}, {"description", "Optional target insertion line anchor, used when insert_after_symbol is omitted."}}},
                    {"position",            {{"type", "string"},  {"enum", json::array({"before", "after"})}, {"description", "Insert before or after insert_line. Defaults to before; ignored for insert_after_symbol, which inserts after."}}},
                    {"expected_hash",       {{"type", "string"},  {"description", "Optional hash of the source function span from read_outline."}}},
                    {"dry_run",             {{"type", "boolean"}, {"description", "Validate and report the move without writing files."}}}
                }},
                {"required", json::array({"symbol", "source_path", "target_path"})}
            }}
        }}
    };
}

ToolResult MoveFuncTool::execute(const json& args, ToolContext& ctx) {
    for (const char* key : {"symbol", "source_path", "target_path"}) {
        if (!args.contains(key)) return ToolResult::error(std::string("missing required arg: ") + key);
    }
    if (!args.contains("insert_after_symbol") && !args.contains("insert_line"))
        return ToolResult::error("missing insertion target: provide insert_after_symbol or insert_line");

    const std::string symbol = args["symbol"].get<std::string>();
    fs::path sourcePath = pathutil::resolveSafely(args["source_path"].get<std::string>(), ctx);
    if (sourcePath.empty()) return ToolResult::error(
        "refusing to move from outside project root: " + args["source_path"].get<std::string>());
    fs::path targetPath = pathutil::resolveSafely(args["target_path"].get<std::string>(), ctx);
    if (targetPath.empty()) return ToolResult::error(
        "refusing to move to outside project root: " + args["target_path"].get<std::string>());

    std::string error;
    codestruct::TextFile sourceFile;
    if (!codestruct::readTextFile(sourcePath, sourceFile, error))
        return ToolResult::error(error + ": " + pathutil::toUtf8(sourcePath));
    auto sourceEntries = codestruct::buildOutline(sourceFile);
    auto sourceEntry = findMovableSymbol(sourceEntries, symbol, &error);
    if (!sourceEntry) return ToolResult::error(error);

    if (args.contains("expected_hash")) {
        const std::string expected = args["expected_hash"].get<std::string>();
        if (!expected.empty() && expected != sourceEntry->hash)
            return ToolResult::error("source function hash mismatch: expected " + expected + ", got " + sourceEntry->hash);
    }

    int insertLine = args.value("insert_line", 0);
    std::string position = args.value("position", "before");
    if (args.contains("insert_after_symbol")) {
        codestruct::TextFile targetFile;
        if (!codestruct::readTextFile(targetPath, targetFile, error))
            return ToolResult::error(error + ": " + pathutil::toUtf8(targetPath));
        auto targetEntries = codestruct::buildOutline(targetFile);
        auto targetEntry = codestruct::findSymbol(targetEntries, args["insert_after_symbol"].get<std::string>(), &error);
        if (!targetEntry) return ToolResult::error(error);
        insertLine = targetEntry->endLine;
        position = "after";
    }

    json moveArgs = {
        {"source_path", args["source_path"]},
        {"start_line", sourceEntry->startLine},
        {"end_line", sourceEntry->endLine},
        {"target_path", args["target_path"]},
        {"insert_line", insertLine},
        {"position", position},
        {"expected_hash", sourceEntry->hash},
        {"dry_run", args.value("dry_run", false)}
    };

    MoveSpanTool mover;
    ToolResult result = mover.execute(moveArgs, ctx);
    if (!result.ok) return result;

    std::ostringstream out;
    out << "move_func resolved " << symbol << " to "
        << sourceEntry->kind << " " << sourceEntry->startLine << "-"
        << sourceEntry->endLine << " hash=" << sourceEntry->hash << "\n"
        << result.content;
    return ToolResult::success(out.str());
}
