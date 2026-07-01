#include "CodeStructure.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace codestruct {

namespace {

constexpr size_t kBinarySniffBytes = 4096;

bool looksBinary(const std::string& sample) {
    for (char c : sample) {
        if (c == '\0') return true;
    }
    return false;
}

bool hasCrlfStyle(const std::string& content) {
    for (size_t i = 1; i < content.size(); ++i) {
        if (content[i] == '\n' && content[i - 1] == '\r') return true;
    }
    return false;
}

void normaliseLf(std::string& s) {
    size_t w = 0;
    for (size_t r = 0; r < s.size(); ++r) {
        if (s[r] == '\r' && r + 1 < s.size() && s[r + 1] == '\n') continue;
        s[w++] = s[r];
    }
    s.resize(w);
}

std::vector<std::string> splitLinesNoFinalEmpty(const std::string& content) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : content) {
        if (c == '\n') {
            lines.push_back(std::move(line));
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty() || content.empty() || content.back() != '\n')
        lines.push_back(std::move(line));
    if (content.empty()) lines.clear();
    return lines;
}

std::string rebuildContent(const TextFile& file) {
    std::string out;
    for (size_t i = 0; i < file.lines.size(); ++i) {
        out += file.lines[i];
        if (i + 1 < file.lines.size() || file.finalNewline)
            out += '\n';
    }
    if (file.crlf) {
        std::string crlf;
        crlf.reserve(out.size() + out.size() / 8);
        for (char c : out) {
            if (c == '\n') crlf += '\r';
            crlf += c;
        }
        out = std::move(crlf);
    }
    return out;
}

int braceDelta(const std::string& line) {
    int delta = 0;
    bool inString = false;
    bool inChar = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        char next = i + 1 < line.size() ? line[i + 1] : '\0';
        if (!inString && !inChar && c == '/' && next == '/') break;
        if (!inString && !inChar && c == '/' && next == '*') {
            ++i;
            while (i + 1 < line.size() && !(line[i] == '*' && line[i + 1] == '/')) ++i;
            if (i + 1 < line.size()) ++i;
            continue;
        }
        if (escaped) {
            escaped = false;
            continue;
        }
        if ((inString || inChar) && c == '\\') {
            escaped = true;
            continue;
        }
        if (!inChar && c == '"') {
            inString = !inString;
            continue;
        }
        if (!inString && c == '\'') {
            inChar = !inChar;
            continue;
        }
        if (inString || inChar) continue;
        if (c == '{') ++delta;
        if (c == '}') --delta;
    }
    return delta;
}

bool hasOpeningBrace(const std::string& line) {
    return braceDelta(line) > 0 || line.find('{') != std::string::npos;
}

int findBraceEndLine(const std::vector<std::string>& lines, int startLine) {
    int depth = 0;
    bool seenOpen = false;
    for (int lineNo = startLine; lineNo <= static_cast<int>(lines.size()); ++lineNo) {
        const std::string& line = lines[static_cast<size_t>(lineNo - 1)];
        for (char c : line) {
            if (c == '{') {
                ++depth;
                seenOpen = true;
            } else if (c == '}' && seenOpen) {
                --depth;
                if (depth <= 0) return lineNo;
            }
        }
    }
    return startLine;
}

std::string captureOrEmpty(const std::smatch& m, size_t index) {
    return index < m.size() && m[index].matched ? m[index].str() : "";
}

std::string unqualifiedName(std::string name) {
    name = trim(std::move(name));
    size_t pos = name.rfind("::");
    if (pos != std::string::npos) name = name.substr(pos + 2);
    return name;
}

std::string functionNameFromSignature(const std::string& sig) {
    size_t paren = sig.find('(');
    if (paren == std::string::npos) return {};
    std::string before = trim(sig.substr(0, paren));
    before = std::regex_replace(before, std::regex(R"(\s*<[^<>]*>\s*$)"), "");
    std::smatch m;
    if (std::regex_search(before, m, std::regex(R"((operator\s*[^\s]+|~?[A-Za-z_]\w*(?:::[~A-Za-z_]\w*)?)\s*$)")))
        return unqualifiedName(m[1].str());
    return {};
}

std::string fnv1a64(const std::string& data) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << h;
    return out.str();
}

}  // namespace

std::string trim(std::string s) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }).base(), s.end());
    return s;
}

bool readTextFile(const std::filesystem::path& path, TextFile& file, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not open for reading";
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    file.content = buf.str();

    std::string sniff = file.content.substr(0, std::min(file.content.size(), kBinarySniffBytes));
    if (looksBinary(sniff)) {
        error = "binary file";
        return false;
    }

    file.crlf = hasCrlfStyle(file.content);
    file.finalNewline = !file.content.empty() && file.content.back() == '\n';
    std::string normalised = file.content;
    normaliseLf(normalised);
    file.lines = splitLinesNoFinalEmpty(normalised);
    return true;
}

bool writeTextFile(const std::filesystem::path& path, const TextFile& file, std::string& error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "could not open for writing";
        return false;
    }
    std::string content = rebuildContent(file);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
        error = "write failed";
        return false;
    }
    return true;
}

std::string joinLines(const std::vector<std::string>& lines, int startLine, int endLine) {
    std::string out;
    if (startLine < 1 || endLine < startLine || endLine > static_cast<int>(lines.size()))
        return out;
    for (int lineNo = startLine; lineNo <= endLine; ++lineNo) {
        out += lines[static_cast<size_t>(lineNo - 1)];
        out += '\n';
    }
    return out;
}

