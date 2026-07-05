#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/Conversation.h"

class QString;

// On-disk JSON store for conversations, partitioned per project root.
// Layout (resolved via QStandardPaths::AppDataLocation):
//   <AppDataLocation>/projects/<projectKey>/<id>.json
// On Windows with organization/app both set to "AutoCoder", Qt normally
// resolves this as:
//   %APPDATA%/AutoCoder/AutoCoder/projects/<projectKey>/<id>.json
//
// One file per conversation. The filename's `id` is also stored inside.
//
// Checkpoints are stored alongside each conversation in a subdirectory:
//   <AppDataLocation>/projects/<projectKey>/checkpoints/<convId>/
//       <turnIndex>.json
//
// Each checkpoint snapshot saves the conversation state and the file diffs
// that the LLM produced during that turn, so the user can revert.
class ConversationStore {
public:
    struct Entry {
        std::string id;
        std::string title;          // first non-compact user message, truncated
        std::string updatedAtIso;   // for sorting display
    };

    ConversationStore();

    // ----- Conversation CRUD -----

    // List conversations for a project, newest first.
    std::vector<Entry> list(const std::filesystem::path& projectRoot) const;

    // Read a conversation by id; throws std::runtime_error on failure.
    Conversation load(const std::filesystem::path& projectRoot,
                      const std::string& id) const;

    // True when the conversation JSON exists as a regular file.
    bool exists(const std::filesystem::path& projectRoot,
                const std::string& id) const;

    // Persist (overwriting). Returns the file path written.
    std::filesystem::path save(const std::filesystem::path& projectRoot,
                               const std::string& id,
                               const Conversation& conv) const;

    // Delete a conversation file. Returns true if a file was removed.
    bool remove(const std::filesystem::path& projectRoot,
                const std::string& id) const;

    // Build a fresh UUID for a new conversation.
    static std::string newId();

    // Where conversations for `projectRoot` live (creates the dir if missing).
    std::filesystem::path projectDir(const std::filesystem::path& projectRoot) const;

    // ----- Checkpoints -----

    // A checkpoint records the conversation state before a user message, paired
    // with snapshots of every file that the LLM wrote or edited during that turn.
    // The file snapshots are "old content" -- what the file looked like before
    // the turn's modifications.
    struct Checkpoint {
        int turnIndex;                         // 0-based turn number
        std::string userMessage;               // the user's prompt for this turn
        nlohmann::json conversationMessages;   // snapshot of conv_.messages() before the turn
        // Map of path -> original file content (before this turn's modifications).
        std::vector<std::pair<std::string, std::string>> fileSnapshots;
    };

    // Save a checkpoint for conversation `convId`.  Creates the
    // checkpoints/<convId>/ directory if needed.
    void saveCheckpoint(const std::filesystem::path& projectRoot,
                        const std::string& convId,
                        const Checkpoint& cp) const;

    // Load a checkpoint for conversation `convId` at a given turn index.
    // Throws std::runtime_error if not found.
    Checkpoint loadCheckpoint(const std::filesystem::path& projectRoot,
                              const std::string& convId,
                              int turnIndex) const;

    // List all checkpoint turn indices for a conversation (sorted ascending).
    std::vector<int> listCheckpoints(const std::filesystem::path& projectRoot,
                                     const std::string& convId) const;

    // Delete all checkpoints for a conversation.
    void removeCheckpoints(const std::filesystem::path& projectRoot,
                           const std::string& convId) const;

    // Update file snapshots on an already-saved checkpoint (appends/overwrites
    // snapshots without touching the other fields).
    void updateCheckpointSnapshots(const std::filesystem::path& projectRoot,
                                   const std::string& convId,
                                   int turnIndex,
                                   const std::vector<std::pair<std::string, std::string>>& snapshots) const;

    // Delete a single checkpoint file.  Returns true if a file was removed.
    bool removeCheckpoint(const std::filesystem::path& projectRoot,
                          const std::string& convId,
                          int turnIndex) const;

    // Where checkpoints for `convId` live (creates the dir if missing).
    std::filesystem::path checkpointsDir(const std::filesystem::path& projectRoot,
                                         const std::string& convId) const;

private:
    std::filesystem::path rootDir() const;     // .../AutoCoder/projects
    static std::string projectKey(const std::filesystem::path& projectRoot);
    static std::string nowIso();
    static std::string deriveTitle(const nlohmann::json& messages);
};
