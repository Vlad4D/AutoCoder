#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>
#include <nlohmann/json.hpp>

#include "agent/Conversation.h"
#include "persistence/ConversationStore.h"
#include "tools/ToolContext.h"
#include "tools/ToolRegistry.h"

class LlmClient;

// Owns the conversation state, the LLM client, the tool registry, and the
// on-disk store, and runs the LLM tool-use cycle. Designed to live on a worker
// QThread; everything in the public API is either thread-affinity-free state
// (isRunning) or a slot that the UI calls via Qt's queued connection.
//
// Checkpointing: before every user message, a checkpoint is taken that saves
// the current conversation state + snapshots of all files that were modified
// by LLM tool calls. A "revert to checkpoint" restores the conversation + files.
class AgentRunner : public QObject {
    Q_OBJECT
public:
    explicit AgentRunner(QObject* parent = nullptr);
    ~AgentRunner() override;

    bool isRunning() const { return running_.load(); }

    // ----- Revert helpers (static; pure, exposed for testing) -----
    // Merge the file snapshots needed to revert to `targetTurn`: the target
    // checkpoint plus every later one (earliest insertion wins, so the target's
    // pre-image is kept). Keys are toUtf8 canonical paths; "" means the file did
    // not exist before the turn (so revert should delete it).
    static std::map<std::string, std::string> collectRevertSnapshots(
        const ConversationStore& store, const std::filesystem::path& projectRoot,
        const std::string& convId, int targetTurn);
    // Restore files from merged snapshots (clearing read-only first; "" => delete).
    // Returns the toUtf8 paths that failed to write.
    static std::vector<std::string> applyRevertSnapshots(
        const std::map<std::string, std::string>& merged,
        const std::filesystem::path& projectRoot);

    // Build the tool-result message for tool-call arguments that failed to
    // parse as JSON. Large blobs are diagnosed as output-token truncation and
    // the model is told to split the content (write + append) instead of
    // retrying the same oversized call; only a prefix of the raw blob is
    // echoed back. (Static; pure, exposed for testing.)
    static std::string describeUnparsableToolArgs(const std::string& parserError,
                                                  const std::string& argsStr);

    // ----- Test-only seams: drive one turn without the LLM -----
    // beginTurnForTest takes a checkpoint and adds the user message (as
    // submitUserMessage does, minus the network call) and returns the turn
    // index. dispatchToolForTest runs a real tool against this turn's context
    // (populating the file snapshots). endTurnForTest finishes the turn,
    // flushing those snapshots into the checkpoint -- exactly the path revert
    // depends on.
    int        beginTurnForTest(const QString& userMessage);
    ToolResult dispatchToolForTest(const QString& name, const nlohmann::json& args);
    void       endTurnForTest();
    QString    currentConversationIdForTest() const;
    // Runs the auto-verify gate exactly as onStreamFinished would before
    // closing a turn. Returns maybeRunAutoVerify()'s takeover flag.
    bool       runAutoVerifyForTest() { return maybeRunAutoVerify(); }
    nlohmann::json messagesForTest() const { return conv_.messages(); }
    // Feed a streamed assistant-text fragment exactly as the LLM client would
    // (drives onTokenDelta so partialAssistantText_ accumulates).
    void       feedAssistantTokenForTest(const QString& fragment) { onTokenDelta(fragment); }
    // Interrupt the running turn exactly as an aborted stream does (client_->abort()
    // -> onStreamError("cancelled") -> fail()). Exercises the partial-reply salvage.
    void       interruptTurnForTest() { onStreamError(QStringLiteral("cancelled")); }
    // Returns true if any async activity is in progress (tool cycle, compaction,
    // or waiting for user input). Use this to guard UI actions like closing.
    bool isBusy() const {
        return running_.load() || compactInProgress_.load() || awaitingInput_.load();
    }

public slots:
    // Configuration (no side effects until next request).
    void setProvider(QString provider);
    void setApiKey(QString key);
    void setModel(QString model);
    void setBaseUrl(QString url);
    void setMaxIterations(int n);
    void setDefaultBashTimeoutMs(int ms);
    void setMaxContextTokens(int tokens);
    void setSendTokens(int tokens);
    // Sampling temperature for LLM requests. Negative = provider default.
    void setTemperature(double t);
    // Auto-verify: shell command run after a turn that modified files (empty =
    // disabled). On failure the output is fed back to the model to fix.
    void setCheckCommand(QString cmd);

