#pragma once

#include <QFrame>
#include <QString>

class QPushButton;
class QVBoxLayout;

// Inline widget shown in the chat when the bash tool wants to run a command that
// is not on the silent allowlist. Shows the command + the reason it needs review
// and three actions. Emits decided() once the user picks one.
class CommandApprovalWidget : public QFrame {
    Q_OBJECT
public:
    // `explanation` is the LLM's stated purpose for the command (may be empty).
    // Interactive when resolvedDecision is empty (emits decided()); otherwise a
    // read-only record of a past decision ("allow_once"/"always"/"deny"), used
    // when replaying a reloaded conversation.
    CommandApprovalWidget(const QString& command, const QString& reason,
                          const QString& explanation = QString(),
                          const QString& resolvedDecision = QString(),
                          QWidget* parent = nullptr);

signals:
    // decision is "allow_once" or "deny"; persist is true when the user chose
    // "Always allow this exact command".
    void decided(const QString& decision, bool persist);

private:
    void finish(const QString& decision, bool persist, QPushButton* chosen);

    QVBoxLayout* layout_ = nullptr;
};
