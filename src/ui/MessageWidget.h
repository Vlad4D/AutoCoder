#pragma once

#include <QFrame>
#include <QString>
#include <QElapsedTimer>
#include <functional>

class QPushButton;
class QResizeEvent;
class QTextBrowser;
class ImagePreviewPane;

// One chat bubble. Role determines color/alignment. Content is rendered as
// Markdown (Qt::MarkdownText). Use appendText() during streaming to grow it.
//
// User messages can optionally carry a "revert" button that restores the
// conversation and all modified files to the state before this message was sent.
class MessageWidget : public QFrame {
    Q_OBJECT
public:
    enum class Role { User, Assistant };

    MessageWidget(Role role, QWidget* parent = nullptr);

    void setText(const QString& markdown);
    void appendText(const QString& fragment);
    void finalizeText();
    void setWorkingDir(const QString& workingDir);

    Role role() const { return role_; }

    // Attach a revert callback. Shows a small "Revert to here" button on the
    // bubble. Only meaningful for User-role bubbles; for Assistant it's a no-op.
    // The callback is invoked with no arguments when the user clicks revert.
    void setRevertCallback(std::function<void()> cb);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rerender();
    void scheduleRerender(bool immediate = false);
    // After setMarkdown(), walk the document and apply QTextCharFormat colors:
    // syntax-highlight fenced code blocks and colorize prose markup (headings,
    // bold, italic, links, inline `code`) so replies aren't a flat gray wall.
    void colorizeDocument();
    void refreshImagePreview();

    Role role_;
    QTextBrowser* body_;
    ImagePreviewPane* imagePreview_ = nullptr;
    QPushButton*  revertBtn_ = nullptr;
    QString buffer_;
    QString workingDir_;
    QElapsedTimer renderClock_;
    bool renderPending_ = false;
    bool inRerender_ = false;
};