    // Enable or disable the "Reasoning approach" section in the system prompt.
    // Default true (enabled). Takes effect on the next LLM request.
    void setReasoningGuidance(bool enabled);

    // Switch the active project root. Resets state, refreshes the sidebar list,
    // and starts a new empty conversation.
    void setProject(QString rootPath);

    // Start a fresh conversation in the current project.
    void newConversation();

    // Remove a saved conversation from disk. Re-emits conversationsListed.
    void deleteConversation(QString id);

    // Remove several saved conversations from disk in one batch. If the current
    // conversation is among them, starts a fresh one. Re-emits conversationsListed once.
    void deleteConversations(QStringList ids);

    // Replay a saved conversation. Emits conversationCleared then a stream of
    // userMessageAdded / assistantTextDelta+assistantTextFinalized /
    // toolCallStarted+toolCallFinished signals so the UI can rebuild itself.
    void loadConversation(QString id);
    void loadEarlierConversationHistory();

    // Submit a user message and run the tool cycle until the assistant returns
    // a text-only response.
    void submitUserMessage(QString text);

    // Cooperative cancel: aborts in-flight HTTP and signals tools to bail.
    void cancel();

    // Set only the (atomic) cancelled_ flag. Safe to call directly from the UI
    // thread via Qt::DirectConnection: when the worker thread is blocked inside a
    // synchronous tool (e.g. a long bash), the queued cancel() cannot run, but a
    // running tool polls ctx_.cancelled and will observe this flag immediately.
    void flagCancelled() { cancelled_.store(true); }

    // ----- Checkpoint control -----

    // Restore the conversation and all modified files to the state they were
    // in at the given checkpoint turn.  This is a destructive operation that
    // replaces the in-memory conversation (and re-emits all signals so the UI
    // rebuilds) AND overwrites any files that were modified in subsequent turns.
    void revertToCheckpoint(QString convId, int turnIndex);

    // ----- ask_user / compact support -----

    // Provide the user's answer to a pending ask_user query.
    // Resumes the tool cycle with the answer injected as the tool result.
    void provideUserInput(QString answer);

    // Provide the user's decision on a pending bash-command approval.
    // decision is "allow_once" or "deny"; persist adds the FULL exact command
    // to the always-allow list. On allow the command runs here, then the cycle
    // resumes; on deny a denial result is returned to the LLM.
    void provideCommandDecision(QString toolCallId, QString decision, bool persist);

    // Compact the conversation: ask the LLM to summarize/concisely compress
    // the conversation so far, then replace the older messages with the summary
    // to free up token budget. Emits conversationCleared + replay signals
    // so the UI rebuilds with the compressed view.
    void compactConversation();

signals:
    // ----- Conversation rendering -----
    void conversationCleared();
    void conversationReplayStarted(int totalMessages, bool keepBusyAfterReplay);
    void conversationReplayProgress(int messagesDone, int totalMessages);
    void conversationReplayHistoryHidden(int hiddenMessages);
    void conversationReplayFinished();
    void userMessageAdded(QString text, bool canRevert, int turnIndex);
    // Emitted after the initial userMessageAdded (with canRevert=false) once
    // the checkpoint is ready, so the UI can enable the revert button.
    // Carries the owning conversation id: delivery can lag message-add by
    // seconds (system prompt build), during which the user may have switched
    // conversations -- the button must never be wired to the wrong one.
    void userMessageRevertReady(QString convId, int turnIndex);
    void assistantTextDelta(QString fragment);
    void assistantTextFinalized();
    void toolCallStarted(QString name, QString argsJson);
    void toolCallOutputDelta(QString name, QString chunk);
    void toolCallFinished(QString name, QString result, bool ok);
    void turnFinished();
    void errorOccurred(QString detail);

