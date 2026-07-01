#include "AgentRunner.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <QByteArray>
#include <QTimer>

#include "agent/SystemPrompt.h"
#include "diagnostics/TraceTimer.h"
#include "llm/LlmClient.h"
#include "tools/AskUserTool.h"
#include "tools/PathUtil.h"
#include "tools/shell/CommandPolicy.h"
#include "tools/shell/InternalShell.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr size_t kReplayToolResultPreviewBytes = 96 * 1024;
constexpr size_t kGuidanceSnapshotMaxBytes = 64 * 1024;
constexpr size_t kDefaultReplayMessagesForUi = 40;
constexpr size_t kReplayMessagesIncrement = 240;
constexpr int kReplayProgressStep = 10;
constexpr int kMaxStreamRetries = 3;

// True if a stream error looks like a transient connection/network/server hiccup
// that is worth retrying automatically (rather than a request the API rejected).
bool isRetryableStreamError(const QString& detail) {
    const QString d = detail.toLower();
    static const char* kTransient[] = {
        "connection closed", "remote host closed", "closed the connection",
        "connection reset", "connection refused", "connection aborted",
        "timed out", "timeout", "network", "temporarily", "temporary failure",
        "ssl handshake", "broken pipe", "502", "503", "504", "429",
        "overloaded", "try again",
    };
    for (const char* t : kTransient) {
        if (d.contains(QLatin1String(t))) return true;
    }
    return false;
}

std::string replayToolResultPreview(const std::string& result) {
    if (result.size() <= kReplayToolResultPreviewBytes) return result;

    std::string preview = result.substr(0, kReplayToolResultPreviewBytes);
    preview += "\n[... ";
    preview += std::to_string(result.size() - kReplayToolResultPreviewBytes);
    preview += " bytes hidden from replay preview]";
    return preview;
}

size_t replayStartIndex(const json& messages, size_t visibleLimit) {
    if (!messages.is_array() || messages.size() <= visibleLimit) return 0;
    return messages.size() - visibleLimit;
}

bool isCompactSummaryMessage(const json& m) {
    if (m.value("role", "") != "user") return false;
    if (!m.contains("content") || !m["content"].is_string()) return false;
    static const std::string kCompactPrefix = "[Previous conversation compacted]";
    return m["content"].get_ref<const std::string&>().starts_with(kCompactPrefix);
}

json currentSystemMessage(const fs::path& projectRoot) {
    return {
        {"role", "system"},
        {"content", SystemPrompt::build(projectRoot)}
    };
}

std::optional<std::string> buildGuidanceSnapshot(const fs::path& projectRoot) {
    auto guidanceFile = SystemPrompt::findProjectGuidanceFile(projectRoot);
    if (!guidanceFile) return std::nullopt;

    std::ifstream in(*guidanceFile, std::ios::binary);
    if (!in) return std::nullopt;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string content = buffer.str();

    std::string note;
    if (content.size() > kGuidanceSnapshotMaxBytes) {
        note = "\n\n[Guidance snapshot truncated: ";
        note += std::to_string(content.size() - kGuidanceSnapshotMaxBytes);
        note += " bytes omitted.]";
        content.resize(kGuidanceSnapshotMaxBytes);
    }

    std::string snapshot = "Project guidance snapshot from ";
    snapshot += pathutil::toUtf8(guidanceFile->filename());
    snapshot += ":\n\n";
    snapshot += content;
    snapshot += note;
    return snapshot;
}

json checkpointMessagesBeforeTurn(const ConversationStore::Checkpoint& cp) {
    json messages = cp.conversationMessages;
    if (!messages.is_array() || messages.empty()) return messages;

    // Older checkpoints were written after appending the user message, even
    // though revert expects the snapshot from before that message. Normalize
    // those legacy snapshots by dropping the just-added user message.
    const json& last = messages.back();
    if (last.value("role", "") == "user"
        && last.value("content", "") == cp.userMessage) {
        messages.erase(messages.end() - 1);
    }
    return messages;
}

}  // namespace

AgentRunner::AgentRunner(QObject* parent) : QObject(parent) {
    tools_  = ToolRegistry::makeDefault();
    client_ = new LlmClient(this);   // child; moves with us when moveToThread'd
    connect(client_, &LlmClient::tokenDelta,     this, &AgentRunner::onTokenDelta);
    connect(client_, &LlmClient::streamFinished, this, &AgentRunner::onStreamFinished);
    connect(client_, &LlmClient::errorOccurred,  this, &AgentRunner::onStreamError);
    connect(client_, &LlmClient::tokenUsage,     this, &AgentRunner::tokenUsageStats);
}

AgentRunner::~AgentRunner() = default;

void AgentRunner::setApiKey(QString key) {
    apiKey_ = std::move(key);
    if (client_) client_->setApiKey(apiKey_);
}

void AgentRunner::setProvider(QString provider) {
    provider_ = std::move(provider);
    if (client_) client_->setProvider(provider_);
}

void AgentRunner::setModel(QString model) {
    model_ = std::move(model);
    if (client_) client_->setModel(model_);
    conv_.setModel(model_.toStdString());
}

void AgentRunner::setBaseUrl(QString url) {
    if (client_) client_->setBaseUrl(std::move(url));
}

void AgentRunner::setMaxIterations(int n) {
    if (n > 0) maxIter_ = n;
}

void AgentRunner::setDefaultBashTimeoutMs(int ms) {
    ctx_.defaultBashTimeoutMs = ms > 0 ? ms : 0;
}

void AgentRunner::setMaxContextTokens(int tokens) {
    if (tokens > 0) maxContextTokens_ = tokens;
}

void AgentRunner::setCheckCommand(QString cmd) {
    checkCommand_ = cmd.trimmed();
}

void AgentRunner::setReasoningGuidance(bool enabled) {
    reasoningGuidanceEnabled_ = enabled;
    systemPromptNeedsRefresh_ = true;
}

void AgentRunner::setSendTokens(int tokens) {
    if (tokens > 0) {
        sendTokens_ = tokens;
        if (client_) client_->setMaxTokens(tokens);
    }
}

void AgentRunner::deleteConversation(QString id) {
    bool wasCurrent = (currentConvId_ == id.toStdString());
    store_.removeCheckpoints(projectRoot_, id.toStdString());
    store_.remove(projectRoot_, id.toStdString());
    if (wasCurrent) {
        newConversation();
    }
    emitConversationsList();
}

void AgentRunner::deleteConversations(QStringList ids) {
    bool deletedCurrent = false;
    for (const QString& id : ids) {
        const std::string idStd = id.toStdString();
        if (currentConvId_ == idStd) deletedCurrent = true;
        store_.removeCheckpoints(projectRoot_, idStd);
        store_.remove(projectRoot_, idStd);
    }
    if (deletedCurrent) {
        newConversation();
    }
    emitConversationsList();
}

void AgentRunner::setProject(QString rootPath) {
    const QByteArray rootUtf8 = rootPath.toUtf8();
    projectRoot_ = pathutil::fromUtf8(std::string(rootUtf8.constData(), rootUtf8.size()));
    newConversation();
    emitConversationsList();
}

void AgentRunner::newConversation() {
    diagnostics::TraceTimer timer("AgentRunner::newConversation");
    // Abort any in-flight LLM stream first, so its callbacks can't write into
    // the fresh conversation. abort() is a no-op when idle; with a live stream
    // it routes synchronously through onStreamError -> fail(), finalizing the
    // old run (running_, snapshots, open text bubble) before the reset below.
    if (client_) client_->abort();

    cancelled_.store(false);
    running_.store(false);
    awaitingInput_.store(false);
    inAssistantTextRun_ = false;
    readSet_.clear();
    currentConvId_ = ConversationStore::newId();
    visibleReplayMessages_ = kDefaultReplayMessagesForUi;
    turnIndex_ = 0;

    conv_ = Conversation(projectRoot_, model_.toStdString());

    ctx_ = {};
    ctx_.projectRoot = projectRoot_;
    ctx_.readSet     = &readSet_;
    ctx_.cancelled   = &cancelled_;
    ctx_.fileBeforeSnapshots = &fileBeforeSnapshots_;
    ctx_.pendingUserQuestion = &pendingUserQuestion_;
    ctx_.pendingUserOptions  = &pendingUserOptions_;
    pendingToolCallId_.clear();
    pendingUserOptions_.clear();
    pendingIsCommandApproval_ = false;
    pendingToolCalls_ = json::array();
    pendingResumeIndex_ = 0;

    // System prompt (including the repomap) is set lazily in sendToLlm(),
    // so creating a new conversation is instant. It is *not* needed until
    // we actually send a message to the model.
    systemPromptNeedsRefresh_ = true;

    emit conversationCleared();
    emitContextStats();
    // Show the new conversation in the sidebar right away (it is not on disk
    // yet; emitConversationsList() prepends the in-memory entry).
    emitConversationsList();
}

