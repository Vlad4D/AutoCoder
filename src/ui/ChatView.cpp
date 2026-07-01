#include "AskUserWidget.h"
#include "ChatView.h"

#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QDir>
#include <QPushButton>
#include <QScrollBar>
#include <QSpacerItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

#include "MessageWidget.h"
#include "ToolCallWidget.h"

ChatView::ChatView(QWidget* parent) : QScrollArea(parent) {
    workingDir_ = QDir::currentPath();
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);

    container_ = new QWidget(this);
    container_->installEventFilter(this);
    layout_ = new QVBoxLayout(container_);
    layout_->setContentsMargins(16, 16, 16, 16);
    layout_->setSpacing(10);
    layout_->addStretch(1);  // pushes content to top until full

    setWidget(container_);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        stickToBottom_ = isAtBottom();
    });

    // Follow streaming/new content only while the user is already at the bottom.
    // Expanding an older tool bubble changes the range too, but should not yank
    // the view away from what the user is inspecting.
    connect(verticalScrollBar(), &QScrollBar::rangeChanged,
            this, [this](int /*min*/, int /*max*/) {
        if (bulkAppendDepth_ > 0) return;
        if (stickToBottom_) scrollToBottom();
            });
}

void ChatView::setWorkingDir(const QString& workingDir) {
    workingDir_ = workingDir;
}

MessageWidget* ChatView::appendUser(const QString& text,
                                     std::function<void()> revertCb) {
    activeAssistant_ = nullptr;
    auto* m = new MessageWidget(MessageWidget::Role::User, container_);
    m->setWorkingDir(workingDir_);
    m->setText(text);
    if (revertCb) {
        m->setRevertCallback(std::move(revertCb));
    }
    layout_->insertWidget(layout_->count() - 1, m);
    if (bulkAppendDepth_ == 0) {
        QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
    }
    return m;
}

MessageWidget* ChatView::beginAssistant() {
    activeAssistant_ = new MessageWidget(MessageWidget::Role::Assistant, container_);
    activeAssistant_->setWorkingDir(workingDir_);
    layout_->insertWidget(layout_->count() - 1, activeAssistant_);
    if (bulkAppendDepth_ == 0) {
        QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
    }
    return activeAssistant_;
}

void ChatView::appendToActiveAssistant(const QString& fragment) {
    if (!activeAssistant_) {
        beginAssistant();
    }
    activeAssistant_->appendText(fragment);
    if (bulkAppendDepth_ == 0) {
        QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
    }
}

void ChatView::endAssistant() {
    if (activeAssistant_) {
        activeAssistant_->finalizeText();
    }
    activeAssistant_ = nullptr;
}

void ChatView::discardActiveAssistant() {
    if (activeAssistant_) {
        layout_->removeWidget(activeAssistant_);
        activeAssistant_->deleteLater();
        activeAssistant_ = nullptr;
    }
}

ToolCallWidget* ChatView::appendToolCall(const QString& name, const QString& argsJson,
                                         const QString& workingDir) {
    activeAssistant_ = nullptr;
    auto* w = new ToolCallWidget(name, argsJson, workingDir, container_);
    layout_->insertWidget(layout_->count() - 1, w);
    if (bulkAppendDepth_ == 0) {
        QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
    }
    return w;
}

ToolCallWidget* ChatView::appendCompletedToolCall(const QString& name, const QString& argsJson,
                                                  const QString& result, bool ok) {
    auto* w = appendToolCall(name, argsJson);
    w->setResult(result, ok);
    return w;
}

AskUserWidget* ChatView::appendAskUser(const QString& question, const QStringList& options) {
    activeAssistant_ = nullptr;
    auto* w = new AskUserWidget(question, options, container_);
    layout_->insertWidget(layout_->count() - 1, w);
    if (bulkAppendDepth_ == 0) {
        QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
    }
    return w;
}

void ChatView::appendCompactSummary(const QString& summary, int bytesSaved) {
    activeAssistant_ = nullptr;
    auto* w = new ToolCallWidget(QStringLiteral("compact"), QStringLiteral("{}"), {}, container_);
    QString body = summary;
    if (bytesSaved > 0) {
        body = QStringLiteral("Freed %1 KB\n\n%2").arg(bytesSaved / 1024).arg(summary);
    }
    w->setResult(body, true);
    layout_->insertWidget(layout_->count() - 1, w);
    if (bulkAppendDepth_ == 0) {
        QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
    }
}

