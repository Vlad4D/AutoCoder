#pragma once

#include <filesystem>
#include <vector>

#include <QMainWindow>
#include <QStringList>

class AgentRunner;
class ChatView;
class InputBar;
class QCloseEvent;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QThread;
class ToolCallWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    // Cross-thread requests to the worker-resident AgentRunner.
    void requestSetProvider(QString provider);
    void requestSetApiKey(QString key);
    void requestSetModel(QString model);
    void requestSetBaseUrl(QString url);
    void requestSetMaxIterations(int n);
    void requestSetBashTimeoutMs(int ms);
    void requestSetCheckCommand(QString cmd);
    void requestSetMaxContextTokens(int tokens);
    void requestSetSendTokens(int tokens);
    void requestSetReasoningGuidance(bool enabled);
    void requestSetProject(QString rootPath);
    void requestNewConversation();
    void requestLoadConversation(QString id);
    void requestDeleteConversation(QString id);
    void requestDeleteConversations(QStringList ids);
    void requestSubmitUserMessage(QString text);
    void requestCancel();
    void requestRevertToCheckpoint(QString convId, int turnIndex);
    void requestProvideUserInput(QString answer);
    void requestProvideCommandDecision(QString toolCallId, QString decision, bool persist);
    void requestCompactConversation();
    void requestLoadEarlierConversationHistory();

private slots:
    void onOpenProject();
    void onSendUserMessage(const QString& text);
    void onStop();
    void onNewConversation();
    void onSidebarItemClicked(QListWidgetItem* item);
    void onSidebarContextMenu(const QPoint& pos);
    void deleteSelectedConversations();

    // From AgentRunner (queued cross-thread).
    void onConversationCleared();
    void onConversationReplayStarted(int totalMessages, bool keepBusyAfterReplay);
    void onConversationReplayProgress(int messagesDone, int totalMessages);
    void onConversationReplayFinished();
    void onUserMessageAdded(const QString& text, bool canRevert, int turnIndex);
    void onAssistantTextDelta(const QString& fragment);
    void onAssistantTextFinalized();
    void onToolCallStarted(const QString& name, const QString& argsJson);
    void onToolCallOutputDelta(const QString& name, const QString& chunk);
    void onToolCallFinished(const QString& name, const QString& result, bool ok);
    void onTurnFinished();
    void onAgentError(const QString& detail);
    void onContextStats(int messages, int approxTokens, int bytes);
    void onTokenUsageStats(int cachedInputTokens, int uncachedInputTokens,
                           int outputTokens, int totalTokens);
    void onConversationsListed(QStringList ids, QStringList titles,
                               QStringList updatedAt, QString currentId);
    void onConversationTrimmed(int bytesFreed);
    void onUserInputRequired(QString toolCallId, QString toolName,
                              QString question, QStringList options);
    void onCommandApprovalRequired(QString toolCallId, QString command,
                                   QString reason, QString explanation);
    void onCompactStarted();
    void onCompactFinished(int bytesSaved);

private:
    void buildMenus();
    void setProjectRoot(const std::filesystem::path& p);
    void setBusy(bool busy);
    // If the agent is mid-turn, ask the user whether to interrupt it before
    // switching/creating a conversation. Returns true to proceed, false to
    // stay put. (The partial reply is preserved by AgentRunner on interrupt.)
    bool confirmInterruptIfBusy();
    // Re-select the current conversation's row in the sidebar (used to undo a
    // click the user cancelled out of).
    void reselectCurrentConversationRow();
    bool ensureApiKey();
    void pushSettingsToAgent();
    void applyFontSize();
    void saveLastConversationId();
    void revertToTurn(int turnIndex);
    void showConversationLoading();
    void hideConversationLoading();
    void updateContextLabel();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QListWidget*    sidebar_      = nullptr;
    ChatView*       chat_         = nullptr;
    InputBar*       input_        = nullptr;
    QLabel*         contextLabel_ = nullptr;
    QProgressBar*   replayProgress_ = nullptr;
    ToolCallWidget* activeToolCall_ = nullptr;
    // Keeps a reference to the most recently finished ToolCallWidget so that
    // replay can attach a resolved approval status to it. Reset by onToolCallStarted.
    ToolCallWidget* lastToolCall_ = nullptr;

    QThread*        worker_       = nullptr;
    AgentRunner*    agent_        = nullptr;     // lives on worker_

    std::filesystem::path projectRoot_;
    QString currentConversationId_;
    bool    replayInProgress_ = false;
    bool    keepBusyAfterReplay_ = false;
    bool    scrollToTopAfterReplay_ = false;
    bool    startingUp_ = false;  // true during startup restore; suppresses intermediate clears
    bool    currentConversationPersisted_ = false;

    // Track turn indices so we can show a revert button on each user message.
    // Incremented each time onUserMessageAdded fires.
    int currentTurnIndex_ = 0;

    // Track context usage percentage so we can suggest /compact
    // when the conversation gets large. Updated by onContextStats().
    int lastTokenPct_ = 0;
    int maxContextTokens_ = 1048576;
    int reservedSendTokens_ = 8192;
    int lastContextMessages_ = 0;
    int lastApproxTokens_ = 0;
    int lastContextBytes_ = 0;
    bool hasTokenUsageStats_ = false;
    int lastCachedInputTokens_ = 0;
    int lastUncachedInputTokens_ = 0;
    int lastOutputTokens_ = 0;
    int lastTotalTokens_ = 0;
};
