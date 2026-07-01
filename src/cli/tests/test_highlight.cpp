#include "TestHarness.h"

#include <QString>

#include "ui/CodeColorizer.h"

// Unit tests for the diff syntax colorizer. highlightLine() returns HTML, so we
// assert on the presence (or absence) of the expected per-token color spans and
// on HTML-escaping safety.

using namespace test;
using codecolor::Language;

namespace {

// Color constants must match CodeColorizer.cpp.
const char* kComment = "#6A9955";
const char* kString  = "#CE9178";
const char* kNumber  = "#B5CEA8";
const char* kPreproc = "#C586C0";
const char* kKeyword = "#569CD6";

bool has(const QString& haystack, const char* needle) {
    return haystack.contains(QLatin1String(needle));
}

void expectHas(const QString& html, const char* needle, const std::string& what) {
    CHECK_MSG(has(html, needle), what + " -- got: " + html.toStdString());
}

void expectNot(const QString& html, const char* needle, const std::string& what) {
    CHECK_MSG(!has(html, needle), what + " -- got: " + html.toStdString());
}

}  // namespace

void suite_highlight() {
    using codecolor::highlightLine;
    using codecolor::languageForPath;

    // ---- language detection ----
    CHECK(languageForPath(QStringLiteral("foo.cpp")) == Language::Cpp);
    CHECK(languageForPath(QStringLiteral("a/b/Widget.h")) == Language::Cpp);
    CHECK(languageForPath(QStringLiteral("x.HXX")) == Language::Cpp);   // case-insensitive
    CHECK(languageForPath(QStringLiteral("README.md")) == Language::Generic);
    CHECK(languageForPath(QStringLiteral("Makefile")) == Language::Generic);  // no extension
    CHECK(languageForPath(QString()) == Language::Generic);

    // ---- C/C++ token classes ----
    {
        const QString h = highlightLine(QStringLiteral("int x = 0;"), Language::Cpp);
        expectHas(h, kKeyword, "cpp: 'int' is a keyword");
        expectHas(h, kNumber, "cpp: '0' is a number");
    }
    {
        const QString h = highlightLine(QStringLiteral("return 0xFF;"), Language::Cpp);
        expectHas(h, kKeyword, "cpp: 'return' is a keyword");
        expectHas(h, kNumber, "cpp: '0xFF' is a number");
    }
    expectHas(highlightLine(QStringLiteral("auto s = \"text\";"), Language::Cpp),
              kString, "cpp: string literal");
    expectHas(highlightLine(QStringLiteral("x++;  // trailing note"), Language::Cpp),
              kComment, "cpp: line comment");
    expectHas(highlightLine(QStringLiteral("y = /* inline */ z;"), Language::Cpp),
              kComment, "cpp: block comment");
    expectHas(highlightLine(QStringLiteral("#include <foo>"), Language::Cpp),
              kPreproc, "cpp: preprocessor directive");

    // ---- HTML-escaping safety + literal '%' survival (arg()->concat guard) ----
    {
        const QString h = highlightLine(QStringLiteral("if (a < b && c > d) {"), Language::Cpp);
        expectHas(h, "&lt;", "escape: '<' becomes &lt;");
        expectHas(h, "&gt;", "escape: '>' becomes &gt;");
        expectHas(h, "&amp;", "escape: '&' becomes &amp;");
    }
    expectHas(highlightLine(QStringLiteral("printf(\"x%dy\");"), Language::Cpp),
              "x%dy", "literal '%' survives highlighting");

    // ---- generic fallback ----
    {
        const QString h = highlightLine(QStringLiteral("if (x) return y"), Language::Generic);
        expectHas(h, kKeyword, "generic: 'if'/'return' are keywords");
    }
    {
        // C++-only keywords/types must NOT highlight under the generic fallback.
        const QString h = highlightLine(QStringLiteral("constexpr int v = w"), Language::Generic);
        expectNot(h, kKeyword, "generic: 'constexpr'/'int' are NOT generic keywords");
    }
    expectHas(highlightLine(QStringLiteral("count = 5  # python comment"), Language::Generic),
              kComment, "generic: '#' line comment");

    // ---- languageForName (markdown fence tags) ----
    CHECK(codecolor::languageForName(QStringLiteral("cpp")) == Language::Cpp);
    CHECK(codecolor::languageForName(QStringLiteral("C++")) == Language::Cpp);
    CHECK(codecolor::languageForName(QStringLiteral("h")) == Language::Cpp);
    CHECK(codecolor::languageForName(QStringLiteral("python")) == Language::Generic);
    CHECK(codecolor::languageForName(QString()) == Language::Generic);

    // ---- spansForLine: colored ranges over the original line ----
    {
        const QString line = QStringLiteral("int x = 0;");
        const auto spans = codecolor::spansForLine(line, Language::Cpp);
        // 'int' (keyword) and '0' (number); 'x', '=', ';' are uncolored default.
        CHECK_MSG(spans.size() == 2,
                  "spansForLine: 2 colored tokens, got " + std::to_string(spans.size()));
        bool sawKeyword = false, sawNumber = false;
        for (const codecolor::Span& s : spans) {
            const QString tok = line.mid(s.start, s.length);
            if (tok == QStringLiteral("int")) {
                sawKeyword = true;
                CHECK(s.color == QStringLiteral("#569CD6"));
            } else if (tok == QStringLiteral("0")) {
                sawNumber = true;
                CHECK(s.color == QStringLiteral("#B5CEA8"));
            }
        }
        CHECK_MSG(sawKeyword, "spansForLine: 'int' span present");
        CHECK_MSG(sawNumber, "spansForLine: '0' span present");
    }
}