    // ----- Status / sidebar -----
    // Emitted around a system-prompt (re)build. The repo map walk + symbol
    // extraction can take seconds on large projects, so the UI shows why the
    // first message of a conversation pauses before the LLM request starts.
    void systemPromptBuildStarted();
    void systemPromptBuildFinished(int promptBytes);
    void contextStats(int messageCount, int approxTokens, int bytesTotal);
    void tokenUsageStats(int cachedInputTokens, int uncachedInputTokens,
                         int outputTokens, int totalTokens);
    void conversationsListed(QStringList ids,
                             QStringList titles,
                             QStringList updatedAt,
                             QString currentId);

    // Emitted when the conversation is trimmed due to budget limits.
    // Indicates the oldest exchange(s) were dropped, freeing  bytes.
    void conversationTrimmed(int bytesFreed);

    // Emitted when a transient stream/network error is being auto-retried, so
    // the UI can drop any partial streamed reply and show a brief status instead
    // of an error. attempt is 1-based; maxAttempts is the cap.
    void streamRetrying(int attempt, int maxAttempts);

    // Emitted once per turn when the agent has modified an unusually large
    // number of files, so the UI can surface a non-blocking warning.
    void manyFilesChanged(int fileCount);

    // Emitted when the LLM calls ask_user() and is waiting for a response.
    // The UI should prompt the user and call provideUserInput() with the answer.
    // options is an optional list of predefined answer choices (may be empty).
    void userInputRequired(QString toolCallId, QString toolName, QString question,
                           QStringList options);

    // Emitted when the bash tool wants to run a command that is not on the
    // silent allowlist. The UI should show an Allow/Deny prompt and call
    // provideCommandDecision() with the result.
    void commandApprovalRequired(QString toolCallId, QString command,
                                 QString reason, QString explanation);

    // Emitted during replay for a bash call that previously went through the
    // approval gate, so the UI can re-render a resolved (read-only) approval
    // widget. decision is "allow_once", "always", or "deny".
    void commandApprovalResolved(QString command, QString reason,
                                 QString explanation, QString decision);

    // Emitted when compactConversation() starts/finishes, so the UI can show
    // a status message.
    void compactStarted();
    void compactFinished(int bytesSaved);

    // Emitted during compaction replay to insert a collapsed summary widget
    // instead of a plain user bubble.
    void compactSummaryAppended(QString summary, int bytesSaved);

private slots:
    void onTokenDelta(QString fragment);
    void onStreamFinished(nlohmann::json msg);
    void onStreamError(QString detail);
    void onCompactResponse(nlohmann::json msg, size_t splitIndex);

private:
    void sendToLlm();
    void ensureSystemPrompt();   // sets system prompt (with repomap) if not set yet
    // Process the assistant's tool calls in [startIdx, end). Each call's result
    // is appended to the conversation. If a call pauses the cycle (ask_user or a
    // bash-command approval), the remaining calls are stashed in
    // pendingToolCalls_/pendingResumeIndex_ and this returns without sending;
    // the matching provide*() resumes them via resumePendingToolCalls(). When
    // the batch completes it runs the call/result balance check and sends to the
    // LLM.
    void runToolCalls(const nlohmann::json& toolCalls, size_t startIdx);
    // Continue the tool calls that followed the one we paused on (no-op tail =>
    // just balance-check + send). Called from provideUserInput/CommandDecision.
    void resumePendingToolCalls();
    // Run the configured check command after a turn that modified files.
    // Returns true if it took over the cycle (check failed; failure output was
    // appended to the conversation and sent to the LLM to fix). Returns false
    // when the turn should finish normally (disabled, nothing modified, check
    // passed, or attempts exhausted).
    bool maybeRunAutoVerify();
    void finish();
    void fail(QString detail);
    void emitContextStats();
    void emitConversationsList();
    void saveCurrent();
    // messagesBeforeUser: the conversation state to checkpoint (pre-user-
    // message). Null (default) checkpoints the current conversation as-is.
    // Taken by value so callers can std::move a snapshot in.
    int takeCheckpoint(const std::string& userMessage,
                       nlohmann::json messagesBeforeUser = nullptr);
    std::map<size_t, int> buildReplayUserTurnMap(const std::vector<int>& turns,
                                                 const std::string& convId) const;
    void repairOrphanedToolCalls();
    bool ensureCompactionWorthwhile();
    void doCompact(size_t messagesBudget, bool forTrim);
    void replayLoadedConversation(const std::vector<int>& turns, bool keepBusyAfterReplay);

