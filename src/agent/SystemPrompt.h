#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace SystemPrompt {

// Find the project guidance file AutoCoder should embed for this project,
// in priority order.
std::optional<std::filesystem::path> findProjectGuidanceFile(
    const std::filesystem::path& projectRoot);

// Read a guidance file for embedding in the system prompt. Content larger
// than maxBytes is cut at a line boundary and a "[guidance truncated -- read
// <name> for the rest]" marker is appended so the model knows to read the
// file itself. Returns an empty string if the file cannot be read.
// Exposed for the offline tests.
std::string readGuidanceCapped(const std::filesystem::path& file,
                               std::size_t maxBytes = 16 * 1024);

// Build the system prompt that orients the model: project root, OS,
// available tools, and ground rules.
// If enableReasoning is false, the "Reasoning approach" section is omitted.
std::string build(const std::filesystem::path& projectRoot,
                  bool enableReasoning = true);

}  // namespace SystemPrompt
