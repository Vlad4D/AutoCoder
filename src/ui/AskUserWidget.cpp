#include "AskUserWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

AskUserWidget::AskUserWidget(const QString& question, const QStringList& options,
                             QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setObjectName(QStringLiteral("askUserWidget"));

    // Style like assistant bubbles but with a distinct accent.
    setStyleSheet(
        "QFrame#askUserWidget { background-color: #1E3A5F; border: 1px solid #3B82F6;"
        "  border-radius: 8px; }"
        "QFrame#askUserWidget QLabel { background: transparent; border: none;"
        "  color: #E5E7EB; }"
        "QFrame#askUserWidget QPushButton {"
        "  background-color: #3B82F6; color: white; border: none;"
        "  border-radius: 6px; padding: 6px 16px; }"
        "QFrame#askUserWidget QPushButton:hover { background-color: #2563EB; }"
        "QFrame#askUserWidget QLineEdit {"
        "  background-color: #2A2A2E; color: #E5E7EB; border: 1px solid #4B5563;"
        "  border-radius: 6px; padding: 6px 10px; }");

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(12, 10, 12, 10);
    layout_->setSpacing(8);

    // Question text.
    questionDisplay_ = new QLabel(this);
    questionDisplay_->setTextFormat(Qt::MarkdownText);
    questionDisplay_->setWordWrap(true);
    questionDisplay_->setText(question);
    layout_->addWidget(questionDisplay_);

    if (options.isEmpty()) {
        // Free-text input row.
        auto* row = new QHBoxLayout();
        row->setSpacing(6);

        inputField_ = new QLineEdit(this);
        inputField_->setPlaceholderText(QStringLiteral("Type your answer..."));
        connect(inputField_, &QLineEdit::returnPressed, this, &AskUserWidget::onSendClicked);
        row->addWidget(inputField_, 1);

        sendBtn_ = new QPushButton(QStringLiteral("Send"), this);
        connect(sendBtn_, &QPushButton::clicked, this, &AskUserWidget::onSendClicked);
        row->addWidget(sendBtn_, 0);

        layout_->addLayout(row);

        // Focus the input after layout.
        QTimer::singleShot(0, inputField_, SLOT(setFocus()));
    } else {
        // Option buttons.
        auto* btnLayout = new QVBoxLayout();
        btnLayout->setSpacing(6);
        for (const QString& opt : options) {
            auto* btn = new QPushButton(opt, this);
            btn->setCursor(Qt::PointingHandCursor);
            connect(btn, &QPushButton::clicked, this, [this, opt]() {
                onOptionClicked(opt);
            });
            btnLayout->addWidget(btn);
        }
        layout_->addLayout(btnLayout);
    }

}

void AskUserWidget::onOptionClicked(const QString& text) {
    // Highlight the clicked button with a checkmark and disable all others.
    for (auto* btn : findChildren<QPushButton*>()) {
        btn->setEnabled(false);
        btn->setCursor(Qt::ArrowCursor);
        if (btn->text() == text) {
            btn->setText(QStringLiteral("✓ ") + btn->text());
            btn->setStyleSheet(
                "background-color: #374151; color: #6EE7B7; border: 2px solid #34D399;"
                "  border-radius: 6px; padding: 6px 16px; font-weight: bold;");
        } else {
            btn->setStyleSheet(
                "background-color: #374151; color: #6B7280; border: 1px solid #4B5563;"
                "  border-radius: 6px; padding: 6px 16px;");
        }
    }
    emit answered(text);
}

void AskUserWidget::onSendClicked() {
    if (!inputField_) return;
    QString text = inputField_->text().trimmed();
    if (text.isEmpty()) return;
    // Show the answer as read-only text by replacing the input row.
    inputField_->hide();
    sendBtn_->hide();
    auto* answerLabel = new QLabel(this);
    answerLabel->setTextFormat(Qt::PlainText);
    answerLabel->setText(QStringLiteral("\u2192 %1").arg(text));
    answerLabel->setStyleSheet(
        "color: #34D399; font-weight: bold; padding: 4px 0;");
    layout_->addWidget(answerLabel);
    emit answered(text);
}
