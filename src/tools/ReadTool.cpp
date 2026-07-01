#include "ReadTool.h"

#include <fstream>
#include <iomanip>
#include <sstream>

#include "IgnoreRules.h"
#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kDefaultLimit = 2000;
constexpr size_t kBinarySniffBytes = 8192;

bool looksBinary(const std::string& sample) {
    for (char c : sample) {
        if (c == '\0') return true;
    }
    return false;
}

}  // namespace

json ReadTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "read"},
            {"description",
             "Read a text file. Returns line-numbered output. Use offset/limit "
             "to read a slice of large files."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path",   {{"type", "string"},  {"description", "Absolute or project-relative path."}}},
                    {"offset", {{"type", "integer"}, {"description", "0-based first line to include. Default 0."}}},
                    {"limit",  {{"type", "integer"}, {"description", "Max lines to return. Default 2000."}}}
                }},
                {"required", json::array({"path"})}
            }}
        }}
    };
}

ToolResult ReadTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("path")) return ToolResult::error("missing required arg: path");
    const std::string pathArg = args["path"].get<std::string>();
    int offset = args.value("offset", 0);
    int limit = args.value("limit", kDefaultLimit);

    fs::path path = pathutil::resolveSafely(pathArg, ctx);
    if (path.empty()) return ToolResult::error(
        "refusing to read outside project root: " + pathArg);
    if (pathutil::IgnoreRules::load(ctx.projectRoot).isIgnored(path)) {
        return ToolResult::error(
            "refusing to read ignored file (matches .gitignore/.autocoderignore or a "
            "built-in secret pattern; its contents are not sent to the LLM): " + pathArg);
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) return ToolResult::error("file does not exist: " + pathutil::toUtf8(path));
    if (!fs::is_regular_file(path, ec)) return ToolResult::error("not a regular file: " + pathutil::toUtf8(path));

    std::ifstream in(path, std::ios::binary);
    if (!in) return ToolResult::error("could not open: " + pathutil::toUtf8(path));

    // Sniff for binary content
    std::string sniff(kBinarySniffBytes, '\0');
    in.read(sniff.data(), kBinarySniffBytes);
    sniff.resize(in.gcount());
    if (looksBinary(sniff)) {
        return ToolResult::success("[binary file, " + std::to_string(fs::file_size(path, ec)) + " bytes, contents not shown]");
    }
    // Also check the rest of the file for NULs (large text files might have
    // binary data after the initial sniff window).
    std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (looksBinary(rest)) {
        return ToolResult::success("[binary file, " + std::to_string(fs::file_size(path, ec)) + " bytes, contents not shown]");
    }
    // Reconstruct full content from the sniff + rest so we can return all lines.
    std::string fullContent = sniff + rest;
    in.clear();
    std::istringstream fullIn(fullContent);

    std::ostringstream out;
    std::string line;
    int lineno = 0;
    int emitted = 0;
    while (std::getline(fullIn, line)) {
        ++lineno;
        // Strip trailing carriage-return if present (CRLF -> LF normalisation).
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (lineno <= offset) continue;
        if (emitted >= limit) {
            out << "[... truncated; " << limit << "-line limit reached]\n";
            break;
        }
        out << std::setw(6) << lineno << "\t" << line << "\n";
        ++emitted;
    }

    if (ctx.readSet) {
        std::error_code ec2;
        fs::path canonical = fs::weakly_canonical(path, ec2);
        ctx.readSet->insert(canonical.empty() ? path : canonical);
    }

    std::string s = out.str();
    if (s.empty()) s = "[empty file]";
    return ToolResult::success(std::move(s));
}
