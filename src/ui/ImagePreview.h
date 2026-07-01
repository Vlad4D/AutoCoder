#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QFrame;
class QPushButton;
class QVBoxLayout;

namespace imagepreview {

// Returns existing image files referenced by text. Recognizes Markdown image/link
// targets, file:// URLs, absolute paths, and simple project-relative paths.
QStringList imagePathsInText(const QString& text, const QString& workingDir = {});

// Resolve and validate one candidate path. Returns an absolute local path, or
// an empty string when the candidate is not a readable image.
QString resolveImagePath(const QString& candidate, const QString& workingDir = {});

}  // namespace imagepreview

class ImagePreviewPane : public QWidget {
public:
    explicit ImagePreviewPane(QWidget* parent = nullptr);

    void setWorkingDir(QString workingDir);
    void setSourcesFromText(const QString& text);
    void setImagePaths(const QStringList& paths);
    bool hasImages() const { return !paths_.isEmpty(); }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Item {
        QString path;
        QFrame* frame = nullptr;
        QLabel* image = nullptr;
        QLabel* caption = nullptr;
        QPushButton* open = nullptr;
    };

    void rebuild();
    void updatePixmaps();
    QString displayPath(const QString& path) const;

    QVBoxLayout* layout_ = nullptr;
    QString workingDir_;
    QStringList paths_;
    QList<Item> items_;
};
