#include "ToolCallWidget.h"

#include "CodeColorizer.h"
#include "ImagePreview.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QPushButton>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStringList>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QVector>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

namespace {

constexpr int kMaxBodyBytes = 96 * 1024;
constexpr int kMaxBodyHeight = 420;
constexpr int kMaxToolHeaderRefChars = 80;

int logicalLineCount(const QString& text) {
    if (text.isEmpty()) return 0;
    return text.count('\n') + (text.endsWith('\n') ? 0 : 1);
}

QString formatLineDelta(int added, int removed) {
    return QStringLiteral("+%1 -%2").arg(added).arg(removed);
}

QString formatColoredLineDelta(const QString& delta) {
    const int split = delta.indexOf(' ');
    if (split < 0) return delta.toHtmlEscaped();

    const QString added = delta.left(split).toHtmlEscaped();
    const QString removed = delta.mid(split + 1).toHtmlEscaped();
    return QStringLiteral("<span style=\"color:#34D399;\">%1</span> "
                          "<span style=\"color:#F87171;\">%2</span>")
        .arg(added, removed);
}

int replacementCountFromOutput(const QString& output) {
    static const QRegularExpression re(QStringLiteral("applied (\\d+) replacement"));
    const QRegularExpressionMatch match = re.match(output);
    return match.hasMatch() ? match.captured(1).toInt() : 1;
}

QString limitBodyText(const QString& text) {
    QByteArray utf = text.toUtf8();
    if (utf.size() <= kMaxBodyBytes) return text;

    QByteArray head = utf.left(kMaxBodyBytes);
    return QString::fromUtf8(head)
         + QStringLiteral("\n[... %1 bytes hidden from preview]").arg(utf.size() - kMaxBodyBytes);
}

QString limitHeaderRef(const QString& text) {
    if (text.length() <= kMaxToolHeaderRefChars)
        return text;
    return text.left(kMaxToolHeaderRefChars - 3) + QStringLiteral("...");
}

QString limitHeaderPathTail(const QString& path) {
    if (path.length() <= kMaxToolHeaderRefChars)
        return path;

    const QString marker = QStringLiteral("...");
    const int tailChars = kMaxToolHeaderRefChars - marker.length();
    return marker + path.right(tailChars);
}

QString expandTabsForDisplay(QString text) {
    text.replace(QLatin1Char('\t'), QStringLiteral("  "));

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString& line : lines) {
        int quote = -1;
        for (int i = 0; i < line.size(); ++i) {
            if (line[i] != QLatin1Char('\'') && line[i] != QLatin1Char('"')) continue;
            if (line.left(i).contains(QLatin1Char(':'))) {
                quote = i;
                break;
            }
        }
        if (quote < 0) continue;

        int pos = quote + 1;
        while (line.mid(pos, 2) == QStringLiteral("\\t")) {
            line.replace(pos, 2, QStringLiteral("  "));
            pos += 2;
        }
    }
    text = lines.join(QLatin1Char('\n'));
    return text;
}

QString unescapeLineDumpText(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (ch != QLatin1Char('\\') || i + 1 >= text.size()) {
            out += ch;
            continue;
        }

        const QChar next = text[++i];
        if (next == QLatin1Char('n') || next == QLatin1Char('r')) {
            continue;
        }
        if (next == QLatin1Char('t')) {
            out += QStringLiteral("  ");
            continue;
        }
        if (next == QLatin1Char('\\') || next == QLatin1Char('\'') || next == QLatin1Char('"')) {
            out += next;
            continue;
        }

        out += QLatin1Char('\\');
        out += next;
    }
    return out;
}

QString prettifyEscapedLineDumps(const QString& text) {
    static const QRegularExpression singleQuotedRe(
        QStringLiteral("^(\\s*\\d+:\\s*)'((?:\\\\.|[^'\\\\])*)'(\\s*)$"));
    static const QRegularExpression doubleQuotedRe(
        QStringLiteral("^(\\s*\\d+:\\s*)\"((?:\\\\.|[^\"\\\\])*)\"(\\s*)$"));

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString& line : lines) {
        QRegularExpressionMatch match = singleQuotedRe.match(line);
        if (!match.hasMatch()) {
            match = doubleQuotedRe.match(line);
        }
        if (!match.hasMatch()) continue;

        line = match.captured(1)
             + unescapeLineDumpText(match.captured(2))
             + match.captured(3);
    }
    return lines.join(QLatin1Char('\n'));
}

// Shorten a file path for display. Paths under the working dir are shown
// relative; external absolute paths keep as much tail context as the header
// budget allows.
static QString shortenPath(const QString& path, const QString& workingDir) {
    QFileInfo fi(path);
    if (!fi.isAbsolute() || fi.fileName().isEmpty())
        return path;  // already relative or empty — show as-is

    // If a working directory is set and the path is under it, show relative.
    if (!workingDir.isEmpty()) {
        QString rel = QDir(workingDir).relativeFilePath(path);
        // Only use the relative form if it's shorter than the fallback
        // and actually under the working dir (doesn't start with "../").
        if (!rel.startsWith(QLatin1String(".."))) {
            return rel;
        }
    }

    return limitHeaderPathTail(QDir::cleanPath(path));
}

QString quoteCdPathForDisplay(const QString& path) {
    if (path.isEmpty() || path == QStringLiteral("."))
        return path;

    QString quoted = path;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(quoted);
}

QString shortenCdPath(const QString& path, const QString& workingDir) {
    if (workingDir.isEmpty())
        return path;

    const QString cleanPath = QDir::cleanPath(path.trimmed());
    QFileInfo fi(cleanPath);
    if (!fi.isAbsolute())
        return cleanPath;

    const QString rel = QDir(workingDir).relativeFilePath(cleanPath);
    if (rel.startsWith(QLatin1String("..")))
        return cleanPath;

    return QDir::cleanPath(rel);
}