void ChatView::appendHiddenHistoryNotice(int hiddenMessages) {
    activeAssistant_ = nullptr;
    auto* frame = new QFrame(container_);
    frame->setObjectName("historyNotice");
    frame->setStyleSheet(
        "QFrame#historyNotice { background-color: #20242B; color: #D1D5DB;"
        " border: 1px solid #374151; border-radius: 6px; }"
        "QFrame#historyNotice QPushButton { background-color: #374151; color: #E5E7EB;"
        " border: 1px solid #4B5563; border-radius: 4px; padding: 4px 10px; }"
        "QFrame#historyNotice QPushButton:hover { background-color: #4B5563; }"
        "QLabel { color: #D1D5DB; }");
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 6, 10, 6);
    auto* label = new QLabel(
        QStringLiteral("Older history hidden for fast loading: %1 saved messages.")
            .arg(hiddenMessages),
        frame);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* button = new QPushButton(QStringLiteral("Load earlier"), frame);
    button->setToolTip(QStringLiteral("Render the next older chunk of this conversation."));
    connect(button, &QPushButton::clicked, this, &ChatView::loadEarlierRequested);
    layout->addWidget(button, 0, Qt::AlignHCenter);
    layout_->insertWidget(layout_->count() - 1, frame);
    if (bulkAppendDepth_ == 0) {
        QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
    }
}

void ChatView::beginBulkAppend() {
    ++bulkAppendDepth_;
    if (bulkAppendDepth_ == 1) {
        stickToBottom_ = true;
        viewport()->setUpdatesEnabled(false);
        container_->setUpdatesEnabled(false);
    }
}

void ChatView::endBulkAppend() {
    if (bulkAppendDepth_ <= 0) return;
    --bulkAppendDepth_;
    if (bulkAppendDepth_ != 0) return;

    container_->setUpdatesEnabled(true);
    viewport()->setUpdatesEnabled(true);
    layout_->activate();
    container_->updateGeometry();
    viewport()->update();
    QTimer::singleShot(0, this, [this]() { scrollToBottom(); });
}

void ChatView::clear() {
    activeAssistant_ = nullptr;
    while (layout_->count() > 1) {
        QLayoutItem* item = layout_->takeAt(0);
        if (auto* w = item->widget()) delete w;
        delete item;
    }
}

void ChatView::scrollToBottom() {
    if (!stickToBottom_) return;
    QScrollBar* vb = verticalScrollBar();
    vb->setValue(vb->maximum());
    stickToBottom_ = true;
}

void ChatView::scrollToTop() {
    QScrollBar* vb = verticalScrollBar();
    vb->setValue(vb->minimum());
    stickToBottom_ = false;
}

bool ChatView::isAtBottom() const {
    const QScrollBar* vb = verticalScrollBar();
    return vb->value() >= vb->maximum() - 4;
}

void ChatView::keyPressEvent(QKeyEvent* event) {
    QScrollBar* vb = verticalScrollBar();
    const int step = vb->pageStep();  // one "page" = viewport height

    switch (event->key()) {
    case Qt::Key_PageUp:
        vb->setValue(vb->value() - step);
        event->accept();
        return;
    case Qt::Key_PageDown:
        vb->setValue(vb->value() + step);
        event->accept();
        return;
    case Qt::Key_Home:
        vb->setValue(vb->minimum());
        event->accept();
        return;
    case Qt::Key_End:
        vb->setValue(vb->maximum());
        event->accept();
        return;
    default:
        QScrollArea::keyPressEvent(event);
    }
}

bool ChatView::eventFilter(QObject* watched, QEvent* event) {
    // Forward key presses from child widgets (e.g. QTextBrowser inside MessageWidget)
    // to our own keyPressEvent so PageUp/PageDown/Home/End scroll the chat,
    // not the inner text widget.
    if (watched != this && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        switch (ke->key()) {
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            keyPressEvent(ke);
            return ke->isAccepted();
        default:
            break;
        }
    }
    return QScrollArea::eventFilter(watched, event);
}
