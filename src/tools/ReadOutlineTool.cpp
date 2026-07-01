#include "ReadOutlineTool.h"

#include <filesystem>
#include <sstream>

#include "CodeStructure.h"
#include "IgnoreRules.h"
#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path canonicalKey(const fs::path& path) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    return canonical.empty() ? path : canonical;
}

}  // namespace

json ReadOutlineTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "read_outline"},
            {"description",
             "Show a source file outline with symbols, spans, and stable span hashes. "
             "Use this before move_span or move_func to avoid sending large code bodies."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Absolute or project-relative path."}}}
                }},
                {"required", json::array({"path"})}
            }}
        }}
    };
}

ToolResult ReadOutlineTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("path")) return ToolResult::error("missing required arg: path");
    const std::string pathArg = args["path"].get<std::string>();

    fs::path path = pathutil::resolveSafely(pathArg, ctx);
    if (path.empty()) return ToolResult::error(
        "refusing to read outline outside project root: " + pathArg);
    if (pathutil::IgnoreRules::load(ctx.projectRoot).isIgnored(path)) {
        return ToolResult::error(
            "refusing to outline ignored file (matches .gitignore/.autocoderignore or a "
            "built-in secret pattern): " + pathArg);
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) return ToolResult::error("file does not exist: " + pathutil::toUtf8(path));
    if (!fs::is_regular_file(path, ec)) return ToolResult::error("not a regular file: " + pathutil::toUtf8(path));

    codestruct::TextFile file;
    std::string error;
    if (!codestruct::readTextFile(path, file, error)) {
        if (error == "binary file") return ToolResult::success("[binary file, outline not available]");
        return ToolResult::error(error + ": " + pathutil::toUtf8(path));
    }

    if (ctx.readSet) ctx.readSet->insert(canonicalKey(path));

    std::vector<codestruct::OutlineEntry> entries = codestruct::buildOutline(file);

    std::ostringstream out;
    out << "=== Outline: " << pathutil::toUtf8(path) << " ===\n";
    if (entries.empty()) {
        out << "[no structural elements found in this file]\n";
    } else {
        for (const auto& e : entries) {
            for (int i = 0; i < e.nesting && i < 20; ++i)
                out << "  ";
            out << e.kind << " " << e.startLine << "-" << e.endLine
                << " hash=" << e.hash;
            if (!e.name.empty())
                out << " name=" << e.name;
            out << ": " << e.signature << "\n";
        }
    }

    out << "(file has " << file.lines.size() << " lines, "
        << entries.size() << " structural elements shown)\n";
    return ToolResult::success(out.str());
}