void AgentRunner::loadConversation(QString id) {
    // Same as newConversation(): end any in-flight stream before the reset.
    if (client_) client_->abort();

    const std::string convId = id.toStdString();
    if (!store_.exists(projectRoot_, convId)) {
        emit errorOccurred(QStringLiteral("Saved conversation no longer exists: %1").arg(id));
        newConversation();
        return;
    }

    try {
        diagnostics::TraceTimer timer("AgentRunner::loadConversation");
        Conversation loaded = store_.load(projectRoot_, convId);
        cancelled_.store(false);
        running_.store(false);
        awaitingInput_.store(false);
        inAssistantTextRun_ = false;
        readSet_.clear();
        currentConvId_ = convId;
        visibleReplayMessages_ = kDefaultReplayMessagesForUi;
        conv_ = std::move(loaded);
        if (conv_.model().empty()) conv_.setModel(model_.toStdString());
        // The loaded conversation already has a system prompt saved with it.
        // We keep it; the repomap is regenerated lazily in ensureSystemPrompt()
        // if/when we send to the LLM.
        systemPromptNeedsRefresh_ = false;

        // Repair any orphaned assistant tool_calls (missing tool results)
        // that may have been saved due to historical bugs.
        repairOrphanedToolCalls();

        ctx_ = {};
        ctx_.projectRoot = projectRoot_;
        ctx_.readSet     = &readSet_;
        ctx_.cancelled   = &cancelled_;
        ctx_.fileBeforeSnapshots = &fileBeforeSnapshots_;
        ctx_.pendingUserQuestion = &pendingUserQuestion_;
        ctx_.pendingUserOptions  = &pendingUserOptions_;
        pendingToolCallId_.clear();
        pendingUserOptions_.clear();
        pendingIsCommandApproval_ = false;
        pendingToolCalls_ = json::array();
        pendingResumeIndex_ = 0;

        // Determine the current turn index from checkpoint count.
        auto turns = store_.listCheckpoints(projectRoot_, currentConvId_);
        turnIndex_ = turns.empty() ? 0 : (*std::max_element(turns.begin(), turns.end()) + 1);

        replayLoadedConversation(turns, false);
        emitContextStats();
        emitConversationsList();
    } catch (const std::exception& e) {
        emit errorOccurred(QString::fromStdString(std::string("load failed: ") + e.what()));
    }
}

void AgentRunner::loadEarlierConversationHistory() {
    if (isBusy()) {
        emit errorOccurred(QStringLiteral("cannot load earlier history while agent is busy"));
        return;
    }
    if (currentConvId_.empty() || conv_.messages().empty()) return;

    const size_t totalMessages = conv_.messages().size();
    if (visibleReplayMessages_ >= totalMessages) return;

    visibleReplayMessages_ = std::min(totalMessages,
                                      visibleReplayMessages_ + kReplayMessagesIncrement);
    auto turns = store_.listCheckpoints(projectRoot_, currentConvId_);
    replayLoadedConversation(turns, false);
}

void AgentRunner::replayLoadedConversation(const std::vector<int>& turns,
                                           bool keepBusyAfterReplay) {
    diagnostics::TraceTimer timer("replayLoadedConversation");
    const auto& messages = conv_.messages();
    const size_t firstReplayIndex = replayStartIndex(messages, visibleReplayMessages_);
    const int hiddenReplayMessages = static_cast<int>(firstReplayIndex);
    const int totalReplayMessages = static_cast<int>(
        messages.size() - firstReplayIndex + (hiddenReplayMessages > 0 ? 1 : 0));

    emit conversationReplayStarted(totalReplayMessages, keepBusyAfterReplay);
    emit conversationCleared();

    int replayDone = 0;
    if (hiddenReplayMessages > 0) {
        emit conversationReplayHistoryHidden(hiddenReplayMessages);
        emit conversationReplayProgress(++replayDone, totalReplayMessages);
    }

    // Index tool results by id so we can pair them with assistant tool_calls.
    // Store both content and the error flag.
    struct ToolResultInfo {
        const std::string* content = nullptr;
        bool isError = false;
    };
    std::map<std::string, ToolResultInfo> toolResults;
    for (const auto& m : messages) {
        if (m.value("role", "") != "tool") continue;
        if (m.contains("content") && m["content"].is_string()) {
            ToolResultInfo info;
            info.content = &m["content"].get_ref<const std::string&>();
            info.isError = m.value("is_error", false);
            toolResults[m.value("tool_call_id", "")] = info;
        }
    }

    const auto userTurns = buildReplayUserTurnMap(turns, currentConvId_);
    for (size_t msgIndex = 0; msgIndex < messages.size(); ++msgIndex) {
        const auto& m = messages[msgIndex];
        if (msgIndex < firstReplayIndex) continue;

        auto markProgress = [&]() {
            ++replayDone;
            if (replayDone == totalReplayMessages
                || replayDone % kReplayProgressStep == 0) {
                emit conversationReplayProgress(replayDone, totalReplayMessages);
            }
        };

        std::string role = m.value("role", "");
        if (role == "system" || role == "tool") {
            markProgress();
            continue;
        }

        if (role == "user") {
            std::string content = m.value("content", "");
            static const std::string kCompactPrefix = "[Previous conversation compacted]";
            if (content.starts_with(kCompactPrefix)) {
                std::string summaryBody = content.substr(kCompactPrefix.size());
                size_t skip = summaryBody.find_first_not_of("\n\r ");
                if (skip != std::string::npos)
                    summaryBody = summaryBody.substr(skip);
                emit compactSummaryAppended(QString::fromStdString(summaryBody), 0);
                markProgress();
                continue;
            }

            const auto mappedTurn = userTurns.find(msgIndex);
            const int turnForMessage = mappedTurn == userTurns.end() ? -1 : mappedTurn->second;
            bool canRevert = turnForMessage >= 0
                && std::find(turns.begin(), turns.end(), turnForMessage) != turns.end();
            emit userMessageAdded(QString::fromStdString(content),
                                  canRevert,
                                  turnForMessage);
        } else if (role == "assistant") {
            if (m.contains("content") && m["content"].is_string()) {
                std::string c = m["content"].get<std::string>();
                if (!c.empty()) {
                    emit assistantTextDelta(QString::fromStdString(c));
                    emit assistantTextFinalized();
                }
            }
            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto& tc : m["tool_calls"]) {
                    std::string tcId = tc.value("id", "");
                    std::string name = tc.value("function", json::object()).value("name", "");
                    std::string args = tc.value("function", json::object()).value("arguments", "");
                    std::string result = "[no result recorded]";
                    bool isError = false;
                    auto found = toolResults.find(tcId);
                    if (found != toolResults.end()) {
                        result = replayToolResultPreview(*found->second.content);
                        isError = found->second.isError;
                    }
                    emit toolCallStarted(QString::fromStdString(name),
                                         QString::fromStdString(args));
                    emit toolCallFinished(QString::fromStdString(name),
                                          QString::fromStdString(result),
                                          !isError);

                    // Re-render the approval widget if this call was gated.
                    const auto& approvals = conv_.approvals();
                    if (approvals.contains(tcId)) {
                        const auto& a = approvals[tcId];
                        emit commandApprovalResolved(
                            QString::fromStdString(a.value("command", "")),
                            QString::fromStdString(a.value("reason", "")),
                            QString::fromStdString(a.value("explanation", "")),
                            QString::fromStdString(a.value("decision", "")));
                    }
                }
            }
        }

        markProgress();
    }

    emit conversationReplayFinished();
}

void AgentRunner::submitUserMessage(QString text) {
    if (running_.load()) {
        emit errorOccurred(QStringLiteral("agent is already running"));
        return;
    }
    if (apiKey_.isEmpty()) {
        emit errorOccurred(provider_ == QStringLiteral("claude")
                               ? QStringLiteral("Claude API key is not set")
                               : QStringLiteral("DeepSeek API key is not set"));
        return;
    }

    // If the user is sending a new message, clear any stale pending ask_user
    // or command-approval that was never answered.
    awaitingInput_.store(false);
    pendingIsCommandApproval_ = false;
    pendingToolCallId_.clear();
    pendingUserQuestion_.clear();
    pendingUserOptions_.clear();

    cancelled_.store(false);
    running_.store(true);
    iter_ = 0;
    streamRetries_ = 0;
    compactTriedThisSend_ = false;
    inAssistantTextRun_ = false;
    checkAttemptsThisTurn_ = 0;

    const std::string userText = text.toStdString();

    // Persist a real conversation as soon as a user turn starts. Fresh
    // conversations build the system prompt lazily, but without it a single
    // user message is skipped by saveCurrent() and an early cancel leaves only
    // an orphan checkpoint.
    ensureSystemPrompt();

    // Take a checkpoint before adding the user message so reverting to this
    // turn restores the conversation to the state before the message was sent.
    const int turnForMessage = takeCheckpoint(userText);

    conv_.addUser(userText);
    saveCurrent();

    emit userMessageAdded(text, turnForMessage >= 0, turnForMessage);
    emitContextStats();

    sendToLlm();
}

void AgentRunner::cancel() {
    diagnostics::TraceTimer timer("AgentRunner::cancel");
    cancelled_.store(true);
    if (client_) client_->abort();
    // Also clear any pending user-input wait so the app can close without
    // showing "agent is still busy" again. The user clearly intends to cancel.
    awaitingInput_.store(false);
    pendingIsCommandApproval_ = false;
    pendingToolCallId_.clear();
    pendingUserQuestion_.clear();
    pendingUserOptions_.clear();
}

void AgentRunner::provideUserInput(QString answer) {
    if (pendingToolCallId_.empty() || pendingIsCommandApproval_) {
        emit errorOccurred(QStringLiteral("no pending ask_user call"));
        return;
    }
    if (running_.load()) {
        emit errorOccurred(QStringLiteral("agent is already running"));
        return;
    }

    // Add the tool result with the user's answer.
    conv_.addToolResult(pendingToolCallId_, answer.toStdString());
    emit toolCallFinished(QStringLiteral("ask_user"), answer, true);

    // Clear the pending state.
    pendingToolCallId_.clear();
    pendingUserQuestion_.clear();
    pendingUserOptions_.clear();
    awaitingInput_.store(false);

    // Reset compaction guard so a needed second compaction isn't skipped.
    compactTriedThisSend_ = false;

    // Resume: run any batch calls that followed this ask_user, then send.
    running_.store(true);
    resumePendingToolCalls();
}

