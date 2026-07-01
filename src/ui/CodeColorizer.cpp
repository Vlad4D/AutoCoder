#include "CodeColorizer.h"

#include <QChar>
#include <QSet>

namespace codecolor {

namespace {

// VS Code-like dark palette. These set only text color, so they read on the
// diff's removed (#2A1416) / added (#142A1B) / container (#0B1220) backgrounds
// as well as the assistant bubble's code blocks.
const QString kColComment = QStringLiteral("#6A9955");
const QString kColString  = QStringLiteral("#CE9178");
const QString kColNumber  = QStringLiteral("#B5CEA8");
const QString kColPreproc = QStringLiteral("#C586C0");
const QString kColKeyword = QStringLiteral("#569CD6");

const QSet<QString>& cppKeywords() {
    static const QSet<QString> k = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
        "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
        "char32_t", "class", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await",
        "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
        "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
        "float", "for", "friend", "goto", "if", "inline", "int", "long",
        "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
        "operator", "or", "or_eq", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
        "static", "static_assert", "static_cast", "struct", "switch", "template",
        "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
        "wchar_t", "while", "xor", "xor_eq", "override", "final",
    };
    return k;
}

// Small cross-language set for the generic fallback (covers common shapes in
// JS/TS, Python, Go, Java, Rust, etc.).
const QSet<QString>& genericKeywords() {
    static const QSet<QString> k = {
        "if", "else", "for", "while", "do", "return", "function", "func", "fn",
        "def", "class", "struct", "enum", "interface", "import", "from",
        "export", "module", "package", "const", "let", "var", "val", "new",
        "delete", "try", "catch", "finally", "throw", "throws", "switch",
        "case", "break", "continue", "default", "true", "false", "null", "nil",
        "none", "undefined", "void", "public", "private", "protected", "static",
        "async", "await", "yield", "in", "is", "of", "and", "or", "not",
        "lambda", "pass", "with", "as", "self", "this", "super", "extends",
        "implements", "type",
    };
    return k;
}

bool isIdentStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
bool isIdentPart(QChar c)  { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

}  // namespace

Language languageForPath(const QString& path) {
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot < 0) return Language::Generic;
    const QString ext = path.mid(dot + 1).toLower();
    static const QSet<QString> cpp = {
        "c", "cc", "cpp", "cxx", "c++", "h", "hpp", "hh", "hxx", "h++",
        "ipp", "inl", "tpp",
    };
    return cpp.contains(ext) ? Language::Cpp : Language::Generic;
}

Language languageForName(const QString& name) {
    static const QSet<QString> cpp = {
        "c", "cc", "cpp", "cxx", "c++", "cplusplus", "h", "hpp", "hh", "hxx",
        "h++", "ipp", "inl", "tpp",
    };
    return cpp.contains(name.trimmed().toLower()) ? Language::Cpp : Language::Generic;
}

QVector<Span> spansForLine(const QString& line, Language lang) {
    const bool cpp = (lang == Language::Cpp);
    const int n = line.size();
    QVector<Span> spans;

    int i = 0;
    while (i < n) {
        const QChar c = line[i];

        // C/C++ preprocessor: a '#' that is the first non-blank on the line.
        if (cpp && c == QLatin1Char('#') && line.left(i).trimmed().isEmpty()) {
            int j = i + 1;
            while (j < n && (line[j] == QLatin1Char(' ') || line[j] == QLatin1Char('\t'))) ++j;
            while (j < n && line[j].isLetter()) ++j;
            spans.push_back({i, j - i, kColPreproc});
            i = j;
            continue;
        }

        // Line comment: // (both languages) or # (generic only).
        if ((c == QLatin1Char('/') && i + 1 < n && line[i + 1] == QLatin1Char('/'))
            || (!cpp && c == QLatin1Char('#'))) {
            spans.push_back({i, n - i, kColComment});
            break;
        }

        // Block comment /* ... */ -- line-local (to closing or end of line).
        if (c == QLatin1Char('/') && i + 1 < n && line[i + 1] == QLatin1Char('*')) {
            const int close = line.indexOf(QStringLiteral("*/"), i + 2);
            const int end = (close < 0) ? n : close + 2;
            spans.push_back({i, end - i, kColComment});
            i = end;
            continue;
        }

        // String / char literal with backslash escapes; unterminated -> to EOL.
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const QChar quote = c;
            int j = i + 1;
            while (j < n) {
                if (line[j] == QLatin1Char('\\')) { j += 2; continue; }
                if (line[j] == quote) { ++j; break; }
                ++j;
            }
            if (j > n) j = n;
            spans.push_back({i, j - i, kColString});
            i = j;
            continue;
        }

        // Number: a digit, or a '.' immediately followed by a digit.
        if (c.isDigit()
            || (c == QLatin1Char('.') && i + 1 < n && line[i + 1].isDigit())) {
            int j = i + 1;
            while (j < n) {
                const QChar d = line[j];
                if (d.isLetterOrNumber() || d == QLatin1Char('.')
                    || d == QLatin1Char('_') || d == QLatin1Char('\'')) {
                    ++j;
                } else if ((d == QLatin1Char('+') || d == QLatin1Char('-'))
                           && (line[j - 1] == QLatin1Char('e')
                               || line[j - 1] == QLatin1Char('E'))) {
                    ++j;  // exponent sign
                } else {
                    break;
                }
            }
            spans.push_back({i, j - i, kColNumber});
            i = j;
            continue;
        }

        // Identifier / keyword.
        if (isIdentStart(c)) {
            int j = i + 1;
            while (j < n && isIdentPart(line[j])) ++j;
            const QString word = line.mid(i, j - i);
            const QSet<QString>& kw = cpp ? cppKeywords() : genericKeywords();
            if (kw.contains(word)) spans.push_back({i, j - i, kColKeyword});
            i = j;
            continue;
        }

        // Default: a single character (whitespace, operator, punctuation).
        ++i;
    }

    return spans;
}

QString highlightLine(const QString& line, Language lang) {
    const QVector<Span> spans = spansForLine(line, lang);
    QString out;
    out.reserve(line.size() + 16);

    int pos = 0;
    for (const Span& s : spans) {
        if (s.start > pos)
            out += line.mid(pos, s.start - pos).toHtmlEscaped();
        out += QStringLiteral("<span style=\"color:%1;\">%2</span>")
                   .arg(s.color, line.mid(s.start, s.length).toHtmlEscaped());
        pos = s.start + s.length;
    }
    if (pos < line.size())
        out += line.mid(pos).toHtmlEscaped();
    return out;
}

}  // namespace codecolor
