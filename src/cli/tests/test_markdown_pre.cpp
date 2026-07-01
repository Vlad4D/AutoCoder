#include "TestHarness.h"

#include <QString>
#include <QStringList>

#include "ui/MarkdownPreprocess.h"

// Unit tests for mdpre::fenceTableArt(): ASCII/box-drawing table art gets
// wrapped in ```text fences, while prose, existing fences and valid GFM pipe
// tables pass through untouched.
//
// This file contains literal box-drawing characters and is saved as UTF-8
// WITH BOM so MSVC decodes the literals correctly (the build does not pass
// /utf-8). Keep the BOM if you edit this file.

using namespace test;
using mdpre::fenceTableArt;

namespace {

QString lines(std::initializer_list<QString> ls) {
    QStringList out;
    for (const QString& l : ls) out << l;
    return out.join(QLatin1Char('\n'));
}

bool wrapped(const QString& out, const QString& artLine) {
    // The art line must appear between a ```text opener and a closing ```.
    const int fence = out.indexOf(QLatin1String("```text"));
    if (fence < 0) return false;
    const int art = out.indexOf(artLine, fence);
    if (art < 0) return false;
    return out.indexOf(QLatin1String("```"), art + artLine.size()) >= 0;
}

std::string s(const QString& q) { return q.toStdString(); }

}  // namespace

