#pragma once

#include <filesystem>
#include <string>

#include "ToolContext.h"

namespace pathutil {

// Resolve `arg` against `ctx.projectRoot` if relative. Does NOT enforce a
// sandbox: tools may access absolute paths the user explicitly references.
std::filesystem::path resolve(const std::string& arg, const ToolContext& ctx);

// Resolve + enforce containment. Returns an empty path if the resolved path
// falls outside `ctx.projectRoot`. All file-manipulation tools should use this.
std::filesystem::path resolveSafely(const std::string& arg, const ToolContext& ctx);

// Component-wise containment check: returns true if `p` is inside or equal to `root`.
// Both paths are weakly_canonical before comparison (resolves symlinks).
// Unlike a string-prefix check, this correctly rejects sibling directories
// whose names happen to share the root's prefix.
bool containedIn(const std::filesystem::path& p, const std::filesystem::path& root);

// Convert between tool-facing UTF-8 strings and native filesystem paths.
// On Windows this avoids the active console code page entirely.
// Also accepts file:// URLs and WSL/MSYS/Cygwin drive aliases such as
// /mnt/c/x, /c/x, and /cygdrive/c/x by converting them to native paths.
std::filesystem::path fromUtf8(const std::string& s);
std::string toUtf8(const std::filesystem::path& path);

// Translate a glob pattern (`**/*.cpp`, `src/*.h`) to an ECMAScript regex
// matching the relative path with forward-slash separators.
std::string globToRegex(const std::string& pattern);

// True if `entry` is in a directory that should be skipped during recursive walks
// (build/.git/node_modules/etc.).
bool isExcludedDir(const std::string& dirName);

// Remove the read-only filesystem attribute from `path` (Windows only).
// Does nothing on other platforms or if the file is not read-only.
// Only acts if the file is inside `projectRoot` -- files outside are left alone.
void makeWritable(const std::filesystem::path& path,
                  const std::filesystem::path& projectRoot);

}  // namespace pathutil