std::string hashLines(const std::vector<std::string>& lines, int startLine, int endLine) {
    return fnv1a64(joinLines(lines, startLine, endLine));
}

std::vector<OutlineEntry> buildOutline(const TextFile& file) {
    static const std::regex namespaceRe(R"(^\s*namespace\s+(\w+)?\s*\{)");
    static const std::regex classRe(R"(^\s*(class|struct)\s+(\w+)(?:\s*:\s*[^{]+)?(?:\s*\{|\s*$))");
    static const std::regex enumRe(R"(^\s*enum\s+(?:class\s+)?(\w+)?\s*\{)");
    static const std::regex defineRe(R"(^\s*#\s*define\s+(\w+))");
    static const std::regex typedefRe(R"(^\s*typedef\s+.+\s+(\w+)\s*;)");
    static const std::regex usingRe(R"(^\s*using\s+(\w+)\s*=)");
    static const std::regex funcDefRe(R"(^\s*(?:template\s*<[^>]+>\s*)?(?:virtual\s+|static\s+|inline\s+|explicit\s+|constexpr\s+|const\s+)*(?!(?:if|while|for|switch|catch|return|delete|throw)\b)(?:(?:[A-Za-z_]\w*(?:::[A-Za-z_]\w*)?(?:\s*<[^>]*>)?[\s*&:<>]+)+)?~?[A-Za-z_]\w*(?:::[~A-Za-z_]\w*)?\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?\{)");
    static const std::regex funcDeclRe(R"(^\s*(?:virtual\s+|static\s+|inline\s+|explicit\s+|constexpr\s+|const\s+)*(?:(?:void|bool|int|char|float|double|long|unsigned|signed|short|auto|size_t|QString|QByteArray|std::[A-Za-z_]\w*|nlohmann::json|ToolResult)\s+[\w:~]+|(?:[\w:]+::)+~?\w+)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?(?:=\s*0\s*)?;)");

    std::vector<OutlineEntry> entries;
    int nesting = 0;
    for (int i = 0; i < static_cast<int>(file.lines.size()); ++i) {
        const int lineNo = i + 1;
        const std::string& line = file.lines[static_cast<size_t>(i)];
        std::string significant = trim(line);
        if (significant.empty() || significant.rfind("//", 0) == 0 || significant.rfind("/*", 0) == 0 || significant[0] == '*')
            continue;
        if (significant[0] == '#' && significant.find("#define") == std::string::npos)
            continue;

        std::smatch m;
        OutlineEntry e;
        e.startLine = lineNo;
        e.endLine = lineNo;
        e.nesting = std::max(0, nesting);

        if (std::regex_search(line, m, namespaceRe)) {
            e.kind = "namespace";
            e.name = captureOrEmpty(m, 1).empty() ? "{anonymous}" : captureOrEmpty(m, 1);
            e.signature = "namespace " + e.name;
        } else if (std::regex_search(line, m, classRe)) {
            e.kind = captureOrEmpty(m, 1);
            e.name = captureOrEmpty(m, 2);
            e.signature = e.kind + " " + e.name;
        } else if (std::regex_search(line, m, enumRe)) {
            e.kind = "enum";
            e.name = captureOrEmpty(m, 1).empty() ? "{anonymous}" : captureOrEmpty(m, 1);
            e.signature = "enum " + e.name;
        } else if (std::regex_search(line, m, defineRe)) {
            e.kind = "macro";
            e.name = captureOrEmpty(m, 1);
            e.signature = "#define " + e.name;
        } else if (std::regex_search(line, m, typedefRe) || std::regex_search(line, m, usingRe)) {
            e.kind = "typedef";
            e.name = captureOrEmpty(m, 1);
            e.signature = significant.substr(0, significant.find(';'));
        } else if (std::regex_search(line, funcDefRe) || std::regex_search(line, funcDeclRe)) {
            e.kind = line.find("::") != std::string::npos ? "method" : "function";
            e.signature = significant;
            if (!e.signature.empty() && (e.signature.back() == '{' || e.signature.back() == ';'))
                e.signature.pop_back();
            e.signature = trim(e.signature);
            e.name = functionNameFromSignature(e.signature);
            if (e.name.empty()) e.kind.clear();
        }

        if (!e.kind.empty()) {
            if (hasOpeningBrace(line))
                e.endLine = findBraceEndLine(file.lines, lineNo);
            e.hash = hashLines(file.lines, e.startLine, e.endLine);
            entries.push_back(std::move(e));
        }

        nesting += braceDelta(line);
        if (nesting < 0) nesting = 0;
    }
    return entries;
}

std::optional<OutlineEntry> findSymbol(const std::vector<OutlineEntry>& entries,
                                       const std::string& symbol,
                                       std::string* error) {
    std::vector<OutlineEntry> matches;
    for (const auto& e : entries) {
        if (e.name == symbol || e.signature.find(symbol) != std::string::npos)
            matches.push_back(e);
    }
    if (matches.empty()) {
        if (error) *error = "symbol not found: " + symbol;
        return std::nullopt;
    }
    if (matches.size() > 1) {
        if (error) {
            std::ostringstream out;
            out << "symbol is ambiguous: " << symbol << "\n";
            for (const auto& e : matches)
                out << e.kind << " " << e.startLine << "-" << e.endLine << " " << e.signature << "\n";
            *error = out.str();
        }
        return std::nullopt;
    }
    return matches.front();
}

}  // namespace codestruct
