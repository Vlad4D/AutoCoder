#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace repomap {

// Compact project map for the system prompt: one line per non-ignored file
// (relative path). Gives the model an immediate picture of the project so it
// does not have to spend its first tool calls on glob/ls discovery.
//
// Honors the same exclusions as the file tools (pathutil::isExcludedDir +
// IgnoreRules). Output is capped at maxBytes. Returns an empty string on
// any error (the system prompt simply omits the map).
std::string build(const std::filesystem::path& projectRoot,
                  std::size_t maxBytes = 24 * 1024);

}  // namespace repomap
