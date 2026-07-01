#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QString>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QResizeEvent;
class QTextBrowser;
class QToolButton;
class ImagePreviewPane;

// Collapsible display of a single tool call. Header line shows name + status;
// expanded body shows the JSON args and the (possibly truncated) result.
// For bash commands that need user approval, approval buttons are shown at the
// bottom of the widget (instead of using a separate CommandApprovalWidget).
class ToolCallWidget : public QFrame {
    Q_OBJECT
public:
    ToolCallWidget(const QString& name, const QString& argsJson,
                   const QString& workingDir = {}, QWidget* parent = nullptr);

    // Live-streaming output (e.g. from BashTool). Append-only; safe to call
    // many times. Also auto-expands on the first chunk so the user sees progress.
    void appendOutput(const QString& chunk);

    // Final state. If output has been streamed, the streamed buffer wins;
    // otherwise `result` becomes the displayed body.
    void setResult(const QString& result, bool ok);

    // Enable approval mode for bash commands that need user OK.
    // Shows "Allow once", "Always allow", "Deny" buttons at the bottom.
    // Emits decided() when the user picks one.
    void setApprovalRequired(const QString& reason, const QString& explanation);

    // Read-only resolved state for replay: shows the past decision instead of buttons.
    void setApprovalResolved(const QString& decision);

signals:
    // Emitted when the user makes an approval decision.
    // decision is "allow_once" or "deny"; persist is true for "Always allow".
    void decided(const QString& decision, bool persist);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void setExpanded(bool expanded);
    void refreshStyle();
    void refreshHeader();
    void refreshBody();
    void scrollBodyToBottom();
    void finishApproval(const QString& decision, bool persist, QPushButton* chosen);
    QString lineDeltaSummary() const;
    QString byteSummary() const;
    QString formatBashBody() const;
    QString formatEditBody() const;
    QString formatReplaceLinesBody() const;
    QString formatWriteBody() const;
    QString formatReadBody() const;
    QString formatGrepBody() const;
    QString formatReadOutlineBody() const;
    void refreshImagePreview(const QString& renderedBody);

    QString        name_;
    QToolButton*   expandBtn_;
    QLabel*        header_;
    QTextBrowser*  body_;
    ImagePreviewPane* imagePreview_ = nullptr;
    QString        argsJson_;
    QString        output_;        // streamed-or-final body content
    QString        fileRef_;       // extracted path/pattern from args (for display)
    QElapsedTimer  timer_;         // measures wall-clock duration of the call
    QString        workingDir_;    // project root for relative path display
    bool           finished_ = false;
    bool           ok_ = false;
    bool           expanded_ = false;
    bool           approvalActive_ = false;  // true while waiting for user decision
    QWidget*       approvalRow_ = nullptr;   // widget containing the approval buttons/status
};
