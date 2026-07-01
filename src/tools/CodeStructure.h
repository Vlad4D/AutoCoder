#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace codestruct {

struct TextFile {
    std::string content;
    std::vector<std::string> lines;
    bool crlf = false;
    bool finalNewline = false;
};

struct OutlineEntry {
    int startLine = 0;
    int endLine = 0;
    int nesting = 0;
    std::string kind;
    std::string name;
    std::string signature;
    std::string hash;
};

bool readTextFile(const std::filesystem::path& path, TextFile& file, std::string& error);
bool writeTextFile(const std::filesystem::path& path, const TextFile& file, std::string& error);

std::string hashLines(const std::vector<std::string>& lines, int startLine, int endLine);
std::string joinLines(const std::vector<std::string>& lines, int startLine, int endLine);

std::vector<OutlineEntry> buildOutline(const TextFile& file);
std::optional<OutlineEntry> findSymbol(const std::vector<OutlineEntry>& entries,
                                       const std::string& symbol,
                                       std::string* error = nullptr);

std::string trim(std::string s);

}  // namespace codestruct
