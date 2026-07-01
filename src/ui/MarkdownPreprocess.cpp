#include "MarkdownPreprocess.h"

#include <QChar>
#include <QList>
#include <QStringList>
#include <QStringView>

namespace {

enum class Kind { None, Box, Border, Pipe };

int leadingSpaces(QStringView s) {
    int i = 0;
    while (i < s.size() && s[i] == u' ') ++i;
    return i;
}

bool isBlank(QStringView line) {
    for (QChar c : line)
        if (!c.isSpace()) return false;
    return true;
}

// Opening code fence per CommonMark: up to 3 spaces of indent, then 3+
// backticks or tildes. A backtick fence's info string may not contain
// backticks. Returns the fence run length (0 if not a fence) and sets *ch.
int fenceRun(QStringView line, QChar* ch) {
    const int i = leadingSpaces(line);
    if (i > 3 || i >= line.size()) return 0;
    const QChar c = line[i];
    if (c != u'`' && c != u'~') return 0;
    int n = 0;
    while (i + n < line.size() && line[i + n] == c) ++n;
    if (n < 3) return 0;
    if (c == u'`') {
        for (int j = i + n; j < line.size(); ++j)
            if (line[j] == u'`') return 0;
    }
    *ch = c;
    return n;
}

// Closing fence: same char, at least the opening length, nothing but
// whitespace after (a closing fence has no info string).
bool closesFence(QStringView line, QChar ch, int openLen) {
    const int i = leadingSpaces(line);
    if (i > 3 || i >= line.size()) return false;
    int n = 0;
    while (i + n < line.size() && line[i + n] == ch) ++n;
    if (n < openLen) return false;
    for (int j = i + n; j < line.size(); ++j)
        if (!line[j].isSpace()) return false;
    return true;
}

// Two or more box-drawing characters (U+2500..U+257F): table borders, file
// trees. One alone may be prose mentioning the character.
bool isBoxLine(QStringView line) {
    int n = 0;
    for (QChar c : line) {
        const char16_t u = c.unicode();
        if (u >= 0x2500 && u <= 0x257F && ++n >= 2) return true;
    }
    return false;
}

// "+---+---+" / "+===+" style horizontal border.
bool isBorderLine(QStringView line) {
    const QStringView t = line.trimmed();
    if (t.size() < 3 || t.front() != u'+' || t.back() != u'+') return false;
    for (QChar c : t)
        if (c != u'+' && c != u'-' && c != u'=') return false;
    return true;
}

bool startsWithListMarker(QStringView line) {
    const int i = leadingSpaces(line);
    if (i > 3 || i >= line.size()) return false;
    const QChar c = line[i];
    if (c == u'-' || c == u'*' || c == u'+')
        return i + 1 < line.size() && line[i + 1] == u' ';
    if (c.isDigit()) {
        int j = i;
        while (j < line.size() && line[j].isDigit()) ++j;
        if (j < line.size() && (line[j] == u'.' || line[j] == u')'))
            return j + 1 < line.size() && line[j + 1] == u' ';
    }
    return false;
}

// Two or more '|' cell separators, and not a bullet/numbered list item
// ("- a | b" is prose). Art rows need not start with '|'.
bool isPipeRow(QStringView line) {
    if (startsWithListMarker(line)) return false;
    int n = 0;
    for (QChar c : line)
        if (c == u'|' && ++n >= 2) return true;
    return false;
}

Kind classify(QStringView line) {
    if (isBoxLine(line)) return Kind::Box;
    if (isBorderLine(line)) return Kind::Border;
    if (isPipeRow(line)) return Kind::Pipe;
    return Kind::None;
}

// Strip outer pipes GFM-style; the remainder's '|' count + 1 is the cell count.
QStringView stripOuterPipes(QStringView line) {
    QStringView t = line.trimmed();
    if (t.startsWith(u'|')) t = t.mid(1);
    if (t.endsWith(u'|')) t = t.chopped(1);
    return t;
}

int pipeCellCount(QStringView line) {
    int cells = 1;
    for (QChar c : stripOuterPipes(line))
        if (c == u'|') ++cells;
    return cells;
}

// GFM delimiter row: every cell is ":?-+:?" (at least one dash).
bool isGfmDelimiterRow(QStringView line) {
    const auto cells = stripOuterPipes(line).split(u'|');
    if (cells.isEmpty()) return false;
    for (QStringView cell : cells) {
        QStringView t = cell.trimmed();
        if (t.startsWith(u':')) t = t.mid(1);
        if (t.endsWith(u':')) t = t.chopped(1);
        if (t.isEmpty()) return false;
        for (QChar c : t)
            if (c != u'-') return false;
    }
    return true;
}

struct RunLine {
    QString text;
    Kind kind;
};

// A valid GFM pipe table (header row, delimiter row with matching cell count,
// pipe-only body) renders as a real QTextTable -- leave it for Qt.
bool isValidGfmTable(const QList<RunLine>& run) {
    if (run.size() < 2) return false;
    for (const RunLine& l : run)
        if (l.kind != Kind::Pipe) return false;
    if (!isGfmDelimiterRow(run[1].text)) return false;
    return pipeCellCount(run[0].text) == pipeCellCount(run[1].text);
}

}  // namespace

namespace mdpre {

QString fenceTableArt(const QString& markdown) {
    QStringList lines = markdown.split(u'\n');
    for (QString& l : lines)
        if (l.endsWith(u'\r')) l.chop(1);

    QStringList out;
    out.reserve(lines.size() + 8);

    QList<RunLine> run;  // pending candidate table-art lines
    bool runHasArt = false;  // run contains a box/border line (unambiguous art)

    auto flushRun = [&]() {
        if (run.isEmpty()) return;
        const bool wrap = !isValidGfmTable(run) && (run.size() >= 2 || runHasArt);
        if (wrap) {
            // Blank line before the fence so it can't attach to a preceding
            // paragraph or list item.
            if (!out.isEmpty() && !isBlank(out.last()))
                out << QString();
            out << QStringLiteral("```text");
            for (const RunLine& l : run) out << l.text;
            out << QStringLiteral("```");
        } else {
            for (const RunLine& l : run) out << l.text;
        }
        run.clear();
        runHasArt = false;
    };

    bool inFence = false;
    QChar fenceCh;
    int fenceLen = 0;

    for (const QString& line : lines) {
        if (inFence) {
            out << line;
            if (closesFence(line, fenceCh, fenceLen)) inFence = false;
            continue;
        }
        QChar ch;
        if (const int n = fenceRun(line, &ch)) {
            flushRun();
            inFence = true;
            fenceCh = ch;
            fenceLen = n;
            out << line;
            continue;
        }
        const Kind k = classify(line);
        if (k != Kind::None) {
            run.append({line, k});
            if (k != Kind::Pipe) runHasArt = true;
        } else {
            flushRun();
            out << line;
        }
    }
    // End-of-buffer flush: closes the synthetic fence for a table still
    // streaming in; the next rerender reprocesses the full buffer anyway.
    flushRun();

    return out.join(u'\n');
}

}  // namespace mdpre
