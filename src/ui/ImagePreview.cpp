#include "ImagePreview.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSet>
#include <QSize>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>
#include <algorithm>

namespace {

constexpr int kMaxImages = 8;
constexpr int kPreviewSourceWidth = 1400;
constexpr int kPreviewSourceHeight = 900;
constexpr int kPreviewMaxWidth = 720;
constexpr int kPreviewMaxHeight = 420;
constexpr int kPreviewFramePadding = 18;

class ClickableImageLabel : public QLabel {
public:
    explicit ClickableImageLabel(QString path, QWidget* parent = nullptr)
        : QLabel(parent), path_(std::move(path)) {
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && !path_.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path_));
            event->accept();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }

private:
    QString path_;
};

QString stripMarkdownTarget(QString s) {
    s = s.trimmed();
    if (s.startsWith(QLatin1Char('<'))) {
        const int end = s.indexOf(QLatin1Char('>'), 1);
        if (end > 0) s = s.mid(1, end - 1);
    } else if (s.startsWith(QLatin1Char('"')) || s.startsWith(QLatin1Char('\''))) {
        const QChar quote = s.front();
        const int end = s.indexOf(quote, 1);
        if (end > 0) s = s.mid(1, end - 1);
    } else {
        const int title = s.indexOf(QStringLiteral(" \""));
        if (title > 0) s = s.left(title);
    }
    return s.trimmed();
}

void appendUnique(QStringList& out, QSet<QString>& seen, const QString& path) {
    if (path.isEmpty() || seen.contains(path)) return;
    seen.insert(path);
    out << path;
}

QImage readPreviewImage(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (original.isValid()
        && (original.width() > kPreviewSourceWidth
            || original.height() > kPreviewSourceHeight)) {
        const QSize scaled = original.scaled(kPreviewSourceWidth, kPreviewSourceHeight,
                                            Qt::KeepAspectRatio);
        if (scaled.isValid()) reader.setScaledSize(scaled);
    }
    return reader.read();
}

QImage scaledDownToFit(const QImage& image, int maxWidth, int maxHeight) {
    if (image.isNull()) return image;
    if (image.width() <= maxWidth && image.height() <= maxHeight) return image;
    return image.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

}  // namespace

namespace imagepreview {

QString resolveImagePath(const QString& candidate, const QString& workingDir) {
    QString s = stripMarkdownTarget(candidate);
    if (s.isEmpty()) return {};

    QString path;
    const QUrl url(s);
    if (url.isValid() && url.isLocalFile()) {
        path = url.toLocalFile();
    } else {
        path = QUrl::fromPercentEncoding(s.toUtf8());
    }

    QFileInfo info(path);
    if (!info.isAbsolute()) {
        const QString baseDir = workingDir.isEmpty() ? QDir::currentPath() : workingDir;
        info.setFile(QDir(baseDir).absoluteFilePath(path));
    }

    if (!info.exists() || !info.isFile()) return {};

    const QString abs = QDir::cleanPath(info.absoluteFilePath());
    QImageReader reader(abs);
    if (!reader.canRead()) return {};
    return abs;
}

QStringList imagePathsInText(const QString& text, const QString& workingDir) {
    QStringList out;
    QSet<QString> seen;

    static const QRegularExpression markdownLinkRe(
        QStringLiteral(R"(!?\[[^\]\r\n]*\]\(([^)\r\n]+)\))"),
        QRegularExpression::CaseInsensitiveOption);
    auto mdIt = markdownLinkRe.globalMatch(text);
    while (mdIt.hasNext() && out.size() < kMaxImages) {
        const QRegularExpressionMatch m = mdIt.next();
        appendUnique(out, seen, resolveImagePath(m.captured(1), workingDir));
    }

    static const QRegularExpression htmlImgRe(
        QStringLiteral(R"(<img\b[^>]*\bsrc\s*=\s*["']([^"']+)["'][^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    auto htmlIt = htmlImgRe.globalMatch(text);
    while (htmlIt.hasNext() && out.size() < kMaxImages) {
        const QRegularExpressionMatch m = htmlIt.next();
        appendUnique(out, seen, resolveImagePath(m.captured(1), workingDir));
    }

    static const QRegularExpression rawPathRe(
        QStringLiteral(R"((?:^|[\s("'`<])((?:[A-Za-z]:[\\/][^\r\n<>"|?*]+?|\\\\[^\r\n<>"|?*]+?|(?:\.{1,2}[\\/])?[^\s\r\n<>"'`()\[\]{}]+?)\.(?:png|jpe?g|gif|bmp|webp|svg|ico|tiff?))(?:$|[\s).,;:'"`>]))"),
        QRegularExpression::CaseInsensitiveOption);
    auto rawIt = rawPathRe.globalMatch(text);
    while (rawIt.hasNext() && out.size() < kMaxImages) {
        const QRegularExpressionMatch m = rawIt.next();
        appendUnique(out, seen, resolveImagePath(m.captured(1), workingDir));
    }

