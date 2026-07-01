#include "CommandApprovalWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

CommandApprovalWidget::CommandApprovalWidget(const QString& command, const QString& reason,
                                             const QString& explanation,
                                             const QString& resolvedDecision, QWidget* parent)
    : QFrame(parent)
{
    const bool resolved = !resolvedDecision.isEmpty();

    setFrameShape(QFrame::StyledPanel);
    setObjectName(QStringLiteral("commandApprovalWidget"));

    // Amber accent to distinguish a safety prompt from the blue ask_user bubble.
    setStyleSheet(
        "QFrame#commandApprovalWidget { background-color: #3A2E12; border: 1px solid #D97706;"
        "  border-radius: 8px; }"
        "QFrame#commandApprovalWidget QLabel { color: #E5E7EB; }"
        "QFrame#commandApprovalWidget QLabel#cmd {"
        "  background-color: #1F2937; color: #FBBF24; border: 1px solid #4B5563;"
        "  border-radius: 6px; padding: 8px 10px; font-family: Consolas, monospace; }"
        "QFrame#commandApprovalWidget QPushButton {"
        "  border: none; border-radius: 6px; padding: 6px 14px; }");

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(12, 10, 12, 10);
    layout_->setSpacing(8);

    auto* title = new QLabel(
        resolved ? QStringLiteral("⚠ Command required your approval")
                 : QStringLiteral("⚠ AutoCoder wants to run a command"), this);
    title->setStyleSheet(QStringLiteral("font-weight: bold; color: #FBBF24;"));
    layout_->addWidget(title);

    // The LLM's stated purpose for the command -- what it is trying to achieve.
    if (!explanation.isEmpty()) {
        auto* why = new QLabel(explanation, this);
        why->setTextFormat(Qt::PlainText);
        why->setWordWrap(true);
        why->setStyleSheet(QStringLiteral("color: #E5E7EB;"));
        layout_->addWidget(why);
    }

    auto* cmd = new QLabel(command, this);
    cmd->setObjectName(QStringLiteral("cmd"));
    cmd->setTextFormat(Qt::PlainText);
    cmd->setWordWrap(true);
    cmd->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout_->addWidget(cmd);

    if (!reason.isEmpty()) {
        auto* why = new QLabel(QStringLiteral("Needs your OK because it %1.").arg(reason), this);
        why->setWordWrap(true);
        why->setStyleSheet(QStringLiteral("color: #D1D5DB;"));
        layout_->addWidget(why);
    }

    // Resolved (reloaded) mode: show a read-only status of the past decision.
    if (resolved) {
        QString text;
        QString color;
        if (resolvedDecision == QStringLiteral("deny")) {
            text = QStringLiteral("✗ Denied");           color = QStringLiteral("#F87171");
        } else if (resolvedDecision == QStringLiteral("always")) {
            text = QStringLiteral("✓ Always allowed");   color = QStringLiteral("#34D399");
        } else {
            text = QStringLiteral("✓ Allowed once");     color = QStringLiteral("#34D399");
        }
        auto* status = new QLabel(text, this);
        status->setStyleSheet(QStringLiteral("font-weight: bold; color: %1;").arg(color));
        layout_->addWidget(status);
        return;
    }

    auto* row = new QHBoxLayout();
    row->setSpacing(6);

    auto* allowBtn = new QPushButton(QStringLiteral("Allow once"), this);
    allowBtn->setCursor(Qt::PointingHandCursor);
    allowBtn->setStyleSheet(QStringLiteral("background-color: #059669; color: white;"));
    connect(allowBtn, &QPushButton::clicked, this,
            [this, allowBtn]() { finish(QStringLiteral("allow_once"), false, allowBtn); });

    auto* alwaysBtn = new QPushButton(QStringLiteral("Always allow this command"), this);
    alwaysBtn->setCursor(Qt::PointingHandCursor);
    alwaysBtn->setStyleSheet(QStringLiteral("background-color: #2563EB; color: white;"));
    connect(alwaysBtn, &QPushButton::clicked, this,
            [this, alwaysBtn]() { finish(QStringLiteral("allow_once"), true, alwaysBtn); });

    auto* denyBtn = new QPushButton(QStringLiteral("Deny"), this);
    denyBtn->setCursor(Qt::PointingHandCursor);
    denyBtn->setStyleSheet(QStringLiteral("background-color: #B91C1C; color: white;"));
    connect(denyBtn, &QPushButton::clicked, this,
            [this, denyBtn]() { finish(QStringLiteral("deny"), false, denyBtn); });

    row->addWidget(allowBtn);
    row->addWidget(alwaysBtn);
    row->addWidget(denyBtn);
    row->addStretch(1);
    layout_->addLayout(row);
}

void CommandApprovalWidget::finish(const QString& decision, bool persist, QPushButton* chosen) {
    // Disable all buttons. Since Qt ignores the disabled state when an explicit
    // background-color is set in the stylesheet, we override the style to muted
    // colors for all buttons. The chosen one keeps a slightly recognizable tint.
    for (auto* btn : findChildren<QPushButton*>()) {
        btn->setEnabled(false);
        btn->setCursor(Qt::ArrowCursor);
        if (btn == chosen) {
            QString text = btn->text();
            if (!text.startsWith(QStringLiteral("✓ ")))
                btn->setText(QStringLiteral("✓ ") + text);
            btn->setStyleSheet(QStringLiteral(
                "background-color: #374151; color: #6EE7B7;"
                " border: 1px solid #6EE7B7; border-radius: 6px; padding: 6px 14px;"
                " font-weight: bold;"));
        } else {
            btn->setStyleSheet(QStringLiteral(
                "background-color: #374151; color: #6B7280;"
                " border-radius: 6px; padding: 6px 14px;"));
        }
    }
    emit decided(decision, persist);
}
