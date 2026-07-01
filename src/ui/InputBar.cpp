#include "InputBar.h"

#include <algorithm>

#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextDocument>
#include <QSizePolicy>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

InputBar::InputBar(QWidget* parent) : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);

    edit_ = new QPlainTextEdit(this);
    edit_->setPlaceholderText(QStringLiteral(
        "Ask AutoCoder…  (Enter to send · Shift+Enter for newline · ↑/↓ for history)"));
    // Auto-growing composer: compact (~2 lines) at rest, expanding as the user
    // types up to a cap (see adjustComposerHeight). Fixed vertical policy so the
    // layout honors the height we set rather than QPlainTextEdit's tall default.
    edit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    edit_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    edit_->installEventFilter(this);
    connect(edit_, &QPlainTextEdit::textChanged, this, &InputBar::onTextChanged);

    // Resting height = 3 text lines. The buttons and spinner are sized to this so
    // they stay the same height as the composer (inline) rather than floating.
    {
        const QFontMetrics fm(edit_->font());
        composerRestHeight_ = fm.lineSpacing() * 3 + edit_->frameWidth() * 2 + 8;
    }

    sendBtn_ = new QPushButton(QStringLiteral("Send"), this);
    connect(sendBtn_, &QPushButton::clicked, this, [this]() {
        QString text = edit_->toPlainText().trimmed();
        if (text.isEmpty()) return;
        // Handle slash commands.
        if (text == QStringLiteral("/compact")) {
            emit compactRequested();
            return;
        }
        emit send(text);
    });

    stopBtn_ = new QPushButton(QStringLiteral("Stop"), this);
    stopBtn_->setVisible(false);
    connect(stopBtn_, &QPushButton::clicked, this, &InputBar::stop);

    // Same fixed size for both so swapping Send<->Stop doesn't shift the row.
    // Bold so Send reads as the primary action.
    constexpr int kButtonWidth = 88;
    sendBtn_->setFixedSize(kButtonWidth, composerRestHeight_);
    stopBtn_->setFixedSize(kButtonWidth, composerRestHeight_);
    sendBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background:#2563EB; color:#FFFFFF; border:none;"
        " border-radius:4px; font-weight:bold; }"
        "QPushButton:hover { background:#1D4ED8; }"
        "QPushButton:pressed { background:#1E40AF; }"
        "QPushButton:disabled { background:#374151; color:#9CA3AF; }"));
    stopBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background:#B91C1C; color:#FFFFFF; border:none;"
        " border-radius:4px; font-weight:bold; }"
        "QPushButton:hover { background:#991B1B; }"
        "QPushButton:pressed { background:#7F1D1D; }"));

    // Animated busy spinner, sitting just left of the Send/Stop button on the
    // same row. ALWAYS present (blank when idle) with a fixed size so swapping
    // idle<->busy never shifts the button.
    spinner_ = new QLabel(this);
    spinner_->setAlignment(Qt::AlignCenter);
    spinner_->setFixedSize(28, composerRestHeight_);
    spinner_->setStyleSheet(QStringLiteral(
        "color:#93C5FD; background:transparent; font-family:Consolas, monospace; font-size:22px;"));

    spinnerTimer_ = new QTimer(this);
    spinnerTimer_->setInterval(100);
    connect(spinnerTimer_, &QTimer::timeout, this, [this]() {
        static const char* kFrames[] = {
            "⠋", "⠙", "⠹", "⠸", "⠼",
            "⠴", "⠦", "⠧", "⠇", "⠏"  // ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏
        };
        constexpr int kCount = int(sizeof(kFrames) / sizeof(kFrames[0]));
        spinnerFrame_ = (spinnerFrame_ + 1) % kCount;
        spinner_->setText(QString::fromUtf8(kFrames[spinnerFrame_]));
    });

    // Button cluster: spinner + Send/Stop on a single row, so the button stays
    // inline with the composer (Stop occupies the same slot as Send).
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->addWidget(spinner_);
    btnRow->addWidget(sendBtn_);
    btnRow->addWidget(stopBtn_);

    // Composer + button cluster live in a width-capped container so they don't
    // stretch edge-to-edge on a wide/maximized window; the container is centered.
    constexpr int kMaxContentWidth = 1000;
    auto* inner = new QWidget(this);
    inner->setMaximumWidth(kMaxContentWidth);
    auto* innerRow = new QHBoxLayout(inner);
    innerRow->setContentsMargins(0, 0, 0, 0);
    innerRow->addWidget(edit_, 1);
    innerRow->addLayout(btnRow);
    // Anchor the cluster to the top so the button stays on the composer's first
    // line when the composer grows past its resting height.
    innerRow->setAlignment(btnRow, Qt::AlignTop);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 8, 8, 8);
    // Stretches on both sides center the container; the large middle stretch lets
    // it fill the width on narrow windows but yields to the side stretches once it
    // reaches kMaxContentWidth, so it stays centered (not edge-to-edge) when wide.
    row->addStretch(1);
    row->addWidget(inner, 1000);
    row->addStretch(1);

    adjustComposerHeight();   // 3-line resting height, flush with the button
}