    Conversation       conv_;
    LlmClient*    client_ = nullptr;     // child of *this*; Qt deletes on parent destruction
    ToolRegistry       tools_;
    ConversationStore  store_;

    std::filesystem::path projectRoot_;
    std::string currentConvId_;
    std::set<std::filesystem::path> readSet_;

    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};
    bool inAssistantTextRun_ = false;
    // Assistant text streamed for the current request but not yet committed to
    // the conversation (onStreamFinished commits the assembled message). Kept so
    // an interrupted turn -- e.g. switching conversations mid-stream -- can
    // persist the partial reply instead of discarding it. Cleared before each
    // send, after a successful stream, and on transient-retry.
    std::string partialAssistantText_;
    int iter_ = 0;
    int maxIter_ = 25;
    int turnIndex_ = 0;    // incremented before each user message

    // Token budget settings (DeepSeek V4 API context window is 1M tokens).
    int maxContextTokens_ = 1048576;
    int sendTokens_ = 8192;   // headroom reserved for the assistant's reply

    ToolContext ctx_{};

    // File content snapshots collected during the current turn.
    // A map<canonical_path, old_content> populated by WriteTool/EditTool.
    std::map<std::filesystem::path, std::string> fileBeforeSnapshots_;

    // ask_user support: when the LLM asks a question, we pause the cycle.
    std::string pendingToolCallId_;   // tool_call_id of the pending ask_user / approval call
    std::string pendingUserQuestion_; // the question being asked
    std::vector<std::string> pendingUserOptions_; // optional predefined answer choices

    // bash command-approval support: when a command needs the user's OK, we pause
    // the cycle before executing it. pendingIsCommandApproval_ disambiguates the
    // resume path (provideCommandDecision) from the ask_user one (provideUserInput).
    bool pendingIsCommandApproval_ = false;
    std::string pendingCommand_;             // the exact command awaiting approval
    std::string pendingCommandReason_;       // why it needs approval (shown / sent to LLM)
    std::string pendingCommandExplanation_;  // the LLM's stated purpose for the command

    // Remaining tool calls of the current assistant batch, stashed when a call
    // pauses the cycle so the calls AFTER it still run on resume (otherwise their
    // tool_call_ids would never get results and the next request would be
    // malformed). pendingResumeIndex_ is the index of the first not-yet-run call.
    nlohmann::json pendingToolCalls_ = nlohmann::json::array();
    size_t pendingResumeIndex_ = 0;

    // Auto-verify state: command configured in Settings; attempts are counted
    // per user turn so a failing check cannot loop forever.
    QString checkCommand_;
    int checkAttemptsThisTurn_ = 0;

    QString apiKey_;
    QString provider_ = QStringLiteral("deepseek");
    QString model_ = QStringLiteral("deepseek-chat");

    std::atomic<bool> compactInProgress_{false};
    QMetaObject::Connection compactConnection_; // Connection for compaction response, cleaned up on error
    std::atomic<bool> compactForTrim_{false};
    bool    compactTriedThisSend_ = false;
    std::atomic<bool> awaitingInput_{false}; // true while waiting for ask_user response
    bool    fileChangeWarned_ = false;       // one-shot per-turn many-files warning
    int     streamRetries_ = 0;              // consecutive transient-error retries
    size_t  visibleReplayMessages_ = 240;
    bool    systemPromptNeedsRefresh_ = true; // regenerate system prompt before next send
    bool    reasoningGuidanceEnabled_ = true; // include "Reasoning approach" in system prompt
};
