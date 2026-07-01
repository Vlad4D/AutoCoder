#include "ConversationStore.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

#include "diagnostics/TraceTimer.h"
#include "tools/PathUtil.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string toBase64(const std::string& bytes) {
    const QByteArray raw(bytes.data(), static_cast<qsizetype>(bytes.size()));
    const QByteArray encoded = raw.toBase64(QByteArray::Base64Encoding);
    return std::string(encoded.constData(), static_cast<size_t>(encoded.size()));
}

std::string fromBase64(const std::string& encoded) {
    const QByteArray input(encoded.data(), static_cast<qsizetype>(encoded.size()));
    const QByteArray decoded = QByteArray::fromBase64(input, QByteArray::Base64Encoding);
    return std::string(decoded.constData(), static_cast<size_t>(decoded.size()));
}

json fileSnapshotToJson(const std::string& path, const std::string& content) {
    return {
        {"path", path},
        {"content_encoding", "base64"},
        {"content_b64", toBase64(content)}
    };
}

std::string fileSnapshotContentFromJson(const json& fj) {
    if (fj.contains("content_b64") && fj["content_b64"].is_string()) {
        return fromBase64(fj["content_b64"].get<std::string>());
    }

    // Backward compatibility for checkpoints written before snapshots were
    // encoded. Those files only existed when the old content happened to be
    // valid UTF-8.
    return fj.value("content", "");
}

constexpr const char* kCompactSummaryPrefix = "[Previous conversation compacted]";

bool isCompactSummaryContent(const std::string& text) {
    return text.rfind(kCompactSummaryPrefix, 0) == 0;
}

std::string formatTitle(std::string text) {
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r') ch = ' ';
    }
    if (text.size() > 60) text = text.substr(0, 57) + "\u2026";
    return text;
}

}  // namespace

// Atomic file write: write to `<file>.tmp`, flush, then rename over target.
// This prevents truncated files on crash/power loss.
static void atomicWriteJson(const fs::path& file, const json& j) {
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("could not write: " + pathutil::toUtf8(tmp));
        out << j.dump(2);
        out.flush();
        if (!out) throw std::runtime_error("write failed: " + pathutil::toUtf8(tmp));
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (!ec) return;

#if defined(_WIN32)
    // On Windows std::filesystem::rename FAILS if the destination already exists
    // (POSIX rename replaces atomically; Win32 MoveFile does not). This silently
    // broke every checkpoint/conversation re-save. Use an atomic replace.
    if (MoveFileExW(tmp.wstring().c_str(), file.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return;
    }
#endif

    // Fallback: remove the destination, then rename (loses atomicity but works).
    std::error_code ec2;
    fs::remove(file, ec2);
    fs::rename(tmp, file, ec2);
    if (ec2) {
        fs::remove(tmp, ec2);
        throw std::runtime_error("rename failed: " + pathutil::toUtf8(tmp)
                                 + " -> " + pathutil::toUtf8(file) + ": " + ec.message());
    }
}

ConversationStore::ConversationStore() = default;

fs::path ConversationStore::rootDir() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.autocoder";
    }
    const QByteArray baseUtf8 = base.toUtf8();
    fs::path p = pathutil::fromUtf8(std::string(baseUtf8.constData(), baseUtf8.size())) / "projects";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::string ConversationStore::projectKey(const fs::path& projectRoot) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(projectRoot, ec);
    QByteArray bytes = QByteArray::fromStdString(pathutil::toUtf8(canonical.empty() ? projectRoot : canonical));
    QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
    return std::string(hash.left(32).constData(), 32);
}

fs::path ConversationStore::projectDir(const fs::path& projectRoot) const {
    const std::string key = projectKey(projectRoot);
    const fs::path root = rootDir();
    fs::path p = root / key;
    std::error_code ec;

    // Migration: earlier builds keyed this directory by the first 16 hex chars of
    // the SHA-1 hash; it is now 32 chars. Because the new key is a prefix-superset
    // of the old one, a legacy directory can be moved into place unambiguously so
    // existing conversations and checkpoints remain visible after the upgrade.
    if (key.size() > 16) {
        const fs::path legacy = root / key.substr(0, 16);
        if (legacy != p && fs::is_directory(legacy, ec)) {
            const bool newMissing = !fs::exists(p, ec);
            const bool newEmpty = !newMissing && fs::is_empty(p, ec) && !ec;
            if (newMissing || newEmpty) {
                if (newEmpty) fs::remove(p, ec);
                std::error_code renameEc;
                fs::rename(legacy, p, renameEc);
                if (renameEc) {
                    // Could not migrate (e.g. cross-device); fall back to the
                    // legacy directory so the user's history stays accessible.
                    fs::create_directories(legacy, ec);
                    return legacy;
                }
            }
        }
    }

    fs::create_directories(p, ec);
    return p;
}

std::string ConversationStore::newId() {
    QString u = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return u.toStdString();
}

std::string ConversationStore::nowIso() {
    return QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODate).toStdString();
}

std::string ConversationStore::deriveTitle(const json& messages) {
    bool skippedCompactSummary = false;
    for (const auto& m : messages) {
        if (m.value("role", "") == "user" && m.contains("content") && m["content"].is_string()) {
            std::string c = m["content"].get<std::string>();
            if (isCompactSummaryContent(c)) {
                skippedCompactSummary = true;
                continue;
            }
            return formatTitle(std::move(c));
        }
    }
    if (skippedCompactSummary) return "Compacted conversation";
    return "(empty)";
}