void AgentRunner::provideCommandDecision(QString toolCallId, QString decision, bool persist) {
    Q_UNUSED(toolCallId)
    if (pendingToolCallId_.empty() || !pendingIsCommandApproval_) {
        emit errorOccurred(QStringLiteral("no pending command approval"));
        return;
    }
    if (running_.load()) {
        emit errorOccurred(QStringLiteral("agent is already running"));
        return;
    }

    const std::string id          = pendingToolCallId_;
    const std::string command     = pendingCommand_;
    const std::string reason      = pendingCommandReason_;
    const std::string explanation = pendingCommandExplanation_;

    if (decision == QStringLiteral("allow_once")) {
        if (persist) autocoder::shell::addToUserAllowlist(command);

        // Execute the approved command here -- the loop paused before dispatch.
        ctx_.onOutput = [this](const std::string& chunk) {
            emit toolCallOutputDelta(QStringLiteral("bash"), QString::fromStdString(chunk));
        };
        ToolResult r = tools_.dispatch("bash", json{{"command", command}}, ctx_);
        ctx_.onOutput = nullptr;

        conv_.addToolResult(id, r.content);
        emit toolCallFinished(QStringLiteral("bash"), QString::fromStdString(r.content), r.ok);
    } else {  // deny
        const std::string denial =
            "The user denied permission to run this command:\n" + command
            + "\nReason it required approval: " + reason
            + "\nDo not retry it. Choose a different approach or ask the user what to do.";
        conv_.addToolResult(id, denial, true);
        emit toolCallFinished(QStringLiteral("bash"), QString::fromStdString(denial), false);
    }

    // Record the decision on the conversation so the approval widget can be
    // re-rendered after a reload (stored outside messages_, never sent to LLM).
    const std::string decisionLabel =
        (decision == QStringLiteral("deny")) ? "deny" : (persist ? "always" : "allow_once");
    conv_.setApproval(id, command, reason, explanation, decisionLabel);

    // Clear the pending state.
    pendingIsCommandApproval_ = false;
    pendingToolCallId_.clear();
    pendingCommand_.clear();
    pendingCommandReason_.clear();
    pendingCommandExplanation_.clear();
    awaitingInput_.store(false);
    compactTriedThisSend_ = false;

    emitContextStats();

    // Resume the cycle (set running_ only after the synchronous dispatch above).
    // Run any batch calls that followed this command, then send to the LLM.
    running_.store(true);
    resumePendingToolCalls();
}

void AgentRunner::compactConversation() {
    if (compactInProgress_.load()) {
        emit errorOccurred(QStringLiteral("Compaction is already in progress."));
        return;
    }
    if (running_.load()) {
        emit errorOccurred(QStringLiteral("Cannot compact while the agent is busy. Please wait or click Stop first."));
        return;
    }
    if (!ensureCompactionWorthwhile()) return;
    // Manual compaction: compact roughly the oldest half of the conversation
    // to free up space. Using half the current size as the budget ensures a
    // meaningful split point regardless of the token budget settings.
    doCompact(conv_.approxBytes() / 2, false);
}

bool AgentRunner::ensureCompactionWorthwhile() {
    const auto& msgs = conv_.messages();
    int exchangeCount = 0;
    for (const auto& m : msgs) {
        if (m.value("role", "") == "user") ++exchangeCount;
    }
    if (exchangeCount < 2) {
        emit errorOccurred(QStringLiteral(
            "Not enough conversation to compact (need at least 2 user messages)."));
        return false;
    }
    return true;
}


void AgentRunner::doCompact(size_t messagesBudget, bool forTrim) {
    compactInProgress_.store(true);
    compactForTrim_.store(forTrim);
    emit compactStarted();

    const auto& msgs = conv_.messages();

    // Walk exchanges from newest to oldest, accumulating bytes.
    // We reserve some headroom for the summary message overhead (~200 bytes).
    const size_t headroom = 200;
    const size_t budget = messagesBudget > headroom ? messagesBudget - headroom : 0;
    size_t splitIndex = 0; // index of the first message to keep (exclusive of old part)

    // Identify exchange boundaries (same logic as Conversation::trimToBudget).
    // A user turn includes every assistant/tool message until the next user.
    struct Exchange { size_t start; size_t count; };
    std::vector<Exchange> exchanges;

    size_t i = 1; // skip system prompt at index 0
    while (i < msgs.size()) {
        std::string role = msgs[i].value("role", "");
        if (role == "user") {
            size_t start = i;
            ++i;
            while (i < msgs.size() && msgs[i].value("role", "") != "user")
                ++i;
            exchanges.push_back({start, i - start});
        } else {
            exchanges.push_back({i, 1});
            ++i;
        }
    }

    // Calculate bytes for each exchange.
    struct ExchangeWithSize { size_t start; size_t count; size_t bytes; };
    std::vector<ExchangeWithSize> sized;
    sized.reserve(exchanges.size());
    for (const auto& ex : exchanges) {
        size_t bytes = 0;
        for (size_t j = ex.start; j < ex.start + ex.count; ++j) {
            bytes += msgs[j].dump().size();
        }
        sized.push_back({ex.start, ex.count, bytes});
    }

    // Walk from newest to oldest, accumulating kept bytes.
    size_t keptBytes = 0;
    size_t keptCount = 0;
    for (auto it = sized.rbegin(); it != sized.rend(); ++it) {
        size_t newTotal = keptBytes + it->bytes;
        if (newTotal > budget && keptCount > 0)
            break;
        keptBytes = newTotal;
        keptCount += it->count;
        splitIndex = it->start; // move split point backwards
    }

    // Build the "old part" to send to the LLM for summarization.
    // Include system prompt for context, plus all messages before splitIndex.
    // Strip tool_calls and tool results for the LLM prompt (too bulky).
    json compactMessages = json::array();

    // Include the current system prompt. This refreshes project guidance file
    // references instead of preserving a stale saved prompt.
    compactMessages.push_back(currentSystemMessage(projectRoot_));

    // Add user/assistant text content from the old part (indices 1..splitIndex-1).
    for (size_t idx = 1; idx < splitIndex && idx < msgs.size(); ++idx) {
        const auto& m = msgs[idx];
        std::string role = m.value("role", "");
        if (role == "system") continue;
        if (role == "tool") continue;
        if (role == "assistant") {
            if (m.contains("content") && m["content"].is_string()
                && !m["content"].get<std::string>().empty()) {
                json entry;
                entry["role"] = "assistant";
                entry["content"] = m["content"];
                compactMessages.push_back(std::move(entry));
            }
            continue;
        }
        if (role == "user") {
            compactMessages.push_back({
                {"role", "user"},
                {"content", m.value("content", "")}
            });
        }
    }

    // If there's nothing to compact, fall back to keeping only the last exchange.
    // This happens when the newest exchange alone exceeds the budget.
    if (compactMessages.size() <= 1) {
        // Find the last exchange boundary.
        size_t lastUserIdx = 0;
        for (size_t idx = 1; idx < msgs.size(); ++idx) {
            if (msgs[idx].value("role", "") == "user")
                lastUserIdx = idx;
        }
        if (lastUserIdx > 0) {
            splitIndex = lastUserIdx; // compact everything before the last user message
            // Rebuild compactMessages with just that old part.
            compactMessages.clear();
            compactMessages.push_back(currentSystemMessage(projectRoot_));
            for (size_t idx = 1; idx < splitIndex && idx < msgs.size(); ++idx) {
                const auto& m = msgs[idx];
                std::string role = m.value("role", "");
                if (role == "tool") continue;
                if (role == "assistant") {
                    if (m.contains("content") && m["content"].is_string()
                        && !m["content"].get<std::string>().empty()) {
                        json entry;
                        entry["role"] = "assistant";
                        entry["content"] = m["content"];
                        compactMessages.push_back(std::move(entry));
                    }
                    continue;
                }
                if (role == "user") {
                    compactMessages.push_back({
                        {"role", "user"},
                        {"content", m.value("content", "")}
                    });
                }
            }
            // If still nothing, bail.
            if (compactMessages.size() <= 1) {
                compactInProgress_.store(false);
                emit errorOccurred(QStringLiteral("Nothing to compact."));
                return;
            }
        } else {
            compactInProgress_ = false;
            emit errorOccurred(QStringLiteral("Nothing to compact."));
            return;
        }
    }

    if (auto guidanceSnapshot = buildGuidanceSnapshot(projectRoot_)) {
        compactMessages.push_back({
            {"role", "user"},
            {"content",
             "The following is the current project guidance file content. "
             "Carry forward its stable rules, architecture notes, build/test "
             "details, and workflow constraints into the compacted handoff when "
             "they are relevant.\n\n" + *guidanceSnapshot}
        });
    }

    // Add the compaction instruction as the final user message.
    compactMessages.push_back({
        {"role", "user"},
        {"content",
         "I need you to compress the conversation above into a detailed handoff "
         "summary for the next assistant turn. Preserve the technical continuity "
         "needed to keep working without the original messages. Include the user's "
         "goal, important constraints and preferences, files inspected or modified "
         "with exact paths, key implementation details, decisions made, commands or "
         "tests run and their outcomes, project guidance rules that matter for "
         "future work, known errors or risks, and the current "
         "state plus concrete next steps. Prefer structured Markdown with short "
         "sections and bullets. Do not include raw tool output unless a specific "
         "line, error, path, or value is needed later. Be compact, but do not be "
         "terse; include enough detail to reconstruct the work. Output ONLY the "
         "summary text, nothing else."}
    });

    // Connect the response handler temporarily.
    compactConnection_ = connect(client_, &LlmClient::responseReceived,
                                 this, [this, splitIndex](json msg) {
        QObject::disconnect(compactConnection_);
        onCompactResponse(std::move(msg), splitIndex);
    });

    client_->sendOnce(std::move(compactMessages));
}

