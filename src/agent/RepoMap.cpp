#include "RepoMap.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "diagnostics/TraceTimer.h"
#include "tools/IgnoreRules.h"
#include "tools/PathUtil.h"

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxFilesListed = 2000;     // hard cap on map entries

}  // namespace

namespace repomap {

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

        std::vector<std::string> relPaths;
        relPaths.reserve(files.size());
        for (const fs::path& f : files) {
            std::error_code ec3;
            std::string rel = pathutil::toUtf8(fs::relative(f, projectRoot, ec3));
            if (ec3 || rel.empty()) continue;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            relPaths.push_back(std::move(rel));
        }
        // Stable order: keeps the map byte-identical across rebuilds of an
        // unchanged tree (and therefore provider-cache-friendly).
        std::sort(relPaths.begin(), relPaths.end());

        std::ostringstream o;
        o << "Project map -- one line per file. "
             "Generated at conversation start, so it may be "
             "stale; verify with glob/read before relying on details:\n";
        std::size_t used = static_cast<std::size_t>(o.tellp());

        for (const std::string& rel : relPaths) {
            std::string line = rel + "\n";
            if (used + line.size() > maxBytes) {
                o << "[project map truncated]\n";
                return o.str();
            }
            o << line;
            used += line.size();
        }
        if (listTruncated) {
            o << "[file list truncated at " << kMaxFilesListed << " files]\n";
        }
        return o.str();
    } catch (...) {
        return {};
    }
}

}  // namespace repomap
