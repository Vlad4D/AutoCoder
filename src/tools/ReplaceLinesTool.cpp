#include "ReplaceLinesTool.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "PathUtil.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Detect the dominant line-ending style: true if CRLF, false if LF-only.
bool hasCrlfStyle(const std::string& content) {
    for (size_t i = 1; i < content.size(); ++i) {
        if (content[i] == '\n' && content[i - 1] == '\r')
            return true;
    }
    return false;
}

// Normalise line endings in `s` from CRLF -> LF in-place.
void normaliseLf(std::string& s) {
    size_t w = 0;
    for (size_t r = 0; r < s.size(); ++r) {
        if (s[r] == '\r' && (r + 1 < s.size() && s[r + 1] == '\n'))
            continue;
        s[w++] = s[r];
    }
    s.resize(w);
}

// Normalise line endings in `s` from LF -> CRLF in-place.
void normaliseCrlf(std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 2);
    for (char c : s) {
        if (c == '\n')
            out.push_back('\r');
        out.push_back(c);
    }
    s = std::move(out);
}

// Split `content` into lines (LF-normalised). The line content does NOT
// include the trailing newline character.
std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : content) {
        if (c == '\n') {
            lines.push_back(std::move(line));
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty() || (!content.empty() && content.back() == '\n'))
        lines.push_back(std::move(line));
    return lines;
}

}  // namespace

json ReplaceLinesTool::schema() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", "replace_lines"},
            {"description",
             "Replace a line range in a file that was read earlier in this conversation. "
             "Line numbers are 1-based and inclusive. Useful when exact text matching "
             "would be fragile; line endings are handled automatically."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"path",       {{"type", "string"},  {"description", "Absolute or project-relative path."}}},
                    {"start_line", {{"type", "integer"}, {"description", "1-based line number where replacement begins (inclusive)."}}},
                    {"end_line",   {{"type", "integer"}, {"description", "1-based line number where replacement ends; use start_line-1 to insert."}}},
                    {"new_lines",  {{"type", "string"},  {"description", "Replacement text."}}}
                }},
                {"required", json::array({"path", "start_line", "end_line", "new_lines"})}
            }}
        }}
    };
}