void AgentRunner::onCompactResponse(json msg, size_t splitIndex) {
    if (!compactInProgress_) return;
    compactInProgress_ = false;

    std::string summary;
    if (msg.contains("content") && msg["content"].is_string()) {
        summary = msg["content"].get<std::string>();
    }

    if (summary.empty()) {
        // Don't leave the turn stuck busy.
        QObject::disconnect(compactConnection_);
        compactInProgress_ = false;
        if (compactForTrim_) {
            compactForTrim_ = false;
            if (running_.load()) {
                fail(QStringLiteral("Compaction failed: LLM returned empty summary."));
                return;
            }
        }
        emit errorOccurred(QStringLiteral("Compaction failed: LLM returned empty summary."));
        return;
    }

    // splitIndex is the index of the first message to keep.
    // Old part (before splitIndex) is replaced by the summary.
    // Safety clamp: at minimum keep the system prompt (index 0), at maximum
    // keep everything after the first exchange.
    const auto& msgs = conv_.messages();
    size_t keepStart = splitIndex;
    if (keepStart < 1) keepStart = 1;
    if (keepStart >= msgs.size()) keepStart = msgs.size() - 1;

    size_t bytesBefore = conv_.approxBytes();

    // Rebuild conversation: system prompt + compact summary as user message + kept part.
    json newMessages = json::array();

    newMessages.push_back(currentSystemMessage(projectRoot_));

    std::string compactContent = "[Previous conversation compacted]\n\n";
    if (auto guidanceSnapshot = buildGuidanceSnapshot(projectRoot_)) {
        compactContent += *guidanceSnapshot;
        compactContent += "\n\n";
    }
    compactContent += summary;

    // Add the compact summary as a user message.
    newMessages.push_back({
        {"role", "user"},
        {"content", std::move(compactContent)}
    });

    // Append the kept part (from keepStart onward), including tool results.
    for (size_t idx = keepStart; idx < msgs.size(); ++idx) {
        newMessages.push_back(msgs[idx]);
    }

    // Replace conversation.
    conv_ = Conversation::fromJson({
        {"project_root", pathutil::toUtf8(projectRoot_)},
        {"model", model_.toStdString()},
        {"messages", std::move(newMessages)}
    });

    size_t bytesAfter = conv_.approxBytes();
    int bytesSaved = static_cast<int>(bytesBefore > bytesAfter ? bytesBefore - bytesAfter : 0);

    // Rebuild the UI.
    const auto& replayMessages = conv_.messages();
    const size_t firstReplayIndex = replayStartIndex(replayMessages, visibleReplayMessages_);
    const int hiddenReplayMessages = static_cast<int>(firstReplayIndex);
    const int totalReplayMessages = static_cast<int>(
        replayMessages.size() - firstReplayIndex + (hiddenReplayMessages > 0 ? 1 : 0));
    emit conversationReplayStarted(totalReplayMessages, compactForTrim_);
    emit conversationCleared();
    int replayDone = 0;
    if (hiddenReplayMessages > 0) {
        emit conversationReplayHistoryHidden(hiddenReplayMessages);
        emit conversationReplayProgress(++replayDone, totalReplayMessages);
    }

    // Replay the compact view (same logic as loadConversation).
    // Index tool results by id so we can pair them with assistant tool_calls.
    struct ToolResultInfo {
        const std::string* content = nullptr;
        bool isError = false;
    };
    std::map<std::string, ToolResultInfo> toolResults;
    for (const auto& m : replayMessages) {
        if (m.value("role", "") != "tool") continue;
        if (m.contains("content") && m["content"].is_string()) {
            ToolResultInfo info;
            info.content = &m["content"].get_ref<const std::string&>();
            info.isError = m.value("is_error", false);
            toolResults[m.value("tool_call_id", "")] = info;
        }
    }
    for (size_t msgIndex = firstReplayIndex; msgIndex < replayMessages.size(); ++msgIndex) {
        const auto& m = replayMessages[msgIndex];
        auto markProgress = [&]() {
            ++replayDone;
            if (replayDone == totalReplayMessages
                || replayDone % kReplayProgressStep == 0) {
                emit conversationReplayProgress(replayDone, totalReplayMessages);
            }
        };

        std::string role = m.value("role", "");
        if (role == "system" || role == "tool") {
            markProgress();
            continue;
        }
        if (role == "user") {
            std::string content = m.value("content", "");
            // Detect the compact summary inserted by doCompact — render it as
            // a special collapsed widget instead of a plain user bubble.
            static const std::string kCompactPrefix = "[Previous conversation compacted]";
            if (content.starts_with(kCompactPrefix)) {
                std::string summaryBody = content.substr(kCompactPrefix.size());
                // Strip leading whitespace/newlines.
                size_t skip = summaryBody.find_first_not_of("\n\r ");
                if (skip != std::string::npos)
                    summaryBody = summaryBody.substr(skip);
                emit compactSummaryAppended(QString::fromStdString(summaryBody), bytesSaved);
                markProgress();
                continue;
            }
                emit userMessageAdded(QString::fromStdString(content), false, -1);
        } else if (role == "assistant") {
            if (m.contains("content") && m["content"].is_string()
                && !m["content"].get<std::string>().empty()) {
                emit assistantTextDelta(QString::fromStdString(m["content"]));
                emit assistantTextFinalized();
            }
            if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                for (const auto& tc : m["tool_calls"]) {
                    std::string tcId = tc.value("id", "");
                    std::string name = tc.value("function", json::object()).value("name", "");
                    std::string args = tc.value("function", json::object()).value("arguments", "");
                    std::string result = "[no result recorded]";
                    bool isError = false;
                    auto found = toolResults.find(tcId);
                    if (found != toolResults.end()) {
                        result = replayToolResultPreview(*found->second.content);
                        isError = found->second.isError;
                    }
                    emit toolCallStarted(QString::fromStdString(name),
                                         QString::fromStdString(args));
                    emit toolCallFinished(QString::fromStdString(name),
                                          QString::fromStdString(result),
                                          !isError);
                }
            }
        }
        markProgress();
    }

    emitContextStats();
    emit conversationReplayFinished();

    // Decide what to do next.
    if (compactForTrim_) {
        // Automatic compaction triggered by sendToLlm().
        if (bytesSaved > 0) {
            emit conversationTrimmed(bytesSaved);
        }
        compactForTrim_ = false;
        // Retry sendToLlm() — the budget check may now pass or fall through
        // to trimToBudget() as a last resort.
        sendToLlm();
    } else {
        // Manual compaction triggered by UI button.
        emit compactFinished(bytesSaved);
        saveCurrent();
        emitConversationsList();
    }
}

int AgentRunner::takeCheckpoint(const std::string& userMessage) {
    if (currentConvId_.empty()) return -1;

    // Backstop: persist the just-finished turn's snapshots into ITS OWN
    // checkpoint (the previous turn) in case finish()/fail() didn't run. The new
    // checkpoint must NOT inherit the previous turn's snapshots -- otherwise
    // reverting a turn that changed no files would wrongly undo the prior turn's
    // changes. This turn's snapshots are flushed into this checkpoint at finish.
    const int prevTurn = turnIndex_ - 1;
    if (prevTurn >= 0 && !fileBeforeSnapshots_.empty()) {
        std::vector<std::pair<std::string, std::string>> snaps;
        snaps.reserve(fileBeforeSnapshots_.size());
        for (const auto& [filePath, oldContent] : fileBeforeSnapshots_) {
            snaps.emplace_back(pathutil::toUtf8(filePath), oldContent);
        }
        try {
            store_.updateCheckpointSnapshots(projectRoot_, currentConvId_, prevTurn, snaps);
        } catch (...) { /* best-effort */ }
    }

    ConversationStore::Checkpoint cp;
    cp.turnIndex           = turnIndex_++;
    cp.userMessage         = userMessage;
    cp.conversationMessages = conv_.messages();

    fileBeforeSnapshots_.clear();
    fileChangeWarned_ = false;  // reset the per-turn many-files warning

    // Wire up fileBeforeSnapshots_ for the upcoming turn so WriteTool/EditTool
    // can populate it.
    ctx_.fileBeforeSnapshots = &fileBeforeSnapshots_;

    try {
        store_.saveCheckpoint(projectRoot_, currentConvId_, cp);
    } catch (const std::exception& e) {
        emit errorOccurred(QString::fromStdString(
            std::string("failed to save checkpoint: ") + e.what()));
    }
    return cp.turnIndex;
}

