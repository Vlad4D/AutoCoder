#include "FindCalleesTool.h"

#include <filesystem>
#include <map>
#include <regex>
#include <set>
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

std::string stripCommentsAndStrings(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    bool inString = false;
    bool inChar = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        char next = i + 1 < line.size() ? line[i + 1] : '\0';
        if (!inString && !inChar && c == '/' && next == '/') break;
        if (escaped) {
            out += ' ';
            escaped = false;
            continue;
        }
        if ((inString || inChar) && c == '\\') {
            out += ' ';
            escaped = true;
            continue;
        }
        if (!inChar && c == '"') {
            inString = !inString;
            out += ' ';
            continue;
        }
        if (!inString && c == '\'') {
            inChar = !inChar;
            out += ' ';
            continue;
        }
        out += (inString || inChar) ? ' ' : c;
    }
    return out;
}

bool isIgnoredCallName(const std::string& name) {
    static const std::set<std::string> ignored = {
        "if", "for", "while", "switch", "catch", "return", "sizeof",
        "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
        "new", "delete", "emit", "connect", "SIGNAL", "SLOT"
    };
    return ignored.find(name) != ignored.end();
}

std::string normalizeCallName(std::string name) {
    size_t pos = name.rfind("::");
    if (pos != std::string::npos) name = name.substr(pos + 2);
    pos = name.rfind('.');
    if (pos != std::string::npos) name = name.substr(pos + 1);
    pos = name.rfind("->");
    if (pos != std::string::npos) name = name.substr(pos + 2);
    return name;
}

}  // namespace

json FindCalleesTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "find_callees"},
            {"description",
             "Find likely function/method calls made inside a function span. "
             "Accepts either a symbol name or an explicit line range."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path",       {{"type", "string"},  {"description", "Absolute or project-relative source file path."}}},
                    {"function",   {{"type", "string"},  {"description", "Optional function/method name or signature fragment."}}},
                    {"start_line", {{"type", "integer"}, {"description", "Optional 1-based first line of the range."}}},
                    {"end_line",   {{"type", "integer"}, {"description", "Optional 1-based last line of the range."}}}
                }},
                {"required", json::array({"path"})}
            }}
        }}
    };
}

ToolResult FindCalleesTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("path")) return ToolResult::error("missing required arg: path");
    fs::path path = pathutil::resolveSafely(args["path"].get<std::string>(), ctx);
    if (path.empty()) return ToolResult::error(
        "refusing to read outside project root: " + args["path"].get<std::string>());

    codestruct::TextFile file;
    std::string error;
    if (!codestruct::readTextFile(path, file, error))
        return ToolResult::error(error + ": " + pathutil::toUtf8(path));

    if (ctx.readSet) ctx.readSet->insert(canonicalKey(path));

    int startLine = args.value("start_line", 0);
    int endLine = args.value("end_line", 0);
    std::string label;
    if (args.contains("function")) {
        auto entries = codestruct::buildOutline(file);
        auto entry = codestruct::findSymbol(entries, args["function"].get<std::string>(), &error);
        if (!entry) return ToolResult::error(error);
        startLine = entry->startLine;
        endLine = entry->endLine;
        label = entry->signature;
    } else {
        label = pathutil::toUtf8(path) + ":" + std::to_string(startLine) + "-" + std::to_string(endLine);
    }

    if (startLine < 1 || endLine < startLine || endLine > static_cast<int>(file.lines.size()))
        return ToolResult::error("provide a valid function or start_line/end_line range");

    struct Hit {
        int count = 0;
        int firstLine = 0;
    };
    std::map<std::string, Hit> hits;
    std::regex callRe(R"(((?:[A-Za-z_]\w*(?:::|\.|->))*[A-Za-z_]\w*)\s*\()");

    for (int lineNo = startLine; lineNo <= endLine; ++lineNo) {
        std::string line = stripCommentsAndStrings(file.lines[static_cast<size_t>(lineNo - 1)]);
        auto begin = std::sregex_iterator(line.begin(), line.end(), callRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string name = normalizeCallName((*it)[1].str());
            if (name.empty() || isIgnoredCallName(name)) continue;
            auto& hit = hits[name];
            ++hit.count;
            if (hit.firstLine == 0) hit.firstLine = lineNo;
        }
    }

    std::ostringstream out;
    out << "=== Callees: " << label << " ===\n";
    if (hits.empty()) {
        out << "[no likely callees found]\n";
    } else {
        for (const auto& [name, hit] : hits) {
            out << name << " count=" << hit.count
                << " first_line=" << hit.firstLine << "\n";
        }
    }
    out << "(" << hits.size() << " unique likely callee(s), heuristic scan)\n";
    return ToolResult::success(out.str());
}