ToolResult ReplaceLinesTool::execute(const json& args, ToolContext& ctx) {
    if (!args.contains("path"))       return ToolResult::error("missing required arg: path");
    if (!args.contains("start_line")) return ToolResult::error("missing required arg: start_line");
    if (!args.contains("end_line"))   return ToolResult::error("missing required arg: end_line");
    if (!args.contains("new_lines"))  return ToolResult::error("missing required arg: new_lines");

    const std::string pathArg = args["path"].get<std::string>();
    int startLine = args["start_line"].get<int>();
    int endLine   = args["end_line"].get<int>();
    std::string newLines = args["new_lines"].get<std::string>();

    if (startLine < 1) return ToolResult::error("start_line must be >= 1");
    if (endLine < startLine - 1)
        return ToolResult::error("end_line must be >= start_line - 1");

    fs::path path = pathutil::resolveSafely(pathArg, ctx);
    if (path.empty()) return ToolResult::error(
        "refusing to modify outside project root: " + pathArg);
    if (!fs::exists(path)) return ToolResult::error("file does not exist: " + pathutil::toUtf8(path));

    // Enforce read-before-write.
    if (ctx.readSet) {
        std::error_code ec2;
        fs::path canonical = fs::weakly_canonical(path, ec2);
        const fs::path& key = canonical.empty() ? path : canonical;
        if (ctx.readSet->find(key) == ctx.readSet->end()) {
            return ToolResult::error(
                "refusing to modify a file you have not read in this conversation: "
                + pathutil::toUtf8(path) + ". Call `read` on it first.");
        }
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) return ToolResult::error("could not open for reading: " + pathutil::toUtf8(path));
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string content = buf.str();
    in.close();

    // Snapshot for checkpoint before modifying.
    if (ctx.fileBeforeSnapshots) {
        std::error_code ec3;
        fs::path canonical2 = fs::weakly_canonical(path, ec3);
        const fs::path& key2 = canonical2.empty() ? path : canonical2;
        if (ctx.fileBeforeSnapshots->find(key2) == ctx.fileBeforeSnapshots->end()) {
            (*ctx.fileBeforeSnapshots)[key2] = content;
        }
    }

    const bool originalIsCrlf = hasCrlfStyle(content);
    normaliseLf(content);

    auto lines = splitLines(content);
    // lines.size() is the number of lines. If the file ends with a newline,
    // there's a trailing empty string in the vector representing an empty
    // last line. The actual logical line count is lines.size().
    int totalLines = static_cast<int>(lines.size());

    // end_line == start_line - 1 means "insert before start_line, remove nothing".
    bool isInsertion = (endLine == startLine - 1);
    if (!isInsertion && (startLine > totalLines || endLine > totalLines)) {
        return ToolResult::error(
            "line numbers out of range. File has " + std::to_string(totalLines)
            + " lines, but requested " + std::to_string(startLine) + "-"
            + std::to_string(endLine));
    }

    // Build the new content.
    std::string result;
    result.reserve(content.size() + newLines.size());

    // Lines before the replacement range (1-based -> 0-based).
    int insertIdx = isInsertion ? (startLine - 1) : (startLine - 1);
    for (int i = 0; i < insertIdx; ++i) {
        result += lines[i];
        result += '\n';
    }

    // Insert the new text (normalised to LF).
    normaliseLf(newLines);
    result += newLines;
    if (!newLines.empty() && newLines.back() != '\n')
        result += '\n';

    // Lines after the replacement range.
    int afterIdx = isInsertion ? (startLine - 1) : endLine;
    for (int i = afterIdx; i < totalLines; ++i) {
        result += lines[i];
        // Append '\n' after each line except the very last trailing-empty line
        // (which represents the original file ending with '\n').
        if (i < totalLines - 1 || !lines[i].empty() || (!content.empty() && content.back() != '\n'))
            result += '\n';
    }

    // Restore original line-ending style.
    if (originalIsCrlf)
        normaliseCrlf(result);

    pathutil::makeWritable(path, ctx.projectRoot);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return ToolResult::error("could not open for writing: " + pathutil::toUtf8(path));
    out.write(result.data(), static_cast<std::streamsize>(result.size()));
    if (!out) return ToolResult::error("write failed: " + pathutil::toUtf8(path));

    // Mark as read so future edits/replace work.
    if (ctx.readSet) {
        std::error_code ec4;
        fs::path canonical = fs::weakly_canonical(path, ec4);
        ctx.readSet->insert(canonical.empty() ? path : canonical);
    }

    int linesRemoved = isInsertion ? 0 : (endLine - startLine + 1);
    // newLines is LF-normalised here; an optional trailing newline does not
    // add a line.
    int linesInserted = newLines.empty() ? 0
        : static_cast<int>(std::count(newLines.begin(), newLines.end(), '\n'))
          + (newLines.back() == '\n' ? 0 : 1);

    // Embed old/new content in the output so the UI can render a diff preview.
    // Include a few unchanged context lines on each side of the modified range
    // so the change is shown in the context of the surrounding code. The same
    // context is added to both blocks, so the diff renders it as unchanged.
    constexpr int kContextLines = 3;
    auto joinRange = [&](int from, int to) {
        std::string s;
        for (int i = from; i < to && i < totalLines; ++i) {
            if (i < 0) continue;
            s += lines[i];
            if (i < totalLines - 1 || !lines[i].empty())
                s += '\n';
        }
        return s;
    };

    const std::string ctxBefore =
        joinRange(std::max(0, (startLine - 1) - kContextLines), startLine - 1);
    const std::string ctxAfter = joinRange(afterIdx, afterIdx + kContextLines);

    std::string oldMid;
    if (!isInsertion) oldMid = joinRange(startLine - 1, endLine);

    std::string newMid = newLines;
    if (!newMid.empty() && newMid.back() != '\n') newMid += '\n';

    const std::string oldBlock = ctxBefore + oldMid + ctxAfter;
    const std::string newBlock = ctxBefore + newMid + ctxAfter;

    // Post-edit line numbering, so the model can chain further replace_lines
    // calls without re-reading the file to recover shifted line numbers.
    const int lineDelta = linesInserted - linesRemoved;
    std::string numbering;
    if (linesInserted > 0) {
        numbering = ". New content now spans lines " + std::to_string(startLine)
                  + "-" + std::to_string(startLine + linesInserted - 1);
    }
    if (lineDelta != 0) {
        numbering += ". Lines below the edit shifted by "
                   + std::string(lineDelta > 0 ? "+" : "")
                   + std::to_string(lineDelta);
    }

    std::string output = "=== OLD CONTENT ===\n"
                       + oldBlock
                       + "=== NEW CONTENT ===\n"
                       + newBlock
                       + "=== RESULT ===\n"
                       + "replaced lines " + std::to_string(startLine) + "-"
                       + std::to_string(endLine) + " in " + pathutil::toUtf8(path)
                       + " (" + std::to_string(linesRemoved) + " line(s) removed, "
                       + std::to_string(linesInserted) + " line(s) inserted)"
                       + numbering;
    return ToolResult::success(output);
}