void suite_markdown_pre() {
    // ---- box-drawing table -> fenced ----
    {
        const QString in = lines({
            QStringLiteral("Here is the layout:"),
            QStringLiteral("┌──┬──┐"),
            QStringLiteral("│a │b │"),
            QStringLiteral("└──┴──┘"),
            QStringLiteral("Done."),
        });
        const QString out = fenceTableArt(in);
        CHECK_MSG(wrapped(out, QStringLiteral("│a │b │")),
                  "box table fenced -- got: " + s(out));
        CHECK_MSG(out.contains(QLatin1String("Here is the layout:")) &&
                  out.contains(QLatin1String("Done.")),
                  "prose around box table preserved -- got: " + s(out));
    }

    // ---- "+---+" bordered table -> fenced ----
    {
        const QString in = lines({
            QStringLiteral("+----+-----+"),
            QStringLiteral("| id | name|"),
            QStringLiteral("+----+-----+"),
        });
        const QString out = fenceTableArt(in);
        CHECK_MSG(wrapped(out, QStringLiteral("| id | name|")),
                  "plus-border table fenced -- got: " + s(out));
    }

    // ---- pipe rows without a GFM delimiter -> fenced ----
    {
        const QString in = lines({
            QStringLiteral("| Name  | Age |"),
            QStringLiteral("| Alice | 30  |"),
            QStringLiteral("| Bob   | 41  |"),
        });
        const QString out = fenceTableArt(in);
        CHECK_MSG(wrapped(out, QStringLiteral("| Alice | 30  |")),
                  "separator-less pipe table fenced -- got: " + s(out));
    }

    // ---- valid GFM table -> byte-identical passthrough ----
    {
        const QString in = lines({
            QStringLiteral("| Name  | Age |"),
            QStringLiteral("|-------|-----|"),
            QStringLiteral("| Alice | 30  |"),
        });
        CHECK_MSG(fenceTableArt(in) == in,
                  "valid GFM table untouched -- got: " + s(fenceTableArt(in)));
    }
    {
        const QString in = lines({
            QStringLiteral("| a | b |"),
            QStringLiteral("| :-- | --: |"),
            QStringLiteral("| 1 | 2 |"),
        });
        CHECK_MSG(fenceTableArt(in) == in,
                  "GFM table with alignment colons untouched -- got: "
                      + s(fenceTableArt(in)));
    }

    // ---- "GFM" table with mismatched delimiter cell count -> fenced ----
    {
        const QString in = lines({
            QStringLiteral("| Name  | Age | City |"),
            QStringLiteral("|-------|-----|"),
            QStringLiteral("| Alice | 30  | Oslo |"),
        });
        CHECK_MSG(wrapped(fenceTableArt(in), QStringLiteral("|-------|-----|")),
                  "cell-count mismatch fenced -- got: " + s(fenceTableArt(in)));
    }

    // ---- art inside an existing fence -> unchanged ----
    {
        const QString in = lines({
            QStringLiteral("```"),
            QStringLiteral("| a | b |"),
            QStringLiteral("| c | d |"),
            QStringLiteral("```"),
        });
        CHECK_MSG(fenceTableArt(in) == in,
                  "art inside ``` fence untouched -- got: " + s(fenceTableArt(in)));
    }
    {
        const QString in = lines({
            QStringLiteral("~~~"),
            QStringLiteral("+---+---+"),
            QStringLiteral("| a | b |"),
            QStringLiteral("~~~"),
        });
        CHECK_MSG(fenceTableArt(in) == in,
                  "art inside ~~~ fence untouched -- got: " + s(fenceTableArt(in)));
    }

    // ---- unclosed fence at end of buffer (mid-stream) -> unchanged ----
    {
        const QString in = lines({
            QStringLiteral("```cpp"),
            QStringLiteral("| a | b |"),
            QStringLiteral("| c | d |"),
        });
        CHECK_MSG(fenceTableArt(in) == in,
                  "unclosed fence untouched -- got: " + s(fenceTableArt(in)));
    }

    // ---- prose with pipes -> unchanged ----
    {
        const QString in = QStringLiteral("use `a | b | c` to pipe commands");
        CHECK_MSG(fenceTableArt(in) == in,
                  "lone pipe prose line untouched -- got: " + s(fenceTableArt(in)));
    }
    {
        const QString in = lines({
            QStringLiteral("- foo | bar | baz"),
            QStringLiteral("- qux | quux | corge"),
        });
        CHECK_MSG(fenceTableArt(in) == in,
                  "bullet list with pipes untouched -- got: " + s(fenceTableArt(in)));
    }

    // ---- art run at end of buffer (mid-stream) -> fenced and closed ----
    {
        const QString in = lines({
            QStringLiteral("Partial table:"),
            QStringLiteral("+----+----+"),
            QStringLiteral("| a  | b  |"),
        });
        const QString out = fenceTableArt(in);
        CHECK_MSG(wrapped(out, QStringLiteral("| a  | b  |")),
                  "mid-stream art fenced -- got: " + s(out));
        CHECK_MSG(out.endsWith(QLatin1String("```")),
                  "synthetic fence closed at EOF -- got: " + s(out));
    }

    // ---- lone file-tree line (box chars) -> fenced ----
    {
        const QString in = QStringLiteral("├── src/");
        CHECK_MSG(wrapped(fenceTableArt(in), in),
                  "file-tree line fenced -- got: " + s(fenceTableArt(in)));
    }

    // ---- CRLF input -> classified correctly, LF-normalized output ----
    {
        const QString in = QStringLiteral("| a | b |\r\n| c | d |\r\n");
        const QString out = fenceTableArt(in);
        CHECK_MSG(wrapped(out, QStringLiteral("| a | b |")),
                  "CRLF pipe rows fenced -- got: " + s(out));
        CHECK_MSG(!out.contains(QLatin1Char('\r')), "output LF-normalized");
    }

    // ---- idempotence: f(f(x)) == f(x) ----
    {
        const QString in = lines({
            QStringLiteral("Intro"),
            QStringLiteral("+---+---+"),
            QStringLiteral("| a | b |"),
            QStringLiteral("+---+---+"),
            QString(),
            QStringLiteral("| h1 | h2 |"),
            QStringLiteral("|----|----|"),
            QStringLiteral("| x  | y  |"),
            QString(),
            QStringLiteral("```py"),
            QStringLiteral("x | y | z"),
            QStringLiteral("```"),
            QStringLiteral("Outro"),
        });
        const QString once = fenceTableArt(in);
        CHECK_MSG(fenceTableArt(once) == once,
                  "idempotent -- second pass changed: " + s(fenceTableArt(once)));
    }
}
