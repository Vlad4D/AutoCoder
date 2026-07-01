#include "MessageWidget.h"

#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QPushButton>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextFragment>
#include <QTimer>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

#include "CodeColorizer.h"
#include "ImagePreview.h"

namespace {

constexpr int kStreamRenderIntervalMs = 180;

// Prose markup palette (dark theme, on the #2A2A2E assistant bubble). Picked to
// be vivid -- replies are mostly prose, so flat text reads as "uncolored".
const QString kColHeading   = QStringLiteral("#93C5FD");  // headings (light blue)
const QString kColBold      = QStringLiteral("#FBBF24");  // **bold** (amber)
const QString kColItalic    = QStringLiteral("#C4B5FD");  // *italic* (light purple)
const QString kColInlineCode= QStringLiteral("#E5C07B");  // `code` (warm)
const QString kColLink      = QStringLiteral("#60A5FA");  // links (blue)

}

MessageWidget::MessageWidget(Role role, QWidget* parent)
    : QFrame(parent), role_(role) {
    setFrameShape(QFrame::StyledPanel);
    setObjectName(role == Role::User ? "userBubble" : "assistantBubble");

    body_ = new QTextBrowser(this);
    body_->setOpenExternalLinks(true);
    body_->setFrameShape(QFrame::NoFrame);
    body_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    if (role == Role::User) {
        setStyleSheet(
            "QFrame#userBubble { background-color: #1E3A5F; border-radius: 8px; }"
            "QFrame#userBubble QTextBrowser { background: transparent; border: none;"
            "  color: #E5E7EB; selection-background-color: #3B82F6;"
            "  selection-color: #FFFFFF; }"
            "QFrame#userBubble QPushButton {"
            "  background-color: #374151; color: #D1D5DB; border: 1px solid #4B5563;"
            "  border-radius: 4px; padding: 2px 8px; font-size: 11px; }"
            "QFrame#userBubble QPushButton:hover { background-color: #4B5563; }");
    } else {
        setStyleSheet(
            "QFrame#assistantBubble { background-color: #2A2A2E; border-radius: 8px; }"
            "QFrame#assistantBubble QTextBrowser { background: transparent; border: none;"
            "  color: #E5E7EB; selection-background-color: #4338CA;"
            "  selection-color: #FFFFFF; }");
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->addWidget(body_);

    imagePreview_ = new ImagePreviewPane(this);
    layout->addWidget(imagePreview_);

    rerender();
    renderClock_.start();
}

void MessageWidget::setRevertCallback(std::function<void()> cb) {
    if (role_ != Role::User) return;
    if (revertBtn_) return;  // already attached

    revertBtn_ = new QPushButton(QStringLiteral("\u21A9 Revert to here"), this);
    revertBtn_->setToolTip(QStringLiteral(
        "Restore the conversation and all modified files to the state "
        "they were in before this message was sent."));
    connect(revertBtn_, &QPushButton::clicked, this, [cb = std::move(cb)]() {
        if (cb) cb();
    });

    // Insert the button into the layout (after the body).
    auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
    if (layout) {
        layout->addWidget(revertBtn_, 0, Qt::AlignRight);
    }
}

void MessageWidget::setText(const QString& markdown) {
    buffer_ = markdown;
    scheduleRerender(true);
}

void MessageWidget::appendText(const QString& fragment) {
    buffer_ += fragment;
    scheduleRerender(false);
}

void MessageWidget::finalizeText() {
    scheduleRerender(true);
}

void MessageWidget::setWorkingDir(const QString& workingDir) {
    workingDir_ = workingDir;
    if (imagePreview_) imagePreview_->setWorkingDir(workingDir_);
    refreshImagePreview();
}

void MessageWidget::scheduleRerender(bool immediate) {
    if (immediate || !renderClock_.isValid() || renderClock_.elapsed() >= kStreamRenderIntervalMs) {
        renderPending_ = false;
        rerender();
        renderClock_.restart();
        return;
    }

    if (renderPending_) return;
    renderPending_ = true;
    const int delay = static_cast<int>(kStreamRenderIntervalMs - renderClock_.elapsed());
    QTimer::singleShot(delay, this, [this]() {
        if (!renderPending_) return;
        renderPending_ = false;
        rerender();
        renderClock_.restart();
    });
}

void MessageWidget::rerender() {
    if (inRerender_) return;
    inRerender_ = true;

    if (role_ == Role::User) {
        // User text is shown VERBATIM. Markdown rendering would reinterpret
        // what the user typed: list dashes disappear into bullets, blank
        // lines collapse, and anything in angle brackets ("<n>", "<m>",
        // "<dir>") is parsed as an HTML tag and swallowed along with the
        // text after it. The model receives the raw text either way; the
        // display must match it.
        body_->setPlainText(buffer_);
    } else {
        body_->setMarkdown(buffer_);
        colorizeDocument();
    }
    refreshImagePreview();

    // Resize to fit content height (no internal scrollbar; the parent ChatView scrolls).
    if (body_->viewport()->width() > 0) {
        body_->document()->setTextWidth(body_->viewport()->width());
    }
    int h = static_cast<int>(body_->document()->size().height()) + 4;
    if (h < 24) h = 24;
    body_->setFixedHeight(h);
    inRerender_ = false;
}

void MessageWidget::refreshImagePreview() {
    if (!imagePreview_) return;
    imagePreview_->setSourcesFromText(buffer_);
}

void MessageWidget::colorizeDocument() {
    QTextDocument* doc = body_->document();
    QTextCursor cur(doc);

    auto recolor = [&](int start, int length, const QString& hex) {
        if (length <= 0) return;
        cur.setPosition(start);
        cur.setPosition(start + length, QTextCursor::KeepAnchor);
        QTextCharFormat fmt;
        fmt.setForeground(QColor(hex));
        cur.mergeCharFormat(fmt);
    };

    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();

        // Fenced code block: tokenize and apply per-token syntax colors. Qt
        // stores one block per code line, so b.text() is a single line.
        if (bf.hasProperty(QTextFormat::BlockCodeLanguage)
            || bf.hasProperty(QTextFormat::BlockCodeFence)) {
            const codecolor::Language lang = codecolor::languageForName(
                bf.stringProperty(QTextFormat::BlockCodeLanguage));
            const int base = b.position();
            for (const codecolor::Span& s : codecolor::spansForLine(b.text(), lang))
                recolor(base + s.start, s.length, s.color);
            continue;
        }

        // Heading: accent the whole line (length()-1 drops the block separator).
        if (bf.headingLevel() > 0) {
            recolor(b.position(), b.length() - 1, kColHeading);
            continue;
        }

        // Normal paragraph / list item: colorize emphasized runs. Checked most-
        // specific first; each fragment gets at most one color.
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            const QTextCharFormat cf = frag.charFormat();

            QString hex;
            if (cf.fontFixedPitch())                       hex = kColInlineCode;  // `code`
            else if (!cf.anchorHref().isEmpty())           hex = kColLink;        // [text](url)
            else if (cf.fontWeight() >= QFont::DemiBold)    hex = kColBold;        // **bold**
            else if (cf.fontItalic())                      hex = kColItalic;      // *italic*

            if (!hex.isEmpty())
                recolor(frag.position(), frag.length(), hex);
        }
    }
}

void MessageWidget::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    if (!inRerender_) scheduleRerender(false);
}