std::map<size_t, int> AgentRunner::buildReplayUserTurnMap(
        const std::vector<int>& turns,
        const std::string& convId) const {
    std::map<size_t, int> out;
    const auto& messages = conv_.messages();
    if (!messages.is_array()) return out;

    auto assignFromPrefix = [&](int checkpointTurn, const json& prefix) -> bool {
        if (!prefix.is_array() || prefix.size() > messages.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (i == 0
                && prefix[i].value("role", "") == "system"
                && messages[i].value("role", "") == "system") {
                continue;
            }
            if (prefix[i] != messages[i]) return false;
        }

        std::vector<size_t> userIndices;
        for (size_t i = 0; i < prefix.size(); ++i) {
            const auto& m = prefix[i];
            if (m.value("role", "") == "user" && !isCompactSummaryMessage(m)) {
                userIndices.push_back(i);
            }
        }

        int turn = checkpointTurn - static_cast<int>(userIndices.size());
        for (size_t userIndex : userIndices) {
            out[userIndex] = turn++;
        }

        for (size_t i = prefix.size(); i < messages.size(); ++i) {
            const auto& m = messages[i];
            if (m.value("role", "") == "user" && !isCompactSummaryMessage(m)) {
                out[i] = turn++;
            }
        }
        return true;
    };

    for (auto it = turns.rbegin(); it != turns.rend(); ++it) {
        try {
            auto cp = store_.loadCheckpoint(projectRoot_, convId, *it);
            if (assignFromPrefix(cp.turnIndex, checkpointMessagesBeforeTurn(cp))) {
                return out;
            }
        } catch (...) {
            // Ignore unreadable checkpoints and fall back to older anchors.
        }
    }

    int turn = 0;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& m = messages[i];
        if (m.value("role", "") == "user" && !isCompactSummaryMessage(m)) {
            out[i] = turn++;
        }
    }
    return out;
}

int AgentRunner::beginTurnForTest(const QString& userMessage) {
    cancelled_.store(false);
    running_.store(true);
    iter_ = 0;
    checkAttemptsThisTurn_ = 0;
    ensureSystemPrompt();
    const int turn = takeCheckpoint(userMessage.toStdString());
    conv_.addUser(userMessage.toStdString());
    saveCurrent();
    return turn;
}

ToolResult AgentRunner::dispatchToolForTest(const QString& name, const nlohmann::json& args) {
    return tools_.dispatch(name.toStdString(), args, ctx_);
}

void AgentRunner::endTurnForTest() {
    finish();
}

QString AgentRunner::currentConversationIdForTest() const {
    return QString::fromStdString(currentConvId_);
}

std::map<std::string, std::string> AgentRunner::collectRevertSnapshots(
        const ConversationStore& store, const fs::path& projectRoot,
        const std::string& convId, int targetTurn) {
    std::map<std::string, std::string> merged;
    try {
        auto cp = store.loadCheckpoint(projectRoot, convId, targetTurn);
        for (const auto& snap : cp.fileSnapshots) {
            merged.emplace(snap.first, snap.second);  // target's pre-image wins
        }
    } catch (...) { /* target checkpoint unreadable */ }

    for (int t : store.listCheckpoints(projectRoot, convId)) {
        if (t <= targetTurn) continue;
        try {
            auto laterCp = store.loadCheckpoint(projectRoot, convId, t);
            for (const auto& [filePathStr, oldContent] : laterCp.fileSnapshots) {
                merged.emplace(filePathStr, oldContent);  // earliest insertion wins
            }
        } catch (...) { /* skip unreadable checkpoints */ }
    }
    return merged;
}

std::vector<std::string> AgentRunner::applyRevertSnapshots(
        const std::map<std::string, std::string>& merged, const fs::path& projectRoot) {
    std::vector<std::string> failures;
    for (const auto& [filePathStr, oldContent] : merged) {
        fs::path filePath = pathutil::fromUtf8(filePathStr);
        std::error_code ec;
        // Clear any read-only attribute first; the file-writing tools do this
        // before every write, and the restore must too or it silently fails on
        // read-only files (e.g. a Perforce workspace).
        pathutil::makeWritable(filePath, projectRoot);
        if (oldContent.empty()) {
            // File was created in one of the reverted turns -- delete it.
            fs::remove(filePath, ec);
        } else {
            // Recreate parent dirs in case the file/tree was deleted in the turn.
            if (filePath.has_parent_path()) {
                fs::create_directories(filePath.parent_path(), ec);
            }
            std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(oldContent.data(), static_cast<std::streamsize>(oldContent.size()));
            }
            if (!out) failures.push_back(filePathStr);
        }
    }
    return failures;
}

std::string AgentRunner::describeUnparsableToolArgs(const std::string& parserError,
                                                    const std::string& argsStr) {
    std::string err = "could not parse tool arguments: " + parserError;

    // A large blob that fails to parse is almost always the model's output cut
    // mid-JSON at the provider's max-output-token limit (seen live: a ~28 KB
    // `write` whose string never closed). Echoing the whole broken blob back
    // only bloats the context, and retrying the same call fails the same way;
    // tell the model to split the content instead.
    constexpr size_t kLikelyTruncationBytes = 8 * 1024;
    constexpr size_t kEchoBytes = 500;
    if (argsStr.size() >= kLikelyTruncationBytes) {
        err += "\nThe arguments blob is " + std::to_string(argsStr.size())
             + " bytes and is not valid JSON, so the tool call was almost "
               "certainly truncated at the output-token limit. Do NOT retry "
               "the same call with the full content. Split it instead: `write` "
               "the first part of the file, then extend it with several "
               "`append` calls (or make several smaller `edit`/`replace_lines` "
               "calls).";
        err += "\nraw (first " + std::to_string(kEchoBytes) + " of "
             + std::to_string(argsStr.size()) + " bytes): "
             + argsStr.substr(0, kEchoBytes);
    } else {
        err += "\nraw: " + argsStr;
    }
    return err;
}

void AgentRunner::revertToCheckpoint(QString convId, int targetTurn) {
    if (running_.load()) {
        emit errorOccurred(QStringLiteral("cannot revert while agent is running"));
        return;
    }

    std::string convIdStr = convId.toStdString();

    try {
        // Load the target checkpoint.
        auto cp = store_.loadCheckpoint(projectRoot_, convIdStr, targetTurn);

        // 1. Restore files to their pre-turn state by merging the target
        // checkpoint's snapshots with all later ones, then writing them back.
        const auto merged = collectRevertSnapshots(store_, projectRoot_, convIdStr, targetTurn);
        for (const std::string& failed : applyRevertSnapshots(merged, projectRoot_)) {
            emit errorOccurred(QStringLiteral("revert: failed to restore %1")
                .arg(QString::fromStdString(failed)));
        }

        // 2. Restore the conversation state.
        cancelled_.store(false);
        running_.store(false);
        inAssistantTextRun_ = false;
        readSet_.clear();
        fileBeforeSnapshots_.clear();

        // Rebuild Conversation from the checkpointed messages.
        // Use fromJson on a temporary json object.
        {
            json temp;
            temp["project_root"] = pathutil::toUtf8(projectRoot_);
            temp["model"] = model_.toStdString();
            temp["messages"] = checkpointMessagesBeforeTurn(cp);
            conv_ = Conversation::fromJson(temp);
        }

        ctx_ = {};
        ctx_.projectRoot = projectRoot_;
        ctx_.readSet     = &readSet_;
        ctx_.cancelled   = &cancelled_;
        ctx_.fileBeforeSnapshots = &fileBeforeSnapshots_;
        ctx_.pendingUserQuestion = &pendingUserQuestion_;
        ctx_.pendingUserOptions  = &pendingUserOptions_;
        pendingToolCallId_.clear();
        pendingUserOptions_.clear();
        pendingIsCommandApproval_ = false;
        pendingToolCalls_ = json::array();
        pendingResumeIndex_ = 0;

        // 3. Rebuild UI via replay.
        const auto& replayMessages = conv_.messages();
        const size_t firstReplayIndex = replayStartIndex(replayMessages, visibleReplayMessages_);
        const int hiddenReplayMessages = static_cast<int>(firstReplayIndex);
        const int totalReplayMessages = static_cast<int>(
            replayMessages.size() - firstReplayIndex + (hiddenReplayMessages > 0 ? 1 : 0));
        emit conversationReplayStarted(totalReplayMessages, false);
        emit conversationCleared();
        int replayDone = 0;
        if (hiddenReplayMessages > 0) {
            emit conversationReplayHistoryHidden(hiddenReplayMessages);
            emit conversationReplayProgress(++replayDone, totalReplayMessages);
        }

        struct ToolResultInfo {
            const std::string* content = nullptr;
            bool isError = false;
        };
        std::map<std::string, ToolResultInfo> toolResults;
        for (const auto& m : replayMessages) {
            if (m.value("role", "") != "tool") continue;
            if (m.contains("content") && m["content"].is_string()) {
                ToolResultInfo info;
                info.content = &m["content"].get_ref<const std::string&>();
                info.isError = m.value("is_error", false);
                toolResults[m.value("tool_call_id", "")] = info;
            }
        }
        auto availableTurns = store_.listCheckpoints(projectRoot_, convIdStr);
        const auto userTurns = buildReplayUserTurnMap(availableTurns, convIdStr);
        for (size_t msgIndex = 0; msgIndex < replayMessages.size(); ++msgIndex) {
            const auto& m = replayMessages[msgIndex];
            if (msgIndex < firstReplayIndex) continue;

            auto markProgress = [&]() {
                ++replayDone;
                if (replayDone == totalReplayMessages
                    || replayDone % kReplayProgressStep == 0) {
                    emit conversationReplayProgress(replayDone, totalReplayMessages);
                }
            };

            std::string role = m.value("role", "");
            if (role == "system" || role == "tool") {
                markProgress();
                continue;
            }
            if (role == "user") {
                const auto mappedTurn = userTurns.find(msgIndex);
                const int turnForMessage = mappedTurn == userTurns.end() ? -1 : mappedTurn->second;
                bool canRevert = turnForMessage >= 0
                    && turnForMessage < targetTurn
                    && std::find(availableTurns.begin(), availableTurns.end(), turnForMessage)
                        != availableTurns.end();
                emit userMessageAdded(QString::fromStdString(m.value("content", "")),
                                      canRevert,
                                      turnForMessage);
            } else if (role == "assistant") {
                if (m.contains("content") && m["content"].is_string()) {
                    std::string c = m["content"].get<std::string>();
                    if (!c.empty()) {
                        emit assistantTextDelta(QString::fromStdString(c));
                        emit assistantTextFinalized();
                    }
                }
                if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                    for (const auto& tc : m["tool_calls"]) {
                        std::string tcId = tc.value("id", "");
                        std::string name = tc.value("function", json::object()).value("name", "");
                        std::string args = tc.value("function", json::object()).value("arguments", "");
                        std::string result = "[no result recorded]";
                        bool isError = false;
                        auto found = toolResults.find(tcId);
                        if (found != toolResults.end()) {
                            result = replayToolResultPreview(*found->second.content);
                            isError = found->second.isError;
                        }
                        emit toolCallStarted(QString::fromStdString(name),
                                             QString::fromStdString(args));
                        emit toolCallFinished(QString::fromStdString(name),
                                              QString::fromStdString(result),
                                              !isError);
                    }
                }
            }
            markProgress();
        }

        // 4. Save the reverted conversation and update turn index.
        // Always save even if the reverted conversation only has a system message
        // (e.g., reverting to turn 0 of a fresh conversation).
        if (!currentConvId_.empty()) {
            try {
                store_.save(projectRoot_, currentConvId_, conv_);
            } catch (...) { /* best-effort */ }
        }
        // The next user message should reuse the reverted-to turn index.
        turnIndex_ = targetTurn;

        // Remove checkpoints >= targetTurn since they're now invalid.
        for (int t : store_.listCheckpoints(projectRoot_, convIdStr)) {
            if (t >= targetTurn) {
                try {
                    store_.removeCheckpoint(projectRoot_, convIdStr, t);
                } catch (...) { /* best-effort */ }
            }
        }

        emitContextStats();
        emit conversationReplayFinished();
        emitConversationsList();

    } catch (const std::exception& e) {
        emit errorOccurred(QString::fromStdString(
            std::string("revert failed: ") + e.what()));
    }
}

