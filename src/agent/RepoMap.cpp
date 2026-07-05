#include "RepoMap.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "diagnostics/TraceTimer.h"
#include "tools/CodeStructure.h"
#include "tools/IgnoreRules.h"
#include "tools/PathUtil.h"

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxFilesListed = 2000;     // hard cap on map entries
constexpr std::size_t kMaxEnrichedFiles = 400;    // outline-parse budget per build
constexpr std::uintmax_t kMaxSourceBytes = 512 * 1024;  // skip huge sources

bool isCFamilySource(const fs::path& p) {
    std::string ext = pathutil::toUtf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx"
        || ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx";
}

}  // namespace

namespace repomap {

std::string symbolSuffixForFile(const fs::path& file, std::size_t maxChars) {
    try {
        if (!isCFamilySource(file)) return {};
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(file, ec);
        if (ec || size > kMaxSourceBytes) return {};

        codestruct::TextFile tf;
        std::string err;
        if (!codestruct::readTextFile(file, tf, err)) return {};
        const std::vector<codestruct::OutlineEntry> entries =
            codestruct::buildOutline(tf);

        // Container spans (class/struct/enum bodies): members inside them are
        // omitted so a header maps to "class Foo", not every method of Foo.
        std::vector<std::pair<int, int>> containers;
        for (const auto& e : entries) {
            if (e.kind == "class" || e.kind == "struct" || e.kind == "enum")
                containers.emplace_back(e.startLine, e.endLine);
        }
        auto insideContainer = [&containers](int line) {
            for (const auto& s : containers)
                if (line > s.first && line <= s.second) return true;
            return false;
        };

        std::string out;
        std::set<std::string> seen;
        for (const auto& e : entries) {
            if (e.name.empty()) continue;
            std::string item;
            if (e.kind == "function" || e.kind == "method") {
                item = e.name + "()";
            } else if (e.kind == "class" || e.kind == "struct"
                       || e.kind == "enum") {
                item = e.kind + " " + e.name;
            } else {
                continue;  // namespaces/macros/typedefs: low signal per byte
            }
            if (insideContainer(e.startLine)) continue;
            if (!seen.insert(item).second) continue;  // overloads collapse

            const std::size_t sep = out.empty() ? 0 : 2;
            if (out.size() + sep + item.size() > maxChars) {
                out += ", ...";
                break;
            }
            if (sep) out += ", ";
            out += item;
        }
        if (out.empty()) return {};
        return ": " + out;
    } catch (...) {
        return {};
    }
}

std::string build(const fs::path& projectRoot, std::size_t maxBytes) {
    diagnostics::TraceTimer timer("repomap::build");
    try {
        std::error_code ec;
        if (!fs::is_directory(projectRoot, ec) || ec) return {};

        const pathutil::IgnoreRules ignore = pathutil::IgnoreRules::load(projectRoot);

        std::vector<fs::path> files;
        bool listTruncated = false;
        fs::recursive_directory_iterator it(
            projectRoot, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (it != end && !ec) {
            const fs::directory_entry& entry = *it;
            std::error_code ec2;
            if (entry.is_directory(ec2)) {
                const std::string dirName = pathutil::toUtf8(entry.path().filename());
                if (pathutil::isExcludedDir(dirName) || ignore.isIgnored(entry.path(), true)) {
                    it.disable_recursion_pending();
                }
            } else if (entry.is_regular_file(ec2)) {
                if (!ignore.isIgnored(entry.path())) {
                    files.push_back(entry.path());
                    if (files.size() >= kMaxFilesListed) {
                        listTruncated = true;
                        break;
                    }
                }
            }
            (void)it.increment(ec);
        }
        if (files.empty()) return {};

        std::vector<std::pair<std::string, fs::path>> listing;
        listing.reserve(files.size());
        for (fs::path& f : files) {
            std::error_code ec3;
            std::string rel = pathutil::toUtf8(fs::relative(f, projectRoot, ec3));
            if (ec3 || rel.empty()) continue;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            listing.emplace_back(std::move(rel), std::move(f));
        }
        // Stable order: keeps the map byte-identical across rebuilds of an
        // unchanged tree (and therefore provider-cache-friendly).
        std::sort(listing.begin(), listing.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        const std::string header =
            "Project map -- one line per file, with top-level symbols for "
            "C/C++ sources. Generated at conversation start, so it may be "
            "stale; verify with glob/read before relying on details:\n";

        // Pass 1: plain path lines under the byte cap. Path coverage always
        // wins over symbols, so this pass ignores enrichment entirely.
        std::size_t used = header.size();
        std::size_t listed = 0;
        bool mapTruncated = false;
        for (const auto& [rel, abs] : listing) {
            const std::size_t lineSize = rel.size() + 1;  // trailing '\n'
            if (used + lineSize > maxBytes) {
                mapTruncated = true;
                break;
            }
            used += lineSize;
            ++listed;
        }

        // Pass 2: spend whatever budget is left on symbol suffixes, in the
        // same deterministic order. A suffix that does not fit is skipped
        // (later, shorter ones may still fit).
        std::vector<std::string> suffixes(listed);
        std::size_t leftover = maxBytes - used;
        std::size_t parsed = 0;
        for (std::size_t i = 0; i < listed && parsed < kMaxEnrichedFiles
                                && leftover > 0; ++i) {
            if (!isCFamilySource(listing[i].second)) continue;
            ++parsed;
            std::string suffix = symbolSuffixForFile(listing[i].second);
            if (suffix.empty() || suffix.size() > leftover) continue;
            leftover -= suffix.size();
            suffixes[i] = std::move(suffix);
        }

        std::ostringstream o;
        o << header;
        for (std::size_t i = 0; i < listed; ++i) {
            o << listing[i].first << suffixes[i] << "\n";
        }
        if (mapTruncated) {
            o << "[project map truncated]\n";
        } else if (listTruncated) {
            o << "[file list truncated at " << kMaxFilesListed << " files]\n";
        }
        return o.str();
    } catch (...) {
        return {};
    }
}

}  // namespace repomap
