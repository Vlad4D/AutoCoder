#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>

// State shared across every tool call within one conversation.
// Owned by Conversation / AgentRunner; passed by reference to Tool::execute.
struct ToolContext {
    std::filesystem::path projectRoot;          // The folder the user opened.
    std::set<std::filesystem::path>* readSet = nullptr;  // Files that have been Read this conversation.
    std::atomic<bool>* cancelled = nullptr;     // Stop button was pressed.

    // Streaming sink for tools that produce incremental output (BashTool).
    // Set by AgentRunner per dispatch; safe to ignore if the tool returns its
    // result in one shot. The sink is invoked from the worker thread; the
    // implementation is responsible for thread-safe forwarding (e.g. via Qt
    // queued signals).
    std::function<void(const std::string&)> onOutput;

    // Default timeout for BashTool when the model doesn't specify one.
    // 0 means "use the tool's hardcoded default" (currently 120000 ms).
    int defaultBashTimeoutMs = 0;

    // Populated by AgentRunner before each turn: map of file path -> content before
    // the LLM modified it. WriteTool and EditTool populate this when they first
    // touch a file during the turn. Used by AgentRunner to build checkpoints.
    // Owned by AgentRunner; tools just fill it. May be nullptr (no checkpointing active).
    std::map<std::filesystem::path, std::string>* fileBeforeSnapshots = nullptr;

    // (Optional) Set by AgentRunner to allow AskUserTool to signal that a question
    // is pending user input. When non-null and non-empty after a tool dispatch,
    // AgentRunner pauses the loop and emits userInputRequired().
    std::string* pendingUserQuestion = nullptr;

    // (Optional) Set alongside pendingUserQuestion with predefined answer choices.
    // When non-null and non-empty, the UI renders clickable buttons instead of
    // a free-text input. Owned by AgentRunner.
    std::vector<std::string>* pendingUserOptions = nullptr;
};