void AgentRunner::ensureSystemPrompt() {
    if (!systemPromptNeedsRefresh_) return;
    // The conversation might already have a system prompt from loadConversation().
    // Only rebuild if it's missing or stale.
    const auto& msgs = conv_.messages();
    bool hasSystem = !msgs.empty() && msgs[0].value("role", "") == "system";
    if (!hasSystem || systemPromptNeedsRefresh_) {
        conv_.setSystemPrompt(SystemPrompt::build(projectRoot_, reasoningGuidanceEnabled_));
    }
    systemPromptNeedsRefresh_ = false;
}

void AgentRunner::sendToLlm() {
    try {
    ensureSystemPrompt();   // lazy: build system prompt + repomap only when sending
    if (cancelled_.load()) { fail(QStringLiteral("cancelled")); return; }
    if (++iter_ > maxIter_) {
        fail(QStringLiteral("agent exceeded %1 tool-loop iterations").arg(maxIter_));
        return;
    }

    // ---- Intelligent token budget trimming ----
    // Compute the budget: we have maxContextTokens_ total capacity, but must
    // reserve sendTokens_ for the user's new message + the assistant's reply.
    const int availableTokens = std::max(0, maxContextTokens_ - sendTokens_);
    const size_t budgetBytes = static_cast<size_t>(availableTokens) * 4;
    const size_t toolsBytes  = tools_.toolsArray().dump().size();

    // The tools schema is included in every request, so deduct it from the budget.
    size_t messagesBudget = (budgetBytes > toolsBytes) ? (budgetBytes - toolsBytes) : 0;

    // If we're over budget, try LLM-based compaction first (smarter than dropping).
    // Only attempt compaction if we aren't already inside a compaction callback
    // and haven't already compacted during this send.
    if (conv_.approxBytes() > messagesBudget) {
        // Count user exchanges to see if compaction is worthwhile.
        int exchangeCount = 0;
        for (const auto& m : conv_.messages())
            if (m.value("role", "") == "user") ++exchangeCount;

        if (exchangeCount >= 2 && !compactTriedThisSend_) {
            compactTriedThisSend_ = true;
            // Defer to the compaction flow; it will call sendToLlm() again when done.
            // Aim below the ceiling so auto-compaction lands around 40% rather
            // than barely under budget and immediately compacting again.
            doCompact((messagesBudget * 2) / 5, true);
            return;  // sendToLlm will be re-entered after compaction finishes
        }

        // Fallback: drop the oldest exchanges.
        size_t freed = conv_.trimToBudget(messagesBudget, 3);
        if (freed > 0) {
            emit conversationTrimmed(static_cast<int>(freed));
            emitContextStats();
        }
    }

    // If after trimming we still can't fit, emit a soft warning but still try sending.
    const size_t currentMsgBytes = conv_.approxBytes();
    if (currentMsgBytes > messagesBudget) {
        int estTokens = static_cast<int>((currentMsgBytes + toolsBytes) / 4);
        emit errorOccurred(QStringLiteral(
            "Warning: conversation size (~%1 tokens) exceeds the configured budget "
            "(%2 tokens). The API may reject the request.")
            .arg(estTokens)
            .arg(availableTokens));
    }

    // ---- Simple conversation repair ----
    // Remove any invalid messages that would make the API reject the request:
    //   - assistant messages with no content AND no tool_calls
    //   - orphaned tool results (tool result whose tool_call_id doesn't match
    //     any assistant tool_call that precedes it)
    //   - orphaned tool_calls (assistant with tool_calls but no matching tool
    //     results after it -- keep the assistant if it has text content, just
    //     strip the tool_calls; otherwise drop it entirely)
    {
        auto msgs = conv_.messages();
        bool changed = false;

        // Build set of all valid tool_call_ids from assistant messages.
        std::unordered_map<std::string, bool> validCallIds;
        for (const auto& m : msgs) {
            if (m.value("role", "") != "assistant") continue;
            if (!m.contains("tool_calls") || !m["tool_calls"].is_array()) continue;
            for (const auto& tc : m["tool_calls"]) {
                std::string id = tc.value("id", "");
                if (!id.empty()) validCallIds[id] = true;
            }
        }

        for (size_t i = 0; i < msgs.size(); ++i) {
            std::string role = msgs[i].value("role", "");

            if (role == "assistant") {
                bool hasContent = msgs[i].contains("content") &&
                                  msgs[i]["content"].is_string() &&
                                  !msgs[i]["content"].get<std::string>().empty();
                bool hasToolCalls = msgs[i].contains("tool_calls") &&
                                    msgs[i]["tool_calls"].is_array() &&
                                    !msgs[i]["tool_calls"].empty();

                // Remove assistant with neither content nor tool_calls.
                if (!hasContent && !hasToolCalls) {
                    msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(i));
                    --i;
                    changed = true;
                    continue;
                }

                if (!hasToolCalls) continue;

                // Count tool results following this assistant.
                int nCalls = static_cast<int>(msgs[i]["tool_calls"].size());
                int nTools = 0;
                for (size_t j = i + 1; j < msgs.size() && msgs[j].value("role", "") == "tool"; ++j)
                    ++nTools;

                if (nCalls != nTools) {
                    // Mismatch: strip tool_calls if assistant has text,
                    // otherwise drop the whole assistant.
                    if (hasContent) {
                        msgs[i].erase("tool_calls");
                    } else {
                        msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(i));
                        --i;
                    }
                    changed = true;
                }
                continue;
            }

            if (role == "tool") {
                // Drop orphaned tool results (no matching assistant tool_call).
                std::string id = msgs[i].value("tool_call_id", "");
                if (!id.empty() && !validCallIds.count(id)) {
                    msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(i));
                    --i;
                    changed = true;
                }
                continue;
            }
        }

        if (changed) {
            conv_ = Conversation::fromJson({{"project_root", pathutil::toUtf8(projectRoot_)},
                                            {"model", model_.toStdString()},
                                            {"messages", std::move(msgs)}});
        }
    }
    // ---- End simple repair ----

    // A new response is about to stream; discard any leftover partial from a
    // prior iteration so an interruption salvages only this request's reply.
    partialAssistantText_.clear();
    client_->sendStreaming(conv_.messages(), tools_.toolsArray());
    } catch (const std::exception& e) {
        fail(QString::fromStdString(std::string("send failed: ") + e.what()));
    } catch (...) {
        fail(QStringLiteral("send failed: unknown exception"));
    }
}

void AgentRunner::onTokenDelta(QString fragment) {
    if (!running_.load()) return;
    inAssistantTextRun_ = true;
    // Accumulate so an interrupted turn can still persist this reply. The full
    // assembled message supersedes this in onStreamFinished, which clears it.
    partialAssistantText_ += fragment.toStdString();
    emit assistantTextDelta(fragment);
}

void AgentRunner::onStreamFinished(json msg) {
    try {
    if (!running_.load()) return;

    streamRetries_ = 0;  // a stream completed; reset the transient-retry counter

    if (inAssistantTextRun_) {
        emit assistantTextFinalized();
        inAssistantTextRun_ = false;
    }

    bool hasToolCalls = msg.contains("tool_calls")
                     && msg["tool_calls"].is_array()
                     && !msg["tool_calls"].empty();

    conv_.addAssistant(msg);
    // The assembled message is now committed; drop the streamed partial so an
    // interruption later in the tool cycle can't re-append a stale copy.
    partialAssistantText_.clear();
    emitContextStats();

    if (!hasToolCalls) {
        // Before closing the turn, run the configured auto-verify check; a
        // failing check feeds the errors back and continues the cycle.
        if (maybeRunAutoVerify()) return;
        finish();
        return;
    }

    runToolCalls(msg["tool_calls"], 0);
    } catch (const std::exception& e) {
        fail(QString::fromStdString(std::string("stream handling failed: ") + e.what()));
    } catch (...) {
        fail(QStringLiteral("stream handling failed: unknown exception"));
    }
}

