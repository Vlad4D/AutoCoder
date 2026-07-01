#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

// Wraps an OpenAI-style messages[] array with convenience builders.
// Messages are stored as nlohmann::json directly so the wire format and the
// in-memory representation are the same.
class Conversation {
public:
    Conversation() = default;
    Conversation(std::filesystem::path projectRoot, std::string model);

    // ----- Mutation -----
    void setSystemPrompt(std::string text);   // Inserts/replaces the role:system message at index 0.
    void addUser(std::string text);
    void addAssistant(nlohmann::json messageObject);   // Whole assistant msg incl. tool_calls (if any).
    void addToolResult(std::string toolCallId, std::string content, bool isError = false);

    // Record that a bash command went through the user-approval gate. Stored
    // OUTSIDE messages_ (so it is never sent to the LLM) but persisted with the
    // conversation, so the approval widget can be re-rendered after a reload.
    // decision is "allow_once", "always", or "deny".
    void setApproval(const std::string& toolCallId, const std::string& command,
                     const std::string& reason, const std::string& explanation,
                     const std::string& decision);

    // ----- Read -----
    const nlohmann::json& messages() const { return messages_; }
    const nlohmann::json& approvals() const { return approvals_; }
    const std::filesystem::path& projectRoot() const { return projectRoot_; }
    const std::string& model() const { return model_; }

    void setModel(std::string m) { model_ = std::move(m); }
    void setProjectRoot(std::filesystem::path p) { projectRoot_ = std::move(p); }

    // ----- Token budget management -----
    // Trim the oldest whole exchange(s) from the conversation so that the
    // serialized size stays within maxBytes, while keeping at least minMessages
    // messages (including the system prompt). Returns the number of bytes freed.
    // Exchanges are removed from the *middle* -- the system prompt (index 0) and
    // the most recent messages are preserved. An "exchange" is a user message,
    // its paired assistant message (which may include tool_calls), and any
    // subsequent tool-result messages that follow it.
    size_t trimToBudget(size_t maxBytes, int minMessages = 3);

    // Approximate token count (bytes / 4) for the messages array alone.
    int approxTokens() const;
    size_t approxBytes() const;

    // ----- Persistence -----
    nlohmann::json toJson() const;
    static Conversation fromJson(const nlohmann::json& j);

private:
    nlohmann::json messages_ = nlohmann::json::array();
    // toolCallId -> { command, reason, decision }. Persisted, never sent to LLM.
    nlohmann::json approvals_ = nlohmann::json::object();
    std::filesystem::path projectRoot_;
    std::string model_ = "deepseek-chat";

    // Cached serialized size for approxBytes(). Invalidated on mutation.
    mutable size_t cachedBytes_ = 0;
    void invalidateCache() { cachedBytes_ = 0; }
};
