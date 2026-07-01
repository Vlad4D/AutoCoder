#pragma once

#include <QString>

// Preprocessing applied to assistant markdown before QTextBrowser::setMarkdown().
// Qt Core only (no Widgets) so it can be unit-tested in autocoder_tests, like
// CodeColorizer.
namespace mdpre {

// Wrap runs of ASCII/box-drawing "table art" (column layouts the model drew
// assuming a fixed-width font) in ``` fences so the markdown renderer shows
// them monospaced with preserved whitespace. Lines already inside a fenced
// code block, and valid GFM pipe tables (which Qt renders as real tables),
// pass through untouched. Pure function; safe to call repeatedly on a growing
// stream buffer, and idempotent. Output is LF-normalized.
//
// Known limitation: columns aligned purely with spaces (no '|', '+' borders or
// box-drawing characters) are indistinguishable from prose and are left alone;
// the system prompt steers the model away from emitting those.
QString fenceTableArt(const QString& markdown);

}  // namespace mdpre