void AgentRunner::runToolCalls(const nlohmann::json& toolCalls, size_t startIdx) {
    try {
    for (size_t i = startIdx; i < toolCalls.size(); ++i) {
        const auto& tc = toolCalls[i];
        if (cancelled_.load()) { fail(QStringLiteral("cancelled")); return; }

        const std::string id      = tc.value("id", "");
        const auto&       fn      = tc.value("function", json::object());
        const std::string name    = fn.value("name", "");
        const std::string argsStr = fn.value("arguments", "");

        emit toolCallStarted(QString::fromStdString(name),
                             QString::fromStdString(argsStr));

        json args = json::object();
        bool parseOk = true;
        if (!argsStr.empty()) {
            try {
                args = json::parse(argsStr);
            } catch (const std::exception& e) {
                parseOk = false;
                std::string err = describeUnparsableToolArgs(e.what(), argsStr);
                conv_.addToolResult(id, err, true);
                emit toolCallFinished(QString::fromStdString(name),
                                      QString::fromStdString(err), false);
            }
        }
        if (parseOk) {
            const std::string nameForSink = name;
            ctx_.onOutput = [this, nameForSink](const std::string& chunk) {
                emit toolCallOutputDelta(QString::fromStdString(nameForSink),
                                         QString::fromStdString(chunk));
            };

            // Clear pending user question before dispatch; AskUserTool will set it.
            pendingUserQuestion_.clear();
            pendingToolCallId_.clear();
            pendingIsCommandApproval_ = false;

            // RAII guard to ensure onOutput is reset even if dispatch throws.
            struct OnOutputGuard {
                std::function<void(const std::string&)>& sink;
                ~OnOutputGuard() { sink = nullptr; }
            };
            OnOutputGuard guard{ctx_.onOutput};

            // Safety gate: classify bash commands before running them.
            if (name == "bash") {
                const std::string command = args.value("command", std::string{});
                const autocoder::shell::PolicyResult pol =
                    autocoder::shell::classifyCommand(command,
                                                      autocoder::shell::loadUserAllowlist());

                if (pol.verdict == autocoder::shell::Verdict::HardBlock) {
                    const std::string denial =
                        "Command blocked by AutoCoder safety policy (not permitted even with "
                        "approval): " + pol.reason
                        + (pol.offendingSegment.empty()
                               ? std::string{}
                               : "\nOffending part: " + pol.offendingSegment)
                        + "\nThis action is irreversible/catastrophic. Propose a safer "
                          "alternative or use the read/write/edit tools.";
                    conv_.addToolResult(id, denial, true);
                    emit toolCallFinished(QString::fromStdString(name),
                                          QString::fromStdString(denial), false);
                    emitContextStats();
                    continue;  // synchronous; keep processing the rest of the batch
                }

                if (pol.verdict == autocoder::shell::Verdict::Prompt) {
                    const std::string explanation = args.value("explanation", std::string{});
                    pendingIsCommandApproval_   = true;
                    pendingToolCallId_          = id;
                    pendingCommand_             = command;
                    pendingCommandReason_       = pol.reason;
                    pendingCommandExplanation_  = explanation;
                    awaitingInput_.store(true);
                    running_.store(false);  // provideCommandDecision() resets it
                    // Stash the calls AFTER this one so they still run on resume.
                    pendingToolCalls_ = toolCalls;
                    pendingResumeIndex_ = i + 1;
                    emit commandApprovalRequired(QString::fromStdString(id),
                                                 QString::fromStdString(command),
                                                 QString::fromStdString(pol.reason),
                                                 QString::fromStdString(explanation));
                    return;  // pause; provideCommandDecision() resumes the rest
                }
                // Allow: fall through to normal dispatch.
            }

            ToolResult r = tools_.dispatch(name, args, ctx_);

            // Check if the tool asked for user input.
            if (!pendingUserQuestion_.empty() && name == "ask_user") {
                // Remember which tool call we're waiting on.
                pendingToolCallId_ = id;
                awaitingInput_.store(true);
                // Build the options list for the UI.
                QStringList opts;
                opts.reserve(static_cast<int>(pendingUserOptions_.size()));
                for (const auto& o : pendingUserOptions_) {
                    opts << QString::fromStdString(o);
                }
                // Pause the loop -- mark as not running so new user messages
                // are handled properly. provideUserInput() will set running_ back.
                running_.store(false);
                // Stash the calls AFTER this one so they still run on resume.
                pendingToolCalls_ = toolCalls;
                pendingResumeIndex_ = i + 1;
                emit userInputRequired(QString::fromStdString(id),
                                       QString::fromStdString(name),
                                       QString::fromStdString(pendingUserQuestion_),
                                       opts);
                return;
            }

            conv_.addToolResult(id, r.content);
            emit toolCallFinished(QString::fromStdString(name),
                                  QString::fromStdString(r.content), r.ok);
        }
        emitContextStats();

        // Soft, non-blocking warning if this turn has touched a lot of files.
        constexpr size_t kManyFilesThreshold = 50;
        if (!fileChangeWarned_ && fileBeforeSnapshots_.size() >= kManyFilesThreshold) {
            fileChangeWarned_ = true;
            emit manyFilesChanged(static_cast<int>(fileBeforeSnapshots_.size()));
        }
    }

    // Verify tool call / tool result balance after the loop.
    // If this check fires, the tool result was NOT added in the loop above.
    {
        auto msgs = conv_.messages();
        for (int i = static_cast<int>(msgs.size()) - 1; i >= 0; --i) {
            if (msgs[i].value("role", "") == "assistant" &&
                msgs[i].contains("tool_calls")) {
                int nCalls = static_cast<int>(msgs[i]["tool_calls"].size());
                int nTools = 0;
                std::string callIds;
                for (const auto& tc : msgs[i]["tool_calls"]) {
                    if (!callIds.empty()) callIds += ",";
                    callIds += tc.value("id", "(no-id)");
                }
                std::string toolIds;
                for (int j = i + 1; j < static_cast<int>(msgs.size()); ++j) {
                    if (msgs[j].value("role", "") == "tool") {
                        ++nTools;
                        if (!toolIds.empty()) toolIds += ",";
                        toolIds += msgs[j].value("tool_call_id", "(no-id)");
                    } else break;
                }
                if (nCalls != nTools) {
                    emit errorOccurred(QStringLiteral(
                        "POST-LOOP CHECK FAILED: at msg %1 assistant has %2 tool_calls [%3] "
                        "but only %4 tool messages [%5] follow.")
                        .arg(i).arg(nCalls)
                        .arg(QString::fromStdString(callIds))
                        .arg(nTools)
                        .arg(QString::fromStdString(toolIds)));
                    return;
                }
                break;
            }
        }
    }

    sendToLlm();
    } catch (const std::exception& e) {
        fail(QString::fromStdString(std::string("stream handling failed: ") + e.what()));
    } catch (...) {
        fail(QStringLiteral("stream handling failed: unknown exception"));
    }
}

void AgentRunner::resumePendingToolCalls() {
    // Take a local copy first: runToolCalls may stash a new batch into
    // pendingToolCalls_ if a later call in this same batch pauses again.
    const json batch = pendingToolCalls_;
    const size_t start = pendingResumeIndex_;
    pendingToolCalls_ = json::array();
    pendingResumeIndex_ = 0;
    runToolCalls(batch, start);
}

void AgentRunner::onStreamError(QString detail) {
    if (compactInProgress_) {
        // Compaction error — clean up so the UI is not stuck in "busy" mode.
        compactInProgress_ = false;
        QObject::disconnect(compactConnection_);
        if (compactForTrim_) {
            compactForTrim_ = false;
            // running_ was already set by the auto-trim path inside sendToLlm;
            // fail() will reset it and emit turnFinished.
            fail(QStringLiteral("Compaction/trim failed: ") + detail);
            return;
        }
        emit errorOccurred(QStringLiteral("Compaction failed: ") + detail);
        return;
    }
    if (!running_.load()) return;

    // Auto-retry transient connection/network hiccups (e.g. "Connection closed"
    // mid-stream) instead of failing the turn and making the user type "continue".
    if (isRetryableStreamError(detail) && streamRetries_ < kMaxStreamRetries) {
        ++streamRetries_;
        // Drop any partially-streamed reply; the retry regenerates from scratch.
        // (It was never committed to the conversation, only shown in the UI.)
        inAssistantTextRun_ = false;
        partialAssistantText_.clear();
        emit streamRetrying(streamRetries_, kMaxStreamRetries);

        const int backoffMs = 400 * streamRetries_;  // 400 / 800 / 1200 ms
        // The retry reuses this iteration slot (sendToLlm increments iter_).
        if (iter_ > 0) --iter_;
        QTimer::singleShot(backoffMs, this, [this]() {
            if (!running_.load()) return;
            if (cancelled_.load()) { fail(QStringLiteral("cancelled")); return; }
            sendToLlm();
        });
        return;
    }

    fail(std::move(detail));
}

