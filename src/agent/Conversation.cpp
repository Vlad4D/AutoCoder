#include "Conversation.h"

#include "tools/PathUtil.h"

#include <QByteArray>
#include <QString>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string toValidUtf8(std::string text) {
    const QByteArray bytes(text.data(), static_cast<qsizetype>(text.size()));
    const QString decoded = QString::fromUtf8(bytes.constData(), bytes.size());
    const QByteArray utf8 = decoded.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

}  // namespace

Conversation::Conversation(fs::path projectRoot, std::string model)
    : projectRoot_(std::move(projectRoot)), model_(std::move(model)) {}

void Conversation::setSystemPrompt(std::string text) {
    invalidateCache();
    json msg = { {"role", "system"}, {"content", std::move(text)} };
    if (!messages_.empty() && messages_[0].value("role", "") == "system") {
        messages_[0] = std::move(msg);
    } else {
        messages_.insert(messages_.begin(), std::move(msg));
    }
}

void Conversation::addUser(std::string text) {
    invalidateCache();
    messages_.push_back({ {"role", "user"}, {"content", std::move(text)} });
}

void Conversation::addAssistant(json messageObject) {
    invalidateCache();
    // Accept whatever the API returned. Force role=assistant for safety.
    messageObject["role"] = "assistant";
    if (!messageObject.contains("content")) messageObject["content"] = nullptr;
    messages_.push_back(std::move(messageObject));
}

void Conversation::addToolResult(std::string toolCallId, std::string content, bool isError) {
    invalidateCache();
    json msg = {
        {"role", "tool"},
        {"tool_call_id", std::move(toolCallId)},
        {"content", toValidUtf8(std::move(content))}
    };
    if (isError) msg["is_error"] = true;
    messages_.push_back(std::move(msg));
}

void Conversation::setApproval(const std::string& toolCallId, const std::string& command,
                               const std::string& reason, const std::string& explanation,
                               const std::string& decision) {
    if (toolCallId.empty()) return;
    approvals_[toolCallId] = {
        {"command", toValidUtf8(command)},
        {"reason", toValidUtf8(reason)},
        {"explanation", toValidUtf8(explanation)},
        {"decision", decision}
    };
}

size_t Conversation::trimToBudget(size_t maxBytes, int minMessages) {
    if (messages_.empty() || static_cast<int>(messages_.size()) < minMessages)
        return 0;

    const size_t origBytes = approxBytes();
    if (origBytes <= maxBytes)
        return 0;

    // Strategy: identify whole "exchanges" (user + assistant + any tool results)
    // that we can remove, starting from the oldest after the system prompt.
    // We build the exchange list against the *original* messages_ indices,
    // then remove each exchange in order, adjusting for the cumulative shift
    // caused by prior removals.

    // Step 1: find exchange boundaries. An exchange starts at a user message
    // and spans every assistant/tool message until the next user. Tool-using
    // turns often have assistant(tool_calls), tool results, then final
    // assistant text; all of that must stay together.
    struct Exchange { size_t start; size_t count; };
    std::vector<Exchange> exchanges;

    size_t i = 1; // skip system prompt at index 0
    while (i < messages_.size()) {
        std::string role = messages_[i].value("role", "");
        if (role == "user") {
            size_t start = i;
            ++i;
            while (i < messages_.size() && messages_[i].value("role", "") != "user")
                ++i;
            exchanges.push_back({start, i - start});
        } else {
            // Orphan message (assistant without user, or stray tool result).
            // Remove it individually.
            exchanges.push_back({i, 1});
            ++i;
        }
    }

    // Step 2: remove oldest exchanges until under budget or minMessages is hit.
    // We track a running offset that accounts for removals so the exchange
    // indices (which were derived from the original messages_) stay correct.
    auto msgs = messages_; // work on a copy for safety
    size_t offset = 0;
    for (const auto& ex : exchanges) {
        // Only remove this exchange if doing so would still leave at least
        // minMessages messages (to preserve recent context).
        if (static_cast<int>(msgs.size()) - static_cast<int>(ex.count) < minMessages)
            break;

        // Adjust start by how many items we've already removed.
        size_t adjustedStart = ex.start - offset;

        // Erase in reverse order so indices stay valid.
        for (size_t j = adjustedStart + ex.count; j > adjustedStart; --j) {
            msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(j - 1));
        }
        offset += ex.count;

        // Recalculate after removal -- our byte estimate changed.
        size_t curBytes = msgs.dump().size();
        if (curBytes <= maxBytes)
            break;
    }

    // Check if we made progress.
    size_t newBytes = msgs.dump().size();
    if (newBytes >= origBytes)
        return 0;

    messages_ = std::move(msgs);
    invalidateCache();
    return origBytes - newBytes;
}

int Conversation::approxTokens() const {
    return static_cast<int>(approxBytes() / 4);
}

size_t Conversation::approxBytes() const {
    if (cachedBytes_ == 0) {
        cachedBytes_ = messages_.dump().size();
    }
    return cachedBytes_;
}

json Conversation::toJson() const {
    return {
        {"project_root", pathutil::toUtf8(projectRoot_)},
        {"model", model_},
        {"messages", messages_},
        {"approvals", approvals_}
    };
}

Conversation Conversation::fromJson(const json& j) {
    Conversation c;
    c.projectRoot_ = pathutil::fromUtf8(j.value("project_root", ""));
    c.model_       = j.value("model", "deepseek-chat");
    c.messages_    = j.value("messages", json::array());
    c.approvals_   = j.value("approvals", json::object());
    return c;
}
