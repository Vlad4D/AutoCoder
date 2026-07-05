#pragma once

#include <QScrollArea>
#include <QStringList>
#include <functional>

class QKeyEvent;
class MessageWidget;
class ToolCallWidget;
class AskUserWidget;
class QVBoxLayout;
class QWidget;

// Scrollable list of MessageWidgets and ToolCallWidgets. Auto-scrolls to bottom
// when new content is appended. Tracks the currently-streaming assistant bubble.
class ChatView : public QScrollArea {
    Q_OBJECT
public:
    explicit ChatView(QWidget* parent = nullptr);
    void setWorkingDir(const QString& workingDir);

    // appendUser with optional revert callback. When non-null, the user message
    // bubble gets a "Revert to here" button.
    MessageWidget*  appendUser(const QString& text,
                               std::function<void()> revertCb = nullptr);

    // Attach a revert callback to the most recently appended user message.
    // Used by the two-phase send flow: the user bubble is shown immediately
    // (no button), then the button is enabled once the checkpoint exists.
    // No-op if there is no user message (e.g. the chat was cleared).
    void            setLastUserMessageRevert(std::function<void()> revertCb);
    MessageWidget*  beginAssistant();
    void            appendToActiveAssistant(const QString& fragment);
    void            endAssistant();
    // Remove the in-progress assistant bubble entirely (e.g. a partial stream
    // that errored and is being retried). No-op if there is none.
    void            discardActiveAssistant();

    ToolCallWidget* appendToolCall(const QString& name, const QString& argsJson,
                                   const QString& workingDir = {});

    // Replay helper: appends a tool-call bubble with its result already filled in.
    ToolCallWidget* appendCompletedToolCall(const QString& name, const QString& argsJson,
                                            const QString& result, bool ok);

    // Append an inline ask_user widget showing the question and either option
    // buttons or a free-text input. Returns the widget so the caller can connect
    // to its answered() signal.
    AskUserWidget* appendAskUser(const QString& question, const QStringList& options);

    // Append a collapsed summary entry for a compaction event.
    // Rendered as a tool-call-style frame with the summary text expandable.
    void appendCompactSummary(const QString& summary, int bytesSaved);
    void appendHiddenHistoryNotice(int hiddenMessages);

    void            beginBulkAppend();
    void            endBulkAppend();
    void            clear();
    void            scrollToTop();
    void            scrollToBottom();

signals:
    void loadEarlierRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool            isAtBottom() const;

    QWidget*       container_;
    QVBoxLayout*   layout_;
    QString        workingDir_;
    MessageWidget* activeAssistant_ = nullptr;
    bool           stickToBottom_ = true;
    int            bulkAppendDepth_ = 0;
};
