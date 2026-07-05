# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

AutoCoder is a Qt 6 / C++20 **desktop LLM coding agent** for Windows. The user chats with **DeepSeek** (`deepseek-chat`) or **Claude** (`claude-sonnet-4-6`); the agent drives a set of filesystem tools (`bash`, `read`, `write`, `edit`, `glob`, `grep`, plus structural/refactor tools) to explore and modify a target project — Cursor/Copilot style.

`AutoCoder.md` in the repo root is the canonical, deep knowledge base (architecture, build internals, agent loop, tool contract, troubleshooting). Read it for anything beyond what's below. Keep it ASCII-only (it documents that convention for itself).

## Repo gotchas (read before editing)

- **The whole `src/` tree and `CMakeLists.txt` are marked read-only** (Windows read-only attribute, likely a sync/backup tool). Edits fail with EPERM until you clear the bit: `Set-ItemProperty <file> -Name IsReadOnly -Value $false`. Clear per-file as needed before editing.
- **MSVC build does not pass `/utf-8`.** BOM-less sources decode as ANSI. Non-ASCII string literals must use `\uXXXX` escapes inside `QStringLiteral` (existing convention), or the file must be saved UTF-8 **with BOM**. Never add bare non-ASCII characters to a BOM-less source file.

## Build / test / run

Configure once, then build (Debug is the default config):

```sh
cmake --preset default            # VS 2022 generator, output -> build/default/
cmake --build --preset default    # Debug; use --preset release for Release
```

`ninja` preset exists for faster incremental builds (`cmake --preset ninja && cmake --build --preset ninja`). Qt is expected at `C:/Qt/6.8.3/msvc2022_64`; override with `-DCMAKE_PREFIX_PATH=...`. `nlohmann/json` is vendored under `third_party/` — no package manager.

Run the GUI: `./build/default/Debug/AutoCoder.exe`. For build-then-restart-in-one-step (handles the EXE file-lock by launching a copy from `Debug2Release/`), use `build_update_and_restart.bat`.

**Offline unit tests** (no network/API key/UI needed; returns non-zero on failure):

```sh
cmake --build --preset default --target autocoder_tests
build\default\Debug\autocoder_tests.exe          # all suites
build\default\Debug\autocoder_tests.exe paths    # single suite by name
```

Suites live in `src/cli/tests/` as `test_*.cpp` (paths, store, edit_tools, policy, revert_e2e, highlight, repomap, llm_cache, llm_body, systemprompt, autoverify, shell, image, debugrunner), registered in `test_main.cpp` with the minimal harness in `TestHarness.h`. Add a new suite by adding its `.cpp` to the `autocoder_tests` target in `CMakeLists.txt` and registering it in `test_main.cpp`.

**Headless CLI** for scripting/debugging the agent against a live API:

```sh
DEEPSEEK_API_KEY=sk-... ./build/default/Debug/autocoder_cli.exe --project C:/DEV/Foo "your prompt"
ANTHROPIC_API_KEY=sk-ant-... ./build/default/Debug/autocoder_cli.exe --provider claude --project C:/DEV/Foo "your prompt"
```

`bigtest/` holds live-API end-to-end scenarios (PowerShell harness; see `bigtest/README.md`).

## Architecture (the big picture)

Four CMake targets: **`autocoder_core`** (static lib: all tools + llm + agent + persistence + diagnostics; no Widgets), **`AutoCoder`** (GUI exe, core + Qt6::Widgets), **`autocoder_cli`** (console exe, core only), **`autocoder_tests`** (offline test runner).

**Strict layering:** all file I/O, LLM communication, and conversation state live in `autocoder_core`. The `ui/` layer is pure presentation — it never touches files or the network directly. It talks to the agent exclusively over Qt signals/slots.

**`AgentRunner` (`src/agent/`) is the orchestrator.** It owns the `Conversation`, the `LlmClient`, the `ToolRegistry`, and the `ConversationStore`, and runs the LLM tool-use loop. It lives on a worker `QThread` in the GUI (main thread in the CLI); the UI drives it through queued signal/slot connections (`submitUserMessage`, `cancel`, `provideUserInput`, etc.) and renders from its emitted signals (`assistantTextDelta`, `toolCallStarted/Finished`, `userInputRequired`, ...). When touching the agent loop, preserve this thread model — public API is slots or thread-affinity-free atomics.

Key loop behaviors, all in `AgentRunner`:
- **Checkpoint/revert:** before each user message a checkpoint snapshots conversation state + pre-images of every file the agent later modifies; `revertToCheckpoint()` restores both. The revert snapshot-merge logic is static and unit-tested.
- **Budget management:** before sending, if over the token budget it first tries LLM-driven compaction (summarize old messages), falling back to trimming the oldest exchanges.
- **Pausing tool calls:** `ask_user` and a `bash` command that fails the safety gate both pause the batch mid-cycle and wait for the user (`provideUserInput` / `provideCommandDecision`); remaining calls in the batch are stashed and resumed.
- **Auto-verify:** an optional user-configured check command runs after any turn that modified files; on failure its output is fed back to the model to fix (max 2 attempts/turn).

**`SystemPrompt` (`src/agent/`)** builds the system prompt once per conversation and embeds a **repo map** (`RepoMap`): one line per non-ignored file with top-level symbols for C-family sources, capped at 24 KB, byte-stable for an unchanged tree (provider-cache-friendly). It also generates the tool JSON schemas sent on every request.

**`LlmClient` (`src/llm/`)** wraps both providers. AutoCoder's internal conversation format is OpenAI-style; for Claude, requests are adapted to the Anthropic Messages API and responses converted back to `assistant.tool_calls` so the agent loop and persistence stay provider-neutral. It supports streaming (SSE) and non-streaming. For Claude it inserts `cache_control` breakpoints on the stable prefix (`addClaudeCacheBreakpoints`, a pure static — unit-tested); DeepSeek caches server-side automatically.

**Tools (`src/tools/`)** all derive from `Tool` (`name()`, `schema()`, `execute()`), are registered in `ToolRegistry`, and run against a shared `ToolContext` (project root, read-set, `cancelled` atomic, `onOutput` streaming callback). `bash` does **not** shell out to a system shell — it runs through AutoCoder's own deterministic cross-platform shell in `src/tools/shell/` (`InternalShell` + `ShellGrammar` + `CommandPolicy` safety gate). Beyond the basic file tools there are structural/refactor tools (`read_outline`, `move_span`, `move_func`, `find_callers`, `find_callees`) sharing `CodeStructure`, plus `analyze_image`, `ls`, `copy_file`, `replace_lines`, `append`, and `ask_user`.

**Test seams:** `AgentRunner` exposes `beginTurnForTest`/`dispatchToolForTest`/`endTurnForTest`/`runAutoVerifyForTest`/`messagesForTest` to drive a turn without the network — prefer extending these over adding network mocks.

## Persistence & ignore rules

- Conversations: `%APPDATA%/AutoCoder/projects/<projectKey>/<uuid>.json` (per-project bucket).
- API keys: Windows Credential Manager via `SecretStore` (DPAPI); other settings via `QSettings` (registry). Or env: `DEEPSEEK_API_KEY` / `ANTHROPIC_API_KEY`.
- Crash logs: `%LOCALAPPDATA%/AutoCoder/<app>/logs/crashes.log` (DbgHelp handler in `src/diagnostics/`).
- `.autocoderignore` (gitignore syntax) lists files the agent must never read into LLM context; it's additive to `.gitignore` and a built-in secret/credential pattern set, enforced by `IgnoreRules`. The file tools honor the same exclusions as the repo map.
