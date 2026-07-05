#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace repomap {

// Compact project map for the system prompt: one line per non-ignored file
// (relative path), enriched with top-level symbols for C/C++ sources where
// the byte budget allows. Gives the model an immediate picture of the
// project so it does not have to spend its first tool calls on glob/ls
// discovery, and lets it jump straight to the right file for a symbol.
//
// Honors the same exclusions as the file tools (pathutil::isExcludedDir +
// IgnoreRules). Output is capped at maxBytes; the plain path listing is
// always emitted first, and symbol suffixes only spend whatever budget is
// left over, so path coverage never regresses. Byte-stable for an unchanged
// tree (provider-cache-friendly). Returns an empty string on any error (the
// system prompt simply omits the map).
std::string build(const std::filesystem::path& projectRoot,
                  std::size_t maxBytes = 24 * 1024);

// The ": symbol, symbol, ..." suffix for one source file's map line: its
// top-level symbols (classes/structs/enums and free or member functions,
// excluding container members) capped at roughly maxChars. Returns an empty
// string for non-C-family files, unreadable files, or files with no symbols.
// Exposed for the offline tests.
std::string symbolSuffixForFile(const std::filesystem::path& file,
                                std::size_t maxChars = 200);

}  // namespace repomap
