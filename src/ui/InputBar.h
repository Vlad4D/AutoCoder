#pragma once

#include <QFrame>
#include <QStringList>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;

// Composer at the bottom of the chat. Multi-line input; Enter sends, Shift+Enter
// inserts a newline. Up/Down at the first/last line recall previous user
// messages from a per-session history (shell-style).
class InputBar : public QFrame {
    Q_OBJECT
public:
    explicit InputBar(QWidget* parent = nullptr);

    void setBusy(bool busy);
    void clear();
    void setDraft(const QString& text);
    void focusComposer();

    // Add a sent message to the recall history. Idempotent against trailing
    // duplicates (consecutive identical entries are collapsed).
    void pushHistory(const QString& text);

    // Forget all recalled messages (e.g. when the active conversation changes).
    void clearHistory();

signals:
    void send(QString text);
    void stop();
    void compactRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onTextChanged();

private:
    void recallHistoryAt(int index);

    // Resize the composer to fit its content: 3 lines at rest, growing up to a
    // cap as the user types.
    void adjustComposerHeight();

    // Resting composer height (3 text lines). The Send/Stop buttons and spinner
    // are sized to this so the button is the same height as the composer
    // (inline), not a small control beside a taller box. Computed from the font.
    int composerRestHeight_ = 0;

    QPlainTextEdit* edit_;
    QPushButton*    sendBtn_;
    QPushButton*    stopBtn_;

    // Animated "still busy" spinner shown next to Stop while the agent works.
    QLabel*         spinner_       = nullptr;
    QTimer*         spinnerTimer_  = nullptr;
    int             spinnerFrame_  = 0;

    QStringList history_;
    int     historyIndex_   = -1;     // -1 == not navigating
    QString pendingDraft_;            // saved on first Up so Down restores it
    bool    applyingHistory_ = false; // guards onTextChanged during programmatic updates
};
