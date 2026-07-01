#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace SystemPrompt {

// Find the project guidance file AutoCoder should mention for this project,
// in priority order.
std::optional<std::filesystem::path> findProjectGuidanceFile(
    const std::filesystem::path& projectRoot);

// Build the system prompt that orients the model: project root, OS,
// available tools, and ground rules.
// If enableReasoning is false, the "Reasoning approach" section is omitted.
std::string build(const std::filesystem::path& projectRoot,
                  bool enableReasoning = true);

}  // namespace SystemPrompt