void InputBar::adjustComposerHeight() {
    // Height that fits the document, clamped between ~2 lines and a cap so the
    // bar stays compact when empty but grows (then scrolls) for long input.
    // Note: QPlainTextDocumentLayout::documentSize() reports height in *lines*,
    // not pixels, so convert via the font's line spacing.
    const QFontMetrics fm(edit_->font());
    const int frame = int(edit_->frameWidth()) * 2;
    const int pad   = 8;  // breathing room around the text
    const int minH  = composerRestHeight_;  // rest at 3 lines, flush with the button
    const int maxH  = std::max(160, composerRestHeight_);
    const double lines =
        edit_->document()->documentLayout()->documentSize().height();
    const int contentH = int(lines * fm.lineSpacing()) + frame + pad;
    edit_->setFixedHeight(std::clamp(contentH, minH, maxH));
}

void InputBar::setBusy(bool busy) {
    edit_->setReadOnly(false);
    sendBtn_->setVisible(!busy);
    stopBtn_->setVisible(busy);

    // Keep the spinner's slot reserved either way (don't toggle visibility);
    // just blank it when idle so the input bar height stays constant.
    if (busy) {
        spinnerFrame_ = 0;
        spinner_->setText(QString::fromUtf8("⠋"));
        spinnerTimer_->start();
    } else {
        spinnerTimer_->stop();
        spinner_->clear();
    }
}

void InputBar::clear() {
    applyingHistory_ = true;
    edit_->clear();
    applyingHistory_ = false;
    historyIndex_ = -1;
    pendingDraft_.clear();
}

void InputBar::setDraft(const QString& text) {
    applyingHistory_ = true;
    edit_->setPlainText(text);
    QTextCursor c = edit_->textCursor();
    c.movePosition(QTextCursor::End);
    edit_->setTextCursor(c);
    applyingHistory_ = false;
    historyIndex_ = -1;
    pendingDraft_.clear();
    focusComposer();
}

void InputBar::focusComposer() {
    edit_->setFocus();
}

void InputBar::pushHistory(const QString& text) {
    if (text.isEmpty()) return;
    if (!history_.isEmpty() && history_.last() == text) {
        // Collapse consecutive duplicates so re-sending doesn't bloat history.
    } else {
        history_.append(text);
    }
    historyIndex_ = -1;
    pendingDraft_.clear();
}

void InputBar::clearHistory() {
    history_.clear();
    historyIndex_ = -1;
    pendingDraft_.clear();
}

void InputBar::recallHistoryAt(int index) {
    applyingHistory_ = true;
    edit_->setPlainText(history_[index]);
    QTextCursor c = edit_->textCursor();
    c.movePosition(QTextCursor::End);
    edit_->setTextCursor(c);
    applyingHistory_ = false;
}

void InputBar::onTextChanged() {
    adjustComposerHeight();   // grow/shrink with content (also during programmatic edits)
    if (applyingHistory_) return;
    // Any user-driven edit exits history-navigation mode.
    if (historyIndex_ != -1) {
        historyIndex_ = -1;
        pendingDraft_.clear();
    }
}

bool InputBar::eventFilter(QObject* obj, QEvent* event) {
    if (obj != edit_ || event->type() != QEvent::KeyPress) {
        return QFrame::eventFilter(obj, event);
    }
    auto* k = static_cast<QKeyEvent*>(event);
    const int key      = k->key();
    const bool shift   = (k->modifiers() & Qt::ShiftModifier);
    const bool ctrl    = (k->modifiers() & Qt::ControlModifier);
    const bool noMods  = (k->modifiers() == Qt::NoModifier);

    // Enter to send (Shift+Enter for newline).
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && !shift && sendBtn_->isVisible()) {
        QString text = edit_->toPlainText().trimmed();
        if (!text.isEmpty() && text == QStringLiteral("/compact")) {
            emit compactRequested();
        } else if (!text.isEmpty()) {
            sendBtn_->click();
        }
        return true;
    }

    // Up/Down: only intercept on the edge lines and only without modifiers
    // (so things like Shift-Up text selection still work).
    if (key == Qt::Key_Up && noMods) {
        QTextCursor cur = edit_->textCursor();
        if (cur.blockNumber() == 0) {
            if (historyIndex_ == -1) {
                if (history_.isEmpty()) return false;
                pendingDraft_ = edit_->toPlainText();
                historyIndex_ = history_.size() - 1;
            } else if (historyIndex_ > 0) {
                historyIndex_--;
            } else {
                return true;  // already at oldest
            }
            recallHistoryAt(historyIndex_);
            return true;
        }
    }

    if (key == Qt::Key_Down && noMods) {
        if (historyIndex_ == -1) return false;
        QTextCursor cur = edit_->textCursor();
        if (cur.blockNumber() != edit_->document()->blockCount() - 1) return false;
        historyIndex_++;
        if (historyIndex_ >= history_.size()) {
            historyIndex_ = -1;
            applyingHistory_ = true;
            edit_->setPlainText(pendingDraft_);
            QTextCursor c = edit_->textCursor();
            c.movePosition(QTextCursor::End);
            edit_->setTextCursor(c);
            applyingHistory_ = false;
            pendingDraft_.clear();
        } else {
            recallHistoryAt(historyIndex_);
        }
        return true;
    }

    // Suppress unused-warning for ctrl (kept for future shortcuts).
    (void)ctrl;
    return QFrame::eventFilter(obj, event);
}