    return out;
}

}  // namespace imagepreview

ImagePreviewPane::ImagePreviewPane(QWidget* parent) : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 6, 0, 0);
    layout_->setSpacing(8);
    setVisible(false);
}

void ImagePreviewPane::setWorkingDir(QString workingDir) {
    workingDir_ = QDir::cleanPath(std::move(workingDir));
}

void ImagePreviewPane::setSourcesFromText(const QString& text) {
    setImagePaths(imagepreview::imagePathsInText(text, workingDir_));
}

void ImagePreviewPane::setImagePaths(const QStringList& paths) {
    QStringList resolved;
    QSet<QString> seen;
    for (const QString& p : paths) {
        if (resolved.size() >= kMaxImages) break;
        QString path = imagepreview::resolveImagePath(p, workingDir_);
        if (path.isEmpty()) path = imagepreview::resolveImagePath(QDir::cleanPath(p), workingDir_);
        appendUnique(resolved, seen, path);
    }

    if (resolved == paths_) {
        setVisible(!paths_.isEmpty());
        updatePixmaps();
        return;
    }

    paths_ = resolved;
    rebuild();
}

void ImagePreviewPane::rebuild() {
    while (QLayoutItem* item = layout_->takeAt(0)) {
        if (QWidget* w = item->widget()) delete w;
        delete item;
    }
    items_.clear();

    for (const QString& path : paths_) {
        auto* frame = new QFrame(this);
        frame->setObjectName("imagePreviewFrame");
        frame->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        frame->setStyleSheet(QStringLiteral(
            "QFrame#imagePreviewFrame { background-color: #111827;"
            " border: 1px solid #374151; border-radius: 6px; }"
            "QLabel { color: #D1D5DB; }"
            "QPushButton { background-color: #374151; color: #E5E7EB;"
            " border: 1px solid #4B5563; border-radius: 4px; padding: 3px 8px; }"
            "QPushButton:hover { background-color: #4B5563; }"));

        auto* col = new QVBoxLayout(frame);
        col->setContentsMargins(8, 8, 8, 8);
        col->setSpacing(6);

        auto* image = new ClickableImageLabel(path, frame);
        image->setAlignment(Qt::AlignCenter);
        image->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        image->setToolTip(QStringLiteral("Open image: %1").arg(path));
        col->addWidget(image);

        auto* row = new QHBoxLayout;
        auto* caption = new QLabel(displayPath(path), frame);
        caption->setTextInteractionFlags(Qt::TextSelectableByMouse);
        caption->setToolTip(path);
        caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        row->addWidget(caption, 1);

        auto* open = new QPushButton(QStringLiteral("Open"), frame);
        open->setCursor(Qt::PointingHandCursor);
        connect(open, &QPushButton::clicked, frame, [path]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        row->addWidget(open, 0);
        col->addLayout(row);

        layout_->addWidget(frame, 0, Qt::AlignLeft);
        items_.append({path, frame, image, caption, open});
    }

    setVisible(!paths_.isEmpty());
    updatePixmaps();
}

void ImagePreviewPane::updatePixmaps() {
    if (items_.isEmpty()) return;

    const int maxWidth = qBound(1, width() - 20, kPreviewMaxWidth);
    for (const Item& item : items_) {
        QImage img = readPreviewImage(item.path);
        if (img.isNull()) {
            item.image->setText(QStringLiteral("Could not load image preview"));
            continue;
        }

        const QPixmap pix = QPixmap::fromImage(
            scaledDownToFit(img, maxWidth, kPreviewMaxHeight));
        item.image->setPixmap(pix);
        item.image->setFixedSize(pix.size());

        const int buttonWidth = item.open ? item.open->sizeHint().width() : 0;
        const int contentWidth = std::max(pix.width(), buttonWidth);
        const int frameWidth = contentWidth + kPreviewFramePadding;
        if (item.caption) {
            const QString text = displayPath(item.path);
            item.caption->setMaximumWidth(std::max(1, contentWidth - buttonWidth - 8));
            item.caption->setText(item.caption->fontMetrics().elidedText(
                text, Qt::ElideMiddle, item.caption->maximumWidth()));
        }
        if (item.frame) item.frame->setFixedWidth(frameWidth);
    }
}

QString ImagePreviewPane::displayPath(const QString& path) const {
    if (!workingDir_.isEmpty()) {
        const QString rel = QDir(workingDir_).relativeFilePath(path);
        if (!rel.startsWith(QLatin1String("..")))
            return QDir::cleanPath(rel);
    }
    return QDir::toNativeSeparators(path);
}

void ImagePreviewPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updatePixmaps();
}