std::vector<ConversationStore::Entry>
ConversationStore::list(const fs::path& projectRoot) const {
    diagnostics::TraceTimer timer("ConversationStore::list");
    std::vector<Entry> out;
    fs::path dir = projectDir(projectRoot);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        try {
            std::ifstream in(entry.path(), std::ios::binary);
            json j; in >> j;
            Entry e;
            e.id           = j.value("id", pathutil::toUtf8(entry.path().stem()));
            e.title        = j.value("title", "");
            e.updatedAtIso = j.value("updated_at", "");
            if (e.title.empty() || isCompactSummaryContent(e.title)) {
                e.title = deriveTitle(j.value("messages", json::array()));
            }
            out.push_back(std::move(e));
        } catch (...) { /* skip malformed file */ }
    }
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.updatedAtIso > b.updatedAtIso; });
    return out;
}

Conversation ConversationStore::load(const fs::path& projectRoot,
                                     const std::string& id) const {
    fs::path file = projectDir(projectRoot) / (id + ".json");
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("could not open conversation: " + pathutil::toUtf8(file));
    json j; in >> j;
    return Conversation::fromJson(j);
}

bool ConversationStore::exists(const fs::path& projectRoot,
                               const std::string& id) const {
    std::error_code ec;
    const fs::path file = projectDir(projectRoot) / (id + ".json");
    return fs::is_regular_file(file, ec);
}

bool ConversationStore::remove(const fs::path& projectRoot,
                               const std::string& id) const {
    fs::path file = projectDir(projectRoot) / (id + ".json");
    std::error_code ec;
    return fs::remove(file, ec);
}

fs::path ConversationStore::save(const fs::path& projectRoot,
                                 const std::string& id,
                                 const Conversation& conv) const {
    fs::path file = projectDir(projectRoot) / (id + ".json");

    json j = conv.toJson();
    j["id"]         = id;
    j["updated_at"] = nowIso();
    if (!j.contains("created_at") || j["created_at"].is_null()) {
        // Preserve existing created_at if file already exists.
        std::ifstream existing(file);
        if (existing) {
            try {
                json prev; existing >> prev;
                j["created_at"] = prev.value("created_at", j["updated_at"]);
            } catch (...) {
                j["created_at"] = j["updated_at"];
            }
        } else {
            j["created_at"] = j["updated_at"];
        }
    }
    j["title"] = deriveTitle(j.value("messages", json::array()));

    atomicWriteJson(file, j);
    return file;
}

// ===== Checkpoints =====

fs::path ConversationStore::checkpointsDir(const fs::path& projectRoot,
                                           const std::string& convId) const {
    fs::path d = projectDir(projectRoot) / "checkpoints" / convId;
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

void ConversationStore::saveCheckpoint(const fs::path& projectRoot,
                                       const std::string& convId,
                                       const Checkpoint& cp) const {
    json j;
    j["turn_index"]     = cp.turnIndex;
    j["user_message"]   = cp.userMessage;
    j["messages"]       = cp.conversationMessages;

    json filesJson = json::array();
    for (const auto& [path, content] : cp.fileSnapshots) {
        filesJson.push_back(fileSnapshotToJson(path, content));
    }
    j["file_snapshots"] = filesJson;

    fs::path file = checkpointsDir(projectRoot, convId)
                  / (std::to_string(cp.turnIndex) + ".json");
    atomicWriteJson(file, j);
}


ConversationStore::Checkpoint
ConversationStore::loadCheckpoint(const fs::path& projectRoot,
                                  const std::string& convId,
                                  int turnIndex) const {
    fs::path file = checkpointsDir(projectRoot, convId)
                  / (std::to_string(turnIndex) + ".json");
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("could not open checkpoint: " + pathutil::toUtf8(file));
    json j; in >> j;

    Checkpoint cp;
    cp.turnIndex           = j.value("turn_index", turnIndex);
    cp.userMessage         = j.value("user_message", "");
    cp.conversationMessages = j.value("messages", json::array());

    for (const auto& fj : j.value("file_snapshots", json::array())) {
        cp.fileSnapshots.emplace_back(
            fj.value("path", ""),
            fileSnapshotContentFromJson(fj));
    }
    return cp;
}

std::vector<int>
ConversationStore::listCheckpoints(const fs::path& projectRoot,
                                   const std::string& convId) const {
    std::vector<int> turns;
    fs::path dir = checkpointsDir(projectRoot, convId);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return turns;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        // filename is "<turnIndex>.json"
        std::string stem = pathutil::toUtf8(entry.path().stem());
        try {
            int idx = std::stoi(stem);
            turns.push_back(idx);
        } catch (...) { /* skip non-numeric filenames */ }
    }
    std::sort(turns.begin(), turns.end());
    return turns;
}

void ConversationStore::removeCheckpoints(const fs::path& projectRoot,
                                          const std::string& convId) const {
    fs::path dir = checkpointsDir(projectRoot, convId);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

bool ConversationStore::removeCheckpoint(const fs::path& projectRoot,
                                          const std::string& convId,
                                          int turnIndex) const {
    fs::path file = checkpointsDir(projectRoot, convId)
                  / (std::to_string(turnIndex) + ".json");
    std::error_code ec;
    return fs::remove(file, ec);
}

void ConversationStore::updateCheckpointSnapshots(
        const fs::path& projectRoot,
        const std::string& convId,
        int turnIndex,
        const std::vector<std::pair<std::string, std::string>>& snapshots) const {
    fs::path file = checkpointsDir(projectRoot, convId)
                  / (std::to_string(turnIndex) + ".json");
    json j;
    {
        // Close the read handle BEFORE rewriting: on Windows an open handle
        // blocks the atomic replace in atomicWriteJson ("access is denied").
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        in >> j;
    }

    json filesJson = json::array();
    for (const auto& [path, content] : snapshots) {
        filesJson.push_back(fileSnapshotToJson(path, content));
    }
    j["file_snapshots"] = std::move(filesJson);

    atomicWriteJson(file, j);
}
