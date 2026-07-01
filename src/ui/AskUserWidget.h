#pragma once

#include <QFrame>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

// Inline widget displayed in the chat when the LLM calls ask_user().
// Shows the question and either clickable option buttons or a free-text input.
// Emits answered(QString) when the user responds.
class AskUserWidget : public QFrame {
    Q_OBJECT
public:
    // If options is non-empty, render choice buttons; otherwise render a
    // QLineEdit + send button.
    AskUserWidget(const QString& question, const QStringList& options,
                  QWidget* parent = nullptr);

signals:
    // Emitted when the user picks an option or types a response and hits send.
    void answered(const QString& text);

private:
    void onOptionClicked(const QString& text);
    void onSendClicked();

    QLabel* questionDisplay_ = nullptr;
    QLineEdit*    inputField_      = nullptr;
    QPushButton*  sendBtn_         = nullptr;
    QVBoxLayout*  layout_          = nullptr;
};
