#pragma once

#include <QString>
#include <QVector>

// Lightweight, stateless syntax colorizer shared by the tool-call code views and
// the assistant message bubbles. It exposes both an HTML form (for the HTML diff/
// read pipeline) and a span form (colored ranges, for applying QTextCharFormat to
// a QTextDocument). Intentionally NOT a QSyntaxHighlighter.
namespace codecolor {

enum class Language { Generic, Cpp };

// A colored token range over the ORIGINAL line, in UTF-16 code units. Only
// colored tokens are reported; default text (identifiers, operators, whitespace)
// produces no span. color is a CSS hex string, e.g. "#569CD6".
struct Span {
    int start = 0;
    int length = 0;
    QString color;
};

// Pick a language from a file path's extension. C/C++ extensions map to Cpp;
// everything else (including an empty path) maps to Generic.
Language languageForPath(const QString& path);

// Pick a language from a markdown fence info string / language tag (e.g. "cpp",
// "c++", "h"). Unknown/empty maps to Generic.
Language languageForName(const QString& name);

// Colored token ranges for a single line (the shared lexer used by highlightLine).
QVector<Span> spansForLine(const QString& line, Language lang);

// Highlight a single line of code. The returned string is HTML: every character
// is HTML-escaped and colored tokens are wrapped in <span> elements. Default
// tokens (identifiers, operators, whitespace) are emitted as plain escaped text
// and inherit the surrounding element's color.
//
// Highlighting is per-line and stateless: a block comment or string that spans
// multiple lines is colored line-locally. This is deliberate -- diff lines are
// interleaved (added vs removed), so carrying lexer state across them would be
// unreliable anyway.
QString highlightLine(const QString& line, Language lang);

}  // namespace codecolor