QString unwrapWindowsCmdForHeader(QString cmd) {
    static const QRegularExpression quotedCmdRe(
        QStringLiteral("^\\s*(?:cmd(?:\\.exe)?|%COMSPEC%)\\s+/[ck]\\s+\"(.*)\"\\s*$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression bareCmdRe(
        QStringLiteral("^\\s*(?:cmd(?:\\.exe)?|%COMSPEC%)\\s+/[ck]\\s+(.*)$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    QRegularExpressionMatch match = quotedCmdRe.match(cmd);
    if (match.hasMatch())
        return match.captured(1).trimmed();

    match = bareCmdRe.match(cmd);
    if (match.hasMatch())
        return match.captured(1).trimmed();

    return cmd;
}

QString shortenBashCommandForHeader(QString cmd, const QString& workingDir) {
    cmd = unwrapWindowsCmdForHeader(cmd);

    static const QRegularExpression leadingCdRe(
        QStringLiteral("^\\s*cd(?:\\s+/d)?\\s+(?:\"([^\"]+)\"|'([^']+)'|([^&]+?))\\s*&&\\s*(.*)$"),
        QRegularExpression::CaseInsensitiveOption |
        QRegularExpression::DotMatchesEverythingOption);

    const QRegularExpressionMatch match = leadingCdRe.match(cmd);
    if (match.hasMatch()) {
        const QString cdPath = !match.captured(1).isEmpty()
            ? match.captured(1)
            : (!match.captured(2).isEmpty() ? match.captured(2) : match.captured(3));
        const QString rest = match.captured(4).trimmed();
        const QString shortenedPath = shortenCdPath(cdPath, workingDir);

        if (shortenedPath == QStringLiteral("."))
            cmd = rest;
        else
            cmd = QStringLiteral("cd %1 && %2").arg(quoteCdPathForDisplay(shortenedPath), rest);
    }

    return cmd;
}

// Extract a human-readable path/pattern summary from tool args.
QString extractFileRef(const QString& name, const QString& argsJson, const QString& workingDir) {
    try {
        auto args = nlohmann::json::parse(argsJson.toStdString());
        // Tools with a "path" parameter
        if (args.contains("path")) {
            return limitHeaderRef(
                shortenPath(QString::fromStdString(args["path"].get<std::string>()), workingDir));
        }
        // Glob uses "pattern"
        if (args.contains("pattern")) {
            return limitHeaderRef(QStringLiteral("pattern=%1")
                .arg(QString::fromStdString(args["pattern"].get<std::string>())));
        }
        // Bash uses "command" – show a short prefix
        if (name == "bash" && args.contains("command")) {
            QString cmd = QString::fromStdString(args["command"].get<std::string>());
            return limitHeaderRef(shortenBashCommandForHeader(cmd, workingDir));
        }
    } catch (...) {
        // JSON parse failure – skip
    }
    return {};
}

// Format a duration in milliseconds to a short human string.
QString formatDuration(qint64 ms) {
    // Skip sub-second durations to avoid noise from fast tools.
    if (ms < 1000) return {};
    if (ms < 10000)
        return QStringLiteral("%1.%2s").arg(ms / 1000).arg((ms % 1000) / 100, 1, 10, QLatin1Char('0'));
    return QStringLiteral("%1s").arg(ms / 1000);
}

bool isFileModifyingTool(const QString& name) {
    return name == QStringLiteral("bash")
        || name == QStringLiteral("write")
        || name == QStringLiteral("append")
        || name == QStringLiteral("edit")
        || name == QStringLiteral("replace_lines")
        || name == QStringLiteral("move_span")
        || name == QStringLiteral("move_func");
}

struct DiffLine {
    enum Kind { Context, Removed, Added, Skip };

    Kind kind = Context;
    QString text;
};

QString normaliseSnippetLineEndings(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

QStringList splitSnippetLines(QString text) {
    text = normaliseSnippetLineEndings(std::move(text));
    if (text.isEmpty()) return {};

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (!lines.isEmpty() && lines.last().isEmpty() && text.endsWith(QLatin1Char('\n'))) {
        lines.removeLast();
    }
    return lines;
}

QVector<DiffLine> buildLineDiff(const QString& oldText, const QString& newText) {
    const QStringList oldLines = splitSnippetLines(oldText);
    const QStringList newLines = splitSnippetLines(newText);
    const int oldCount = oldLines.size();
    const int newCount = newLines.size();

    QVector<DiffLine> diff;
    diff.reserve(oldCount + newCount);

    constexpr int kMaxLcsCells = 250000;
    if (oldCount * newCount > kMaxLcsCells) {
        for (const QString& line : oldLines)
            diff.push_back({DiffLine::Removed, line});
        for (const QString& line : newLines)
            diff.push_back({DiffLine::Added, line});
        return diff;
    }

    QVector<int> dp((oldCount + 1) * (newCount + 1));
    auto cell = [&](int i, int j) -> int& {
        return dp[i * (newCount + 1) + j];
    };

    for (int i = oldCount - 1; i >= 0; --i) {
        for (int j = newCount - 1; j >= 0; --j) {
            if (oldLines[i] == newLines[j]) {
                cell(i, j) = cell(i + 1, j + 1) + 1;
            } else {
                cell(i, j) = std::max(cell(i + 1, j), cell(i, j + 1));
            }
        }
    }

    int i = 0;
    int j = 0;
    while (i < oldCount && j < newCount) {
        if (oldLines[i] == newLines[j]) {
            diff.push_back({DiffLine::Context, oldLines[i]});
            ++i;
            ++j;
        } else if (cell(i + 1, j) >= cell(i, j + 1)) {
            diff.push_back({DiffLine::Removed, oldLines[i]});
            ++i;
        } else {
            diff.push_back({DiffLine::Added, newLines[j]});
            ++j;
        }
    }

    while (i < oldCount) {
        diff.push_back({DiffLine::Removed, oldLines[i]});
        ++i;
    }
    while (j < newCount) {
        diff.push_back({DiffLine::Added, newLines[j]});
        ++j;
    }

    return diff;
}

QVector<DiffLine> compactDiffHunks(const QVector<DiffLine>& diff) {
    QVector<DiffLine> compact;
    constexpr int kContextLines = 3;
    constexpr int kMaxRenderedLines = 260;

    bool emittedAny = false;
    int i = 0;
    while (i < diff.size()) {
        while (i < diff.size() && diff[i].kind == DiffLine::Context)
            ++i;
        if (i >= diff.size()) break;

        int start = std::max(0, i - kContextLines);
        int end = i;
        while (end < diff.size()) {
            if (diff[end].kind != DiffLine::Context) {
                ++end;
                continue;
            }

            int contextEnd = end;
            while (contextEnd < diff.size() && diff[contextEnd].kind == DiffLine::Context)
                ++contextEnd;
            if (contextEnd - end > kContextLines) {
                end += kContextLines;
                break;
            }
            end = contextEnd;
        }

        if (emittedAny && !compact.isEmpty() && compact.last().kind != DiffLine::Skip)
            compact.push_back({DiffLine::Skip, QString()});
        for (int k = start; k < end && compact.size() < kMaxRenderedLines; ++k)
            compact.push_back(diff[k]);
        emittedAny = true;
        i = end;

        if (compact.size() >= kMaxRenderedLines) {
            compact.push_back({DiffLine::Skip, QStringLiteral("diff preview truncated")});
            break;
        }
    }

    if (compact.isEmpty() && !diff.isEmpty()) {
        compact.push_back({DiffLine::Context, QStringLiteral("no textual changes")});
    }

    return compact;
}

QString diffLineHtml(const DiffLine& line, codecolor::Language lang = codecolor::Language::Generic) {
    QString prefix;
    QString style;

    switch (line.kind) {
    case DiffLine::Removed:
        prefix = QStringLiteral("-");
        style = QStringLiteral("background-color:#2A1416;color:#E5E7EB;");
        break;
    case DiffLine::Added:
        prefix = QStringLiteral("+");
        style = QStringLiteral("background-color:#142A1B;color:#E5E7EB;");
        break;
    case DiffLine::Skip:
        prefix = QStringLiteral(" ");
        style = QStringLiteral("color:#9CA3AF;");
        break;
    case DiffLine::Context:
    default:
        prefix = QStringLiteral(" ");
        style = QStringLiteral("color:#D1D5DB;");
        break;
    }

    // Skip markers are not code -- render them plainly. Real diff lines get
    // per-token syntax highlighting (which also handles HTML escaping). Build by
    // concatenation rather than QString::arg() so a literal '%' in the code can't
    // collide with arg placeholders.
    QString contentHtml;
    if (line.kind == DiffLine::Skip) {
        const QString text = QStringLiteral("...")
            + (line.text.isEmpty() ? QString() : QStringLiteral(" ") + line.text);
        contentHtml = text.toHtmlEscaped();
    } else {
        contentHtml = codecolor::highlightLine(line.text, lang);
    }

    return QStringLiteral("<div style=\"white-space:pre;") + style + QStringLiteral("\">")
        + prefix.toHtmlEscaped() + QStringLiteral(" ") + contentHtml
        + QStringLiteral("</div>");
}

QString editDiffHtml(const QString& oldText, const QString& newText,
                     codecolor::Language lang = codecolor::Language::Generic) {
    const QVector<DiffLine> diff = compactDiffHunks(buildLineDiff(oldText, newText));

    QString html;
    html += QStringLiteral("<div style=\"font-family:Consolas,monospace;"
                           "font-size:inherit;background-color:#0B1220;"
                           "border:1px solid #263244;\">");
    for (const DiffLine& line : diff)
        html += diffLineHtml(line, lang);
    html += QStringLiteral("</div>");
    return html;
}

// Pick the highlighter language from a tool call's "path" argument (edit/write/
// replace_lines all carry one). Unknown/absent path -> Generic.
codecolor::Language languageFromArgsJson(const QString& argsJson) {
    try {
        auto args = nlohmann::json::parse(argsJson.toStdString());
        if (args.contains("path"))
            return codecolor::languageForPath(
                QString::fromStdString(args["path"].get<std::string>()));
    } catch (...) {
    }
    return codecolor::Language::Generic;
}

// The monospace code-block container shared by the read/grep/outline bodies
// (same look as the diff blocks).
QString codeBlockOpen() {
    return QStringLiteral("<div style=\"font-family:Consolas,monospace;font-size:inherit;"
                          "background-color:#0B1220;border:1px solid #263244;\">");
}

// One physical line inside a code block (preserves leading whitespace).
QString codeLineDiv(const QString& innerHtml) {
    return QStringLiteral("<div style=\"white-space:pre;\">") + innerHtml
         + QStringLiteral("</div>");
}

// Escaped text wrapped in a dim color (for gutters, paths, outline metadata,
// and status lines like "[no matches]").
QString dimSpan(const QString& text, const char* color = "#9CA3AF") {
    return QStringLiteral("<span style=\"color:%1;\">%2</span>")
        .arg(QLatin1String(color), text.toHtmlEscaped());
}

// Parse the "=== OLD CONTENT === / === NEW CONTENT === / === RESULT ===" payload
// emitted by replace_lines and (on overwrite) write. Returns false if absent.
bool parseOldNewMarkers(const QString& text, QString& oldOut, QString& newOut) {
    static const QRegularExpression oldRe(QStringLiteral("^=== OLD CONTENT ===\r?\n"));
    static const QRegularExpression newRe(QStringLiteral("\n=== NEW CONTENT ===\r?\n"));
    static const QRegularExpression resultRe(QStringLiteral("\n=== RESULT ===\r?\n"));

    const int oldMatch = oldRe.match(text).capturedStart();
    const int newMatch = newRe.match(text).capturedStart();
    const int resultMatch = resultRe.match(text).capturedStart();
    if (!(oldMatch >= 0 && newMatch > oldMatch && resultMatch > newMatch)) return false;

    const int oldStart = oldMatch + text.mid(oldMatch).indexOf('\n') + 1;
    oldOut = text.mid(oldStart, newMatch - oldStart);
    // newMatch points to '\n' before "=== NEW CONTENT ==="; skip that
    // line to reach the actual new content.
    const int newLineStart = newMatch + 1; // skip the leading '\n'
    const int newStart = newLineStart + text.mid(newLineStart).indexOf('\n') + 1;
    newOut = text.mid(newStart, resultMatch - newStart);
    return true;
}

QString metaHtml(const QString& label, const QString& value) {
    return QStringLiteral("<div style=\"color:#9CA3AF;white-space:pre-wrap;\">"
                          "<span style=\"color:#E5E7EB;\">%1:</span> %2</div>")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
}

QStringList imagePathArgs(const QString& argsJson, const QString& workingDir) {
    QStringList paths;
    try {
        const auto args = nlohmann::json::parse(argsJson.toStdString());
        static const char* kPathKeys[] = {
            "path", "source_path", "target_path", "src", "dst", "destination"
        };
        for (const char* key : kPathKeys) {
            if (!args.contains(key) || !args[key].is_string()) continue;
            const QString path = imagepreview::resolveImagePath(
                QString::fromStdString(args[key].get<std::string>()), workingDir);
            if (!path.isEmpty() && !paths.contains(path)) paths << path;
        }
    } catch (...) {
    }
    return paths;
}

}  // namespace

ToolCallWidget::ToolCallWidget(const QString& name, const QString& argsJson,
                               const QString& workingDir, QWidget* parent)
    : QFrame(parent), name_(name), argsJson_(argsJson), workingDir_(workingDir) {
    setFrameShape(QFrame::StyledPanel);
    setObjectName("toolBubble");
    refreshStyle();

    expandBtn_ = new QToolButton(this);
    expandBtn_->setText("▶");
    expandBtn_->setAutoRaise(true);
    connect(expandBtn_, &QToolButton::clicked, this, [this]() { setExpanded(!expanded_); });

    header_ = new QLabel(this);
    header_->setTextFormat(Qt::RichText);
    header_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->addWidget(expandBtn_);
    head->addWidget(header_, 1);

    body_ = new QTextBrowser(this);
    body_->setOpenExternalLinks(true);
    body_->setFrameShape(QFrame::NoFrame);
    body_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    body_->setLineWrapMode(QTextEdit::NoWrap);
    body_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    body_->setFont(QFont("Consolas", QApplication::font().pointSize()));
    body_->document()->setDocumentMargin(4);
    body_->setVisible(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->addLayout(head);
    layout->addWidget(body_);

    imagePreview_ = new ImagePreviewPane(this);
    imagePreview_->setWorkingDir(workingDir_);
    imagePreview_->setVisible(false);
    layout->addWidget(imagePreview_);

    // Start the elapsed timer and extract the file reference early.
    timer_.start();
    fileRef_ = extractFileRef(name_, argsJson_, workingDir_);

    refreshHeader();

    // Auto-expand for file-modifying tools so the user sees diffs immediately.
    if (isFileModifyingTool(name_)) {
        setExpanded(true);
    }
}

void ToolCallWidget::appendOutput(const QString& chunk) {
    if (chunk.isEmpty()) return;
    output_ += chunk;

    // Auto-expand on first output so the user sees streaming progress.
    if (!expanded_) {
        setExpanded(true);
    } else {
        // Re-render the full body on each chunk (QTextBrowser doesn't support
        // efficient append, but chunks are infrequent enough that this is fine).
        refreshBody();
    }
    scrollBodyToBottom();
    refreshHeader();
}

void ToolCallWidget::setResult(const QString& result, bool ok) {
    finished_ = true;
    ok_ = ok;
    if (output_.isEmpty()) {
        output_ = result;
    }
    refreshStyle();
    refreshHeader();
    if (expanded_) {
        refreshBody();
        scrollBodyToBottom();
    }
}

void ToolCallWidget::setExpanded(bool expanded) {
    expanded_ = expanded;
    expandBtn_->setText(expanded ? "▼" : "▶");
    body_->setVisible(expanded);
    if (!expanded && imagePreview_) imagePreview_->setVisible(false);
    if (expanded) {
        refreshBody();
        scrollBodyToBottom();
    }
}

void ToolCallWidget::setApprovalRequired(const QString& reason, const QString& explanation) {
    approvalActive_ = true;
    auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
    if (!layout) return;

    // Auto-expand so the user sees the command needing approval.
    setExpanded(true);

    approvalRow_ = new QWidget(this);
    approvalRow_->setObjectName("approvalRow");
    approvalRow_->setStyleSheet(
        "QWidget#approvalRow { border-top: 1px solid #D97706; }");
    auto* col = new QVBoxLayout(approvalRow_);
    col->setContentsMargins(0, 6, 0, 0);
    col->setSpacing(6);

    // Title.
    auto* title = new QLabel(QStringLiteral("⚠ AutoCoder wants to run this command"), approvalRow_);
    title->setStyleSheet(QStringLiteral("font-weight: bold; color: #FBBF24;"));
    col->addWidget(title);

    // Explanation – the LLM's stated purpose for the command.
    if (!explanation.isEmpty()) {
        auto* why = new QLabel(explanation, approvalRow_);
        why->setTextFormat(Qt::PlainText);
        why->setWordWrap(true);
        why->setStyleSheet(QStringLiteral("color: #E5E7EB;"));
        col->addWidget(why);
    }

    // Reason why it needs approval (policy).
    if (!reason.isEmpty()) {
        auto* why = new QLabel(QStringLiteral("Needs your OK because it %1.").arg(reason), approvalRow_);
        why->setWordWrap(true);
        why->setStyleSheet(QStringLiteral("color: #D1D5DB;"));
        col->addWidget(why);
    }

    // Button row.
    auto* row = new QHBoxLayout;
    row->setSpacing(6);

    auto* allowBtn = new QPushButton(QStringLiteral("Allow once"), approvalRow_);
    allowBtn->setCursor(Qt::PointingHandCursor);
    allowBtn->setStyleSheet(QStringLiteral("background-color: #059669; color: white;"
        "border: none; border-radius: 6px; padding: 6px 14px;"));
    connect(allowBtn, &QPushButton::clicked, this,
            [this, allowBtn]() { finishApproval(QStringLiteral("allow_once"), false, allowBtn); });

    auto* alwaysBtn = new QPushButton(QStringLiteral("Always allow this command"), approvalRow_);
    alwaysBtn->setCursor(Qt::PointingHandCursor);
    alwaysBtn->setStyleSheet(QStringLiteral("background-color: #2563EB; color: white;"
        "border: none; border-radius: 6px; padding: 6px 14px;"));
    connect(alwaysBtn, &QPushButton::clicked, this,
            [this, alwaysBtn]() { finishApproval(QStringLiteral("allow_once"), true, alwaysBtn); });

    auto* denyBtn = new QPushButton(QStringLiteral("Deny"), approvalRow_);
    denyBtn->setCursor(Qt::PointingHandCursor);
    denyBtn->setStyleSheet(QStringLiteral("background-color: #B91C1C; color: white;"
        "border: none; border-radius: 6px; padding: 6px 14px;"));
    connect(denyBtn, &QPushButton::clicked, this,
            [this, denyBtn]() { finishApproval(QStringLiteral("deny"), false, denyBtn); });

    row->addWidget(allowBtn);
    row->addWidget(alwaysBtn);
    row->addWidget(denyBtn);
    row->addStretch(1);
    col->addLayout(row);

    layout->addWidget(approvalRow_);
}

void ToolCallWidget::setApprovalResolved(const QString& decision) {
    approvalActive_ = false;
    auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
    if (!layout) return;

    approvalRow_ = new QWidget(this);
    approvalRow_->setObjectName("approvalRow");
    auto* col = new QVBoxLayout(approvalRow_);
    col->setContentsMargins(0, 4, 0, 0);

    QString text;
    QString color;
    if (decision == QStringLiteral("deny")) {
        text = QStringLiteral("✗ Denied");
        color = QStringLiteral("#F87171");
    } else if (decision == QStringLiteral("always")) {
        text = QStringLiteral("✓ Always allowed");
        color = QStringLiteral("#34D399");
    } else {
        text = QStringLiteral("✓ Allowed once");
        color = QStringLiteral("#34D399");
    }
    auto* status = new QLabel(text, approvalRow_);
    status->setStyleSheet(QStringLiteral("font-weight: bold; color: %1;").arg(color));
    col->addWidget(status);

    layout->addWidget(approvalRow_);
}

void ToolCallWidget::finishApproval(const QString& decision, bool persist, QPushButton* chosen) {
    // Disable all buttons in the approval row and dim the ones not chosen.
    if (approvalRow_) {
        for (auto* btn : approvalRow_->findChildren<QPushButton*>()) {
            btn->setEnabled(false);
            if (btn != chosen) {
                btn->setStyleSheet(QStringLiteral(
                    "background-color: #374151; color: #9CA3AF; "
                    "border: none; border-radius: 6px; padding: 6px 14px;"));
            }
        }
    }
    approvalActive_ = false;
    emit decided(decision, persist);
}

void ToolCallWidget::refreshStyle() {
    QString bubbleStyle;
    if (finished_ && !ok_) {
        bubbleStyle = QStringLiteral("background-color: #3A1518; border: 1px solid #7F1D1D;");
    } else if (isFileModifyingTool(name_)) {
        bubbleStyle = QStringLiteral("background-color: #3A2F10; border: 1px solid #7A5B14;");
    } else {
        bubbleStyle = QStringLiteral("background-color: #1F2933; border: none;");
    }
    setStyleSheet(
        QStringLiteral(
            "QFrame#toolBubble { %1 color: #E5E7EB; border-radius: 6px; }"
            "QLabel { color: #E5E7EB; }"
            "QPlainTextEdit { background-color: #111827; color: #D1D5DB; border: none; }"
            "QTextBrowser { background-color: #111827; color: #D1D5DB; border: none; }"
            "QToolButton { color: #E5E7EB; border: none; }")
            .arg(bubbleStyle));
}

void ToolCallWidget::refreshHeader() {
    int approxLines = output_.isEmpty() ? 0 : output_.count('\n') + 1;
    QString status = finished_ ? (ok_ ? "ok" : "error") : "running…";

    // Duration
    QString dur = formatDuration(timer_.elapsed());

    // Line/count detail
    QString delta = lineDeltaSummary();
    QString detail = delta.isEmpty()
        ? QStringLiteral("%1 lines").arg(approxLines)
        : formatColoredLineDelta(delta);

    // Build the header string.
    // Format: toolName [fileRef]  · status · duration · detail
    QString text;
    if (name_ == "compact") {
        // Compact summary entry: show a folder icon and the duration.
        // The "freed" size is embedded in the body text instead (set via
        // setResult), so the header avoids cluttering with a meaningless
        // line count or missing bytesSaved value.
        if (dur.isEmpty()) {
            text = QStringLiteral("📦 Compacted");
        } else {
            text = QStringLiteral("📦 Compacted · %1").arg(dur);
        }
    } else {
        text = QStringLiteral("%1").arg(name_.toHtmlEscaped());
        if (!fileRef_.isEmpty()) {
            text += QStringLiteral(" [%1]").arg(fileRef_.toHtmlEscaped());
        }
        text += QStringLiteral("  · %1").arg(status.toHtmlEscaped());
        if (!dur.isEmpty()) {
            text += QStringLiteral(" · %1").arg(dur.toHtmlEscaped());
        }
        // For write/append, surface the byte count in the header instead of in
        // the body. Pair it with the line delta when one is available.
        if (name_ == "write" || name_ == "append") {
            const QString bytes = byteSummary();
            if (!bytes.isEmpty()) {
                text += QStringLiteral(" · %1").arg(bytes.toHtmlEscaped());
            }
            if (!delta.isEmpty()) {
                text += QStringLiteral(" · %1").arg(formatColoredLineDelta(delta));
            }
        } else {
            text += QStringLiteral(" · %1").arg(detail);
        }
    }

    header_->setText(text);
}

void ToolCallWidget::refreshBody() {
    // Compact entries use word wrap (summary text reads better);
    // all others use NoWrap (diffs, command output, JSON args).
    if (name_ == "compact") {
        body_->setLineWrapMode(QTextEdit::WidgetWidth);
        body_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    } else {
        body_->setLineWrapMode(QTextEdit::NoWrap);
    }

    QString body;
    bool bodyIsHtml = false;
    if (name_ == "bash") {
        body = formatBashBody();
    } else if (name_ == "edit") {
        body = formatEditBody();
        bodyIsHtml = true;
    } else if (name_ == "replace_lines") {
        body = formatReplaceLinesBody();
        bodyIsHtml = true;
    } else if (name_ == "write" || name_ == "append") {
        body = formatWriteBody();
        bodyIsHtml = true;
    } else if (name_ == "read") {
        body = formatReadBody();
        bodyIsHtml = true;
    } else if (name_ == "grep") {
        body = formatGrepBody();
        bodyIsHtml = true;
    } else if (name_ == "read_outline") {
        body = formatReadOutlineBody();
        bodyIsHtml = true;
    } else {
        // Generic tools (glob/ls/find_callers/move_*, ...):
        // the path/pattern/status are already in the header, so the body is just
        // the tool's result -- never the internal args JSON.
        body = output_.isEmpty() && !finished_ ? QStringLiteral("[running…]") : output_;
    }
    if (bodyIsHtml) {
        body_->setHtml(body);
    } else {
        body_->setPlainText(limitBodyText(expandTabsForDisplay(body)));
    }
    // Auto-size by plain-text blocks. QTextDocument geometry can be inflated
    // before the viewport width settles, which leaves large empty tool bodies.
    const int lineHeight = body_->fontMetrics().lineSpacing();
    int contentHeight;
    if (body_->lineWrapMode() == QTextEdit::WidgetWidth) {
        // For word-wrap mode, use document->size().height() which reflects the
        // actual wrapped line count. The document's text width must be set to
        // the body's available width first.
        int availWidth = body_->viewport()->width();
        if (availWidth < 100) availWidth = 600; // fallback during construction
        body_->document()->setTextWidth(availWidth);
        contentHeight = static_cast<int>(body_->document()->size().height())
                      + static_cast<int>(body_->document()->documentMargin() * 2)
                      + 6;
    } else {
        contentHeight = body_->document()->blockCount() * lineHeight
                      + static_cast<int>(body_->document()->documentMargin() * 2)
                      + 6;
    }
    if (contentHeight > kMaxBodyHeight) {
        body_->setFixedHeight(kMaxBodyHeight);
        body_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    } else {
        body_->setFixedHeight(contentHeight);
        body_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    refreshImagePreview(body);
}

void ToolCallWidget::refreshImagePreview(const QString& renderedBody) {
    if (!imagePreview_) return;
    if (!expanded_) {
        imagePreview_->setVisible(false);
        return;
    }

    QStringList paths = imagePathArgs(argsJson_, workingDir_);
    const QStringList textPaths = imagepreview::imagePathsInText(
        output_ + QLatin1Char('\n') + renderedBody, workingDir_);
    for (const QString& path : textPaths) {
        if (!paths.contains(path)) paths << path;
    }

    imagePreview_->setImagePaths(paths);
    imagePreview_->setVisible(imagePreview_->hasImages());
}

void ToolCallWidget::scrollBodyToBottom() {
    if (!expanded_ || !body_->isVisible()) return;

    auto scroll = [this]() {
        if (!body_) return;
        QScrollBar* vb = body_->verticalScrollBar();
        vb->setValue(vb->maximum());
    };
    scroll();
    QTimer::singleShot(0, this, scroll);
}


void ToolCallWidget::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    if (expanded_) refreshBody();
}

QString ToolCallWidget::lineDeltaSummary() const {
    // Only show deltas for finished calls.
    if (!finished_) return {};

    try {
        auto args = nlohmann::json::parse(argsJson_.toStdString());

        if (name_ == "edit") {
            if (!args.contains("old_string") || !args.contains("new_string")) return {};
            const std::string oldStr = args["old_string"].get<std::string>();
            const std::string newStr = args["new_string"].get<std::string>();
            // Compute actual added/removed lines from the diff, matching what
            // editDiffHtml shows in the body (context lines are not counted).
            const QVector<DiffLine> diff = compactDiffHunks(buildLineDiff(
                QString::fromStdString(oldStr), QString::fromStdString(newStr)));
            int added = 0, removed = 0;
            for (const DiffLine& line : diff) {
                if (line.kind == DiffLine::Added) ++added;
                else if (line.kind == DiffLine::Removed) ++removed;
            }
            if (added == 0 && removed == 0) return {};
            return formatLineDelta(added, removed);
        }

        if (name_ == "replace_lines") {
            if (!args.contains("start_line") || !args.contains("end_line")) return {};
            // Parse old/new content from the structured output if available.
            static const QRegularExpression oldRe(QStringLiteral("^=== OLD CONTENT ===\r?\n"));
            static const QRegularExpression newRe(QStringLiteral("\n=== NEW CONTENT ===\r?\n"));
            static const QRegularExpression resultRe(QStringLiteral("\n=== RESULT ===\r?\n"));

            const int oldMatch = oldRe.match(output_).capturedStart();
            const int newMatch = newRe.match(output_).capturedStart();
            const int resultMatch = resultRe.match(output_).capturedStart();

            if (oldMatch >= 0 && newMatch > oldMatch && resultMatch > newMatch) {
                const int oldStart = oldMatch + output_.mid(oldMatch).indexOf('\n') + 1;
                const QString oldContent = output_.mid(oldStart, newMatch - oldStart);
                const int newStart = newMatch + output_.mid(newMatch).indexOf('\n') + 1;
                const QString newContent = output_.mid(newStart, resultMatch - newStart);
                const QVector<DiffLine> diff = compactDiffHunks(buildLineDiff(oldContent, newContent));
                int added = 0, removed = 0;
                for (const DiffLine& line : diff) {
                    if (line.kind == DiffLine::Added) ++added;
                    else if (line.kind == DiffLine::Removed) ++removed;
                }
                if (added == 0 && removed == 0) return {};
                return formatLineDelta(added, removed);
            }
            // Fallback: naive line count from args.
            int start = args["start_line"].get<int>();
            int end = args["end_line"].get<int>();
            int removed = end - start + 1;
            int added = 0;
            if (args.contains("new_lines")) {
                added = logicalLineCount(QString::fromStdString(args["new_lines"].get<std::string>()));
            }
            return formatLineDelta(added, removed);
        }

        if (name_ == "write") {
            // Overwrite: compute the real added/removed from the embedded diff.
            QString oldText, newText;
            if (parseOldNewMarkers(output_, oldText, newText)) {
                const QVector<DiffLine> diff = compactDiffHunks(buildLineDiff(oldText, newText));
                int added = 0, removed = 0;
                for (const DiffLine& line : diff) {
                    if (line.kind == DiffLine::Added) ++added;
                    else if (line.kind == DiffLine::Removed) ++removed;
                }
                if (added == 0 && removed == 0) return {};
                return formatLineDelta(added, removed);
            }
            // New file: parse the "(+N -M)" the tool reported.
            static const QRegularExpression re(QStringLiteral("\\((\\+\\d+) -(\\d+)\\)"));
            const QRegularExpressionMatch m = re.match(output_);
            if (m.hasMatch()) {
                return formatLineDelta(m.captured(1).toInt(), m.captured(2).toInt());
            }
            if (args.contains("content")) {
                int n = logicalLineCount(QString::fromStdString(args["content"].get<std::string>()));
                if (n == 0) return {};
                return formatLineDelta(n, 0);
            }
        }
    } catch (...) {
        // JSON parse failure – no delta
    }
    return {};
}

QString ToolCallWidget::byteSummary() const {
    // Prefer the authoritative byte count from the tool result
    // ("wrote N bytes ..." / "appended N bytes ..."). For an overwrite the
    // result also embeds old/new content, so only scan the RESULT section to
    // avoid matching a number inside the file content.
    QString text = output_;
    const int resultIdx = text.indexOf(QStringLiteral("=== RESULT ==="));
    if (resultIdx >= 0) text = text.mid(resultIdx);

    static const QRegularExpression wroteRe(QStringLiteral("(\\d+)\\s+bytes"));
    const QRegularExpressionMatch m = wroteRe.match(text);
    if (m.hasMatch()) {
        return QStringLiteral("%1 bytes").arg(m.captured(1));
    }
    // Fallback: size of the content argument (e.g. while still running).
    try {
        auto args = nlohmann::json::parse(argsJson_.toStdString());
        if (args.contains("content")) {
            const int n = QString::fromStdString(args["content"].get<std::string>())
                              .toUtf8().size();
            return QStringLiteral("%1 bytes").arg(n);
        }
    } catch (...) {
    }
    return {};
}

QString ToolCallWidget::formatBashBody() const {
    QString body;
    try {
        auto args = nlohmann::json::parse(argsJson_.toStdString());

        QString command = QString::fromStdString(args.value("command", ""));
        QString output = output_.isEmpty() && !finished_
            ? QStringLiteral("[running...]")
            : prettifyEscapedLineDumps(output_);

        body += QStringLiteral("$ %1").arg(command);
        if (args.contains("timeout_ms")) {
            body += QStringLiteral("\n\n(timeout: %1ms)")
                        .arg(args["timeout_ms"].get<int>());
        }
        body += QStringLiteral("\n\n");
        body += output;
    } catch (const std::exception&) {
        body = output_.isEmpty() && !finished_
            ? QStringLiteral("[running...]")
            : prettifyEscapedLineDumps(output_);
    }
    return body;
}

QString ToolCallWidget::formatEditBody() const {
    // The file and status live in the header; the body is just the diff. On
    // failure the error message is shown instead.
    try {
        auto args = nlohmann::json::parse(argsJson_.toStdString());

        QString body = QStringLiteral("<div style=\"font-family:Consolas,monospace;\">");
        if (finished_ && !ok_) {
            body += metaHtml(QStringLiteral("error"), output_);
        } else if (args.contains("old_string")) {
            const QString oldStr = QString::fromStdString(args["old_string"].get<std::string>());
            const QString newStr = QString::fromStdString(args.value("new_string", ""));
            body += editDiffHtml(oldStr, newStr, languageFromArgsJson(argsJson_));
        } else {
            body += QStringLiteral("<div style=\"color:#9CA3AF;\">No edit preview available.</div>");
        }
        body += QStringLiteral("</div>");
        return body;
    } catch (const std::exception&) {
        const QString result = output_.isEmpty() ? QStringLiteral("[running...]") : output_;
        return QStringLiteral("<div style=\"font-family:Consolas,monospace;\">%1</div>")
            .arg(metaHtml(QStringLiteral("result"), result));
    }
}

QString ToolCallWidget::formatReplaceLinesBody() const {
    QString body;
    try {
        auto args = nlohmann::json::parse(argsJson_.toStdString());

        QString result = output_.isEmpty() ? QStringLiteral("[running…]") : output_;

        // Check if the tool output contains the structured old/new content
        // markers inserted by ReplaceLinesTool.
        static const QRegularExpression oldRe(QStringLiteral("^=== OLD CONTENT ===\r?\n"));
        static const QRegularExpression newRe(QStringLiteral("\n=== NEW CONTENT ===\r?\n"));
        static const QRegularExpression resultRe(QStringLiteral("\n=== RESULT ===\r?\n"));

        const int oldMatch = oldRe.match(result).capturedStart();
        const int newMatch = newRe.match(result).capturedStart();
        const int resultMatch = resultRe.match(result).capturedStart();

        if (oldMatch >= 0 && newMatch > oldMatch && resultMatch > newMatch) {
            // Parse old/new content from the structured output. The file is in
            // the header, so the body is just the diff.
            const int oldStart = oldMatch + result.mid(oldMatch).indexOf('\n') + 1;
            const QString oldContent = result.mid(oldStart, newMatch - oldStart);
            // newMatch points to '\n' before "=== NEW CONTENT ==="; skip that
            // line to reach the actual new content.
            const int newLineStart = newMatch + 1; // skip the leading '\n'
            const int newStart = newLineStart + result.mid(newLineStart).indexOf('\n') + 1;
            const QString newContent = result.mid(newStart, resultMatch - newStart);

            body += QStringLiteral("<div style=\"font-family:Consolas,monospace;\">");
            body += editDiffHtml(oldContent, newContent, languageFromArgsJson(argsJson_));
            body += QStringLiteral("</div>");
            return body;
        }

        // Fallback (older format / insertion): on error show the error, otherwise
        // just the new lines being inserted -- no internal labels.
        if (finished_ && !ok_) return output_;
        if (args.contains("new_lines")) {
            body = QString::fromStdString(args["new_lines"].get<std::string>());
        } else {
            body = output_;
        }
    } catch (const std::exception&) {
        body = output_;
    }
    return body;
}

QString ToolCallWidget::formatWriteBody() const {
    // Returns HTML. The path, status, byte count and line delta are all in the
    // header, so the body is just a preview of the content being written --
    // rendered as added (green) lines so it reads consistently with the
    // edit/replace_lines diffs. On failure the error message is shown instead.
    if (output_.isEmpty() && !finished_)
        return QStringLiteral("<div style=\"color:#9CA3AF;\">[running…]</div>");
    if (finished_ && !ok_)
        return QStringLiteral("<div style=\"font-family:Consolas,monospace;\">%1</div>")
            .arg(metaHtml(QStringLiteral("error"), output_));

    // Overwrote an existing file: show a real diff (added AND removed lines).
    QString oldText, newText;
    if (parseOldNewMarkers(output_, oldText, newText)) {
        return editDiffHtml(oldText, newText, languageFromArgsJson(argsJson_));
    }

    try {
        auto args = nlohmann::json::parse(argsJson_.toStdString());
        if (!args.contains("content")) {
            return QStringLiteral("<div style=\"color:#9CA3AF;\">%1</div>")
                .arg((output_.isEmpty() ? QStringLiteral("[no content]") : output_).toHtmlEscaped());
        }

        const QString content = QString::fromStdString(args["content"].get<std::string>());
        const QStringList lines = content.split('\n');
        const int previewLines = qMin(50, lines.size());

        QString preview;
        for (int i = 0; i < previewLines; ++i) {
            preview += lines[i] + "\n";
        }
        // Diff against empty -> every line renders as an added (green) line,
        // reusing the exact styling of the edit/replace_lines diff blocks.
        QString html = editDiffHtml(QString(), preview, languageFromArgsJson(argsJson_));
        if (lines.size() > 50) {
            html += QStringLiteral("<div style=\"color:#9CA3AF;\">[... %1 more lines]</div>")
                        .arg(lines.size() - 50);
        }
        return html;
    } catch (const std::exception&) {
        return QStringLiteral("<div style=\"color:#9CA3AF;\">%1</div>").arg(output_.toHtmlEscaped());
    }
}

QString ToolCallWidget::formatReadBody() const {
    // ReadTool emits line-numbered output ("%6d\t<content>"). Render it in the
    // same monospace block the diffs use, dimming the line-number gutter and
    // syntax-highlighting the content. Non-numbered status lines (e.g.
    // "[binary file ...]", "[empty file]", truncation notes) are shown dimmed.
    const QString raw = output_.isEmpty() && !finished_
        ? QStringLiteral("[running…]") : output_;
    if (raw.isEmpty()) return {};

    const codecolor::Language lang = languageFromArgsJson(argsJson_);
    const QStringList lines = limitBodyText(raw).split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    // Leading spaces (gutter padding), the line number, a tab, then the content.
    static const QRegularExpression gutterRe(QStringLiteral("^( *)(\\d+)\\t(.*)$"));

    QString html = codeBlockOpen();
    for (const QString& line : lines) {
        const QRegularExpressionMatch m = gutterRe.match(line);
        QString inner;
        if (m.hasMatch()) {
            QString content = m.captured(3);
            content.replace(QLatin1Char('\t'), QStringLiteral("  "));  // match expandTabsForDisplay
            inner = m.captured(1).toHtmlEscaped()
                  + dimSpan(m.captured(2), "#6B7280") + QStringLiteral("  ")
                  + codecolor::highlightLine(content, lang);
        } else {
            inner = dimSpan(line);  // status line ([binary file ...], truncation, ...)
        }
        html += codeLineDiv(inner);
    }
    html += QStringLiteral("</div>");
    return html;
}

QString ToolCallWidget::formatGrepBody() const {
    // grep (content mode) emits "path:line:matched-text" per line; other modes
    // emit bare paths or "path:count". Highlight the matched code (language taken
    // per-line from its own path so a multi-file result colours each correctly)
    // and dim the "path:line:" locator. Non-content lines are shown plainly.
    const QString raw = output_.isEmpty() && !finished_
        ? QStringLiteral("[running…]") : output_;
    if (raw.isEmpty()) return {};

    const QStringList lines = limitBodyText(raw).split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    // path (may contain a Windows drive ':') then ":<line>:" then the text.
    // The non-greedy path stops at the first ":<digits>:" run.
    static const QRegularExpression hitRe(QStringLiteral("^(.*?):(\\d+):(.*)$"));

    QString html = codeBlockOpen();
    for (const QString& line : lines) {
        const QRegularExpressionMatch m = hitRe.match(line);
        QString inner;
        if (m.hasMatch()) {
            const QString path = m.captured(1);
            QString content = m.captured(3);
            content.replace(QLatin1Char('\t'), QStringLiteral("  "));
            inner = dimSpan(path + QStringLiteral(":") + m.captured(2) + QStringLiteral(":"), "#6B7280")
                  + codecolor::highlightLine(content, codecolor::languageForPath(path));
        } else {
            // Bare path (files_with_matches), "path:count", or a status line.
            inner = dimSpan(line, "#D1D5DB");
        }
        html += codeLineDiv(inner);
    }
    html += QStringLiteral("</div>");
    return html;
}

QString ToolCallWidget::formatReadOutlineBody() const {
    // read_outline emits "<indent>kind start-end hash=.. [name=..]: signature".
    // The signature is the code-like part: dim the metadata locator and
    // syntax-highlight the signature. Header/footer/status lines are dimmed.
    const QString raw = output_.isEmpty() && !finished_
        ? QStringLiteral("[running…]") : output_;
    if (raw.isEmpty()) return {};

    const codecolor::Language lang = languageFromArgsJson(argsJson_);
    const QStringList lines = limitBodyText(raw).split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    QString html = codeBlockOpen();
    for (const QString& line : lines) {
        const int sep = line.indexOf(QStringLiteral(": "));
        // An entry line carries "hash=" in its metadata before the ": signature".
        const bool isEntry = sep > 0 && line.left(sep).contains(QStringLiteral("hash="));
        QString inner;
        if (isEntry) {
            inner = dimSpan(line.left(sep + 2))  // "...: " locator, dimmed
                  + codecolor::highlightLine(line.mid(sep + 2), lang);
        } else {
            inner = dimSpan(line);  // "=== Outline: ... ===", "(file has ...)", etc.
        }
        html += codeLineDiv(inner);
    }
    html += QStringLiteral("</div>");
    return html;
}