bool AgentRunner::maybeRunAutoVerify() {
    static constexpr int kMaxAutoVerifyAttempts = 2;
    if (checkCommand_.isEmpty()) return false;
    if (fileBeforeSnapshots_.empty()) return false;  // nothing modified this turn
    if (checkAttemptsThisTurn_ >= kMaxAutoVerifyAttempts) return false;
    if (cancelled_.load()) return false;

    ++checkAttemptsThisTurn_;
    const std::string cmd = checkCommand_.toStdString();

    emit toolCallStarted(QStringLiteral("auto_verify"),
                         QString::fromStdString(json{{"command", cmd}}.dump()));
    ctx_.onOutput = [this](const std::string& chunk) {
        emit toolCallOutputDelta(QStringLiteral("auto_verify"),
                                 QString::fromStdString(chunk));
    };
    // The command is user-configured (Settings), which is standing consent:
    // it intentionally does not go through the bash approval gate.
    ToolResult res = ToolResult::error("auto-verify did not run");
    try {
        res = autocoder::shell::runInternalShell(
            cmd, ctx_,
            ctx_.defaultBashTimeoutMs > 0 ? ctx_.defaultBashTimeoutMs : 120000,
            64 * 1024);
    } catch (const std::exception& e) {
        res = ToolResult::error(std::string("auto-verify failed to run: ") + e.what());
    } catch (...) {
        res = ToolResult::error("auto-verify failed to run: unknown exception");
    }
    ctx_.onOutput = nullptr;
    emit toolCallFinished(QStringLiteral("auto_verify"),
                          QString::fromStdString(res.content), res.ok);

    if (res.ok) return false;  // check passed; close the turn normally

    std::string feedback =
        "[Auto-verify] The configured check command failed after this turn's "
        "file changes (attempt " + std::to_string(checkAttemptsThisTurn_)
        + " of " + std::to_string(kMaxAutoVerifyAttempts) + ").\n"
        "Command: " + cmd + "\n"
        "Output:\n" + res.content + "\n"
        "Fix the problems. The check runs again automatically when you are done.";
    conv_.addUser(feedback);
    emit userMessageAdded(QString::fromStdString(feedback), false, -1);
    emitContextStats();

    // A fresh send may need compaction again.
    compactTriedThisSend_ = false;
    sendToLlm();
    return true;
}

void AgentRunner::finish() {
    running_.store(false);
    // Detach file snapshots reference (turn is done).
    ctx_.fileBeforeSnapshots = &fileBeforeSnapshots_;

    // Save the current turn's file snapshots into the checkpoint that was
    // taken at the start of this turn (turnIndex_ is already incremented
    // past it, so the checkpoint's turn is turnIndex_ - 1).
    if (!currentConvId_.empty() && turnIndex_ > 0 && !fileBeforeSnapshots_.empty()) {
        std::vector<std::pair<std::string, std::string>> snapshots;
        snapshots.reserve(fileBeforeSnapshots_.size());
        for (const auto& [path, content] : fileBeforeSnapshots_) {
            snapshots.emplace_back(pathutil::toUtf8(path), content);
        }
        try {
            store_.updateCheckpointSnapshots(projectRoot_, currentConvId_,
                                              turnIndex_ - 1, snapshots);
        } catch (...) { /* best-effort */ }
    }

    saveCurrent();
    emit turnFinished();
    emitConversationsList();
}

void AgentRunner::fail(QString detail) {
    running_.store(false);
    // Salvage any assistant text that streamed before this failure/interruption.
    // Without this, aborting an in-flight turn (e.g. switching conversations
    // mid-stream) would discard the reply entirely -- and if it was the turn's
    // first response, saveCurrent() would persist only [system, user], making
    // the whole conversation look emptied on reload. Commit the partial reply as
    // a plain assistant message so it survives; onStreamFinished never runs for
    // an aborted stream, so there is no double-commit.
    if (!partialAssistantText_.empty()) {
        json partial;
        partial["role"]    = "assistant";
        partial["content"] = partialAssistantText_;
        conv_.addAssistant(partial);
        partialAssistantText_.clear();
    }
    if (inAssistantTextRun_) {
        emit assistantTextFinalized();
        inAssistantTextRun_ = false;
    }
    // Flush file snapshots into the checkpoint so reverting to this turn
    // will undo the file changes (same as finish() does).
    if (!currentConvId_.empty() && turnIndex_ > 0 && !fileBeforeSnapshots_.empty()) {
        std::vector<std::pair<std::string, std::string>> snapshots;
        snapshots.reserve(fileBeforeSnapshots_.size());
        for (const auto& [path, content] : fileBeforeSnapshots_) {
            snapshots.emplace_back(pathutil::toUtf8(path), content);
        }
        try {
            store_.updateCheckpointSnapshots(projectRoot_, currentConvId_,
                                              turnIndex_ - 1, snapshots);
        } catch (...) { /* best-effort */ }
    }
    saveCurrent();
    emit errorOccurred(std::move(detail));
    emitConversationsList();
}

void AgentRunner::saveCurrent() {
    if (currentConvId_.empty()) return;
    if (conv_.messages().size() <= 1) return;
    try {
        store_.save(projectRoot_, currentConvId_, conv_);
    } catch (...) { /* save failures surface via emitConversationsList not refreshing */ }
}

void AgentRunner::emitContextStats() {
    size_t bytes = conv_.approxBytes() + tools_.toolsArray().dump().size();
    int approxTokens = static_cast<int>(bytes / 4);
    emit contextStats(static_cast<int>(conv_.messages().size()),
                      approxTokens,
                      static_cast<int>(bytes));
}

void AgentRunner::emitConversationsList() {
    diagnostics::TraceTimer timer("emitConversationsList");
    QStringList ids, titles, updated;
    auto entries = store_.list(projectRoot_);
    ids.reserve(static_cast<int>(entries.size()));
    titles.reserve(static_cast<int>(entries.size()));
    updated.reserve(static_cast<int>(entries.size()));
    for (const auto& e : entries) {
        ids     << QString::fromStdString(e.id);
        titles  << QString::fromStdString(e.title);
        updated << QString::fromStdString(e.updatedAtIso);
    }
    // The current conversation lives only in memory until its first user
    // message triggers a save (saveCurrent() skips empty conversations).
    // Prepend it so the sidebar shows and selects it immediately; once it is
    // saved, the store entry (with a derived title) takes its place.
    const QString currentId = QString::fromStdString(currentConvId_);
    if (!currentId.isEmpty() && !ids.contains(currentId)) {
        ids.prepend(currentId);
        titles.prepend(QStringLiteral("New conversation"));
        updated.prepend(QString());
    }
    emit conversationsListed(ids, titles, updated, currentId);
}

void AgentRunner::repairOrphanedToolCalls() {
    nlohmann::json msgs = conv_.messages();
    bool changed = false;

    // Build set of all valid tool_call_ids from assistant messages.
    std::unordered_map<std::string, bool> validCallIds;
    for (const auto& m : msgs) {
        if (m.value("role", "") != "assistant") continue;
        if (!m.contains("tool_calls") || !m["tool_calls"].is_array()) continue;
        for (const auto& tc : m["tool_calls"]) {
            std::string id = tc.value("id", "");
            if (!id.empty()) validCallIds[id] = true;
        }
    }

    for (size_t i = 0; i < msgs.size(); ++i) {
        std::string role = msgs[i].value("role", "");

        if (role == "assistant") {
            bool hasContent = msgs[i].contains("content") &&
                              msgs[i]["content"].is_string() &&
                              !msgs[i]["content"].get<std::string>().empty();
            bool hasToolCalls = msgs[i].contains("tool_calls") &&
                                msgs[i]["tool_calls"].is_array() &&
                                !msgs[i]["tool_calls"].empty();

            // Remove assistant with neither content nor tool_calls.
            if (!hasContent && !hasToolCalls) {
                msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(i));
                --i;
                changed = true;
                continue;
            }

            if (!hasToolCalls) continue;

            // Collect tool_call_ids from this assistant message.
            std::unordered_set<std::string> callIds;
            for (const auto& tc : msgs[i]["tool_calls"]) {
                callIds.insert(tc.value("id", ""));
            }

            // Check each following tool result: does its tool_call_id match?
            bool anyMissing = false;
            for (size_t j = i + 1; j < msgs.size() && msgs[j].value("role", "") == "tool"; ++j) {
                std::string tid = msgs[j].value("tool_call_id", "");
                if (!tid.empty() && callIds.find(tid) == callIds.end()) {
                    // This tool result references a call that isn't in this assistant message.
                    anyMissing = true;
                    break;
                }
                // Remove matched ids so we can detect missing ones.
            }

            // Count how many tool results we actually have following this message.
            int nCalls = static_cast<int>(callIds.size());
            int nTools = 0;
            for (size_t j = i + 1; j < msgs.size() && msgs[j].value("role", "") == "tool"; ++j)
                ++nTools;

            // If counts disagree or there's an ID mismatch, repair.
            if (nCalls != nTools || anyMissing) {
                if (hasContent) {
                    msgs[i].erase("tool_calls");
                } else {
                    msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(i));
                    --i;
                }
                changed = true;
            }
            continue;
        }

        if (role == "tool") {
            // Drop orphaned tool results (no matching assistant tool_call).
            std::string id = msgs[i].value("tool_call_id", "");
            if (!id.empty() && !validCallIds.count(id)) {
                msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(i));
                --i;
                changed = true;
            }
            continue;
        }
    }

    if (changed) {
        conv_ = Conversation::fromJson({{"project_root", pathutil::toUtf8(projectRoot_)},
                                        {"model", model_.toStdString()},
                                        {"messages", std::move(msgs)}});
    }
}
