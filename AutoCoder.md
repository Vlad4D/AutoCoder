# AutoCoder.md -- Project Knowledge (for LLM coding agents)

> **Convention:** This file uses only ASCII characters (no Unicode, no emoji, no
> box-drawing, no em-dashes) so it renders correctly in any terminal, editor,
> or LLM context regardless of encoding support. Use `--` for dashes, `->` for
> arrows, and plain ASCII `+--` / `|` for tree diagrams.

## Overview

AutoCoder is a **desktop LLM coding agent** built with Qt 6 / C++20. It lets users chat with **DeepSeek** (`deepseek-chat`) or **Claude** (`claude-sonnet-4-6`) and gives the LLM a set of filesystem tools (`bash`, `read`, `write`, `edit`, `glob`, `grep`) to explore and modify a project -- similar to Cursor / Copilot.

## Project Structure

```
C:/DEV/AutoCoder/
+-- CMakeLists.txt          # Top-level build (Qt6 + nlohmann/json vendored)
+-- CMakePresets.json       # VS 2022 & Ninja presets (Qt at C:/Qt/6.8.3/msvc2022_64)
+-- README.md               # User-facing documentation
+-- microsoft_store_description.md  # Microsoft Store listing copy
+-- .gitignore
+-- .p4ignore               # Perforce ignore rules
+-- .autocoderignore        # Files AutoCoder must never read into LLM context
+-- AutoCoder.md            # (This file -- knowledge base for the coding agent)
+-- build_update_and_restart.bat # Build, then kill + copy + launch new version
+-- bigtest/                # End-to-end agent scenarios (live API; see bigtest/README.md)
|   +-- common.ps1              # Harness: CLI runner w/ timeout, BMP verifier, metrics
|   +-- run_smoke.ps1           # L1: empty dir -> hello-world C + CMake (cheap)
|   +-- run_fixbug.ps1          # L2: fixture with planted bug; fix lib, not tests
|   +-- run_dx12.ps1            # L3: DX12 WARP triangle, self-verifies via out.bmp
|   +-- fixture_calc/           # L2 fixture project (off-by-one in calc.cpp)
|   +-- results/                # Per-run transcripts + metrics (VCS-ignored)
+-- cmake/
|   +-- stamp_build_config.cmake.in # Legacy build-stamp script (unreferenced;
|                                   # superseded by the build_date.cpp touch step)
+-- build/
|   +-- default/Debug/      # Binary output directory
|   +-- default/Debug2Release/  # Copy of EXE for hot-reload (avoids file locks)
+-- src/
|   +-- main.cpp            # QApplication entry point (GUI)
|   +-- build_date.h/.cpp   # Build timestamp via __DATE__/__TIME__ (touched pre-build)
|   +-- BuildConfig.h.in    # Legacy build-stamp template (unreferenced)
|   +-- agent/
|   |   +-- AgentRunner.h/.cpp    # Orchestrates LLM + tools + conversation loop
|   |   +-- Conversation.h/.cpp   # In-memory conversation state (json array)
|   |   +-- SystemPrompt.h/.cpp   # System prompt builder (repo map + guidance injected)
|   |   +-- RepoMap.h/.cpp        # Compact project map (files + top-level symbols)
|   +-- cli/
|   |   +-- cli_main.cpp          # Headless CLI test harness (no QThread)
|   |   +-- tests/                # Offline unit tests (autocoder_tests target)
|   |       +-- test_main.cpp         # Runner; suites: paths, store, edit_tools,
|   |       |                         #   policy, revert_e2e, highlight
|   |       +-- TestHarness.h         # Minimal check/failure-count harness
|   |       +-- Suites.h              # Suite function declarations
|   |       +-- test_paths.cpp, test_store.cpp, test_edit_tools.cpp,
|   |       |   test_policy.cpp, test_revert_e2e.cpp, test_highlight.cpp,
|   |       |   test_repomap.cpp, test_llm_cache.cpp, test_llm_body.cpp,
|   |       |   test_systemprompt.cpp, test_autoverify.cpp,
|   |       |   test_shell.cpp, test_image.cpp, test_debugrunner.cpp
|   |       +-- helpers/crashprobe.cpp  # Tiny exe (crash/ods/exit modes) for the debugrunner suite
|   +-- diagnostics/
|   |   +-- CrashHandler.h/.cpp   # DbgHelp crash handler (stack traces to %LOCALAPPDATA%)
|   +-- llm/
|   |   +-- LlmClient.h/.cpp     # Provider-aware HTTP streaming client (DeepSeek & Claude)
|   +-- persistence/
|   |   +-- ConversationStore.h/.cpp # On-disk save/load of conversations
|   |   +-- SecretStore.h/.cpp    # API keys in Windows Credential Manager (DPAPI)
|   +-- tools/
|   |   +-- Tool.h                # Abstract base: name(), schema(), execute()
|   |   +-- ToolContext.h         # Shared context (projectRoot, readSet, cancelled, onOutput)
|   |   +-- ToolRegistry.h/.cpp   # Registers & dispatches tool calls by name
|   |   +-- CodeStructure.h/.cpp  # Shared source-outline parser (outline/move/callee tools)
|   |   +-- PathUtil.h/.cpp       # Path validation / safety helpers
|   |   +-- IgnoreRules.h/.cpp    # gitignore-style ignore matching (see "Ignore rules")
|   |   +-- BashTool.h/.cpp       # Exposes bash tool using AutoCoder internal shell
|   |   +-- shell/
|   |   |   +-- InternalShell.h/.cpp # Deterministic shell parser, built-ins, QProcess runner
|   |   |   +-- DebugRunner.h/.cpp   # Win32 debug-capture runner (crash summary + OutputDebugString)
|   |   |   +-- ShellGrammar.h       # Shared command tokenizer/grammar
|   |   |   +-- CommandPolicy.h/.cpp # Safety gate: Allow/Prompt/HardBlock + user allowlist
|   |   +-- ReadTool.h/.cpp       # Read file with line numbers (offset/limit)
|   |   +-- WriteTool.h/.cpp      # Write file (enforces read-before-write)
|   |   +-- AppendTool.h/.cpp     # Append guarded chunks for large generated files
|   |   +-- EditTool.h/.cpp       # Exact string replace (unique or replace_all)
|   |   +-- CopyFileTool.h/.cpp   # Byte-for-byte in-project file copy
|   |   +-- GlobTool.h/.cpp       # fnmatch-style file search (newest-first)
|   |   +-- GrepTool.h/.cpp       # Regex file content search (rg or std::regex)
|   |   +-- AnalyzeImageTool.h/.cpp # Image analysis without vision (stats + ASCII preview)
|   |   +-- LSTool.h/.cpp         # Directory listing tool
|   |   +-- ReplaceLinesTool.h/.cpp # Line-range replacement (more robust than edit)
|   |   +-- ReadOutlineTool.h/.cpp   # Structural file outline (class/func hierarchy)
|   |   +-- MoveSpanTool.h/.cpp      # Move a line span between files/locations
|   |   +-- MoveFuncTool.h/.cpp      # Move a function/method by symbol name
|   |   +-- FindCallersTool.h/.cpp   # Find all call sites of a function
|   |   +-- FindCalleesTool.h/.cpp   # Find calls made inside a symbol/span
|   |   +-- AskUserTool.h/.cpp       # ask_user tool (pause + question to the user)
|   +-- ui/
|       +-- MainWindow.h/.cpp      # Main window (menu, splitter, toolbar, sidebar)
|       +-- AskUserWidget.h/.cpp   # Inline question bubble (options buttons or free-text input)
|       +-- CommandApprovalWidget.h/.cpp # Inline Allow/Deny prompt for gated bash commands
|       +-- ChatView.h/.cpp        # Scrollable conversation display
|       +-- MessageWidget.h/.cpp   # Single message bubble (assistant: Markdown; user: verbatim plain text)
|       +-- ToolCallWidget.h/.cpp  # Collapsible tool-call result display (highlighted diffs)
|       +-- CodeColorizer.h/.cpp   # Stateless per-line syntax colorizer (HTML or span ranges)
|       +-- InputBar.h/.cpp        # Bottom composer (Ctrl+Enter to send)
|       +-- SettingsDialog.h/.cpp  # Preferences dialog (API key, model, font size, etc.)
+-- third_party/
    +-- nlohmann/
        +-- json.hpp          # Vendored header-only JSON library (single header)
```

## Build System

- **CMake** >= 3.21 + **Visual Studio 2022** (or Ninja).
- Qt 6.5+ (currently 6.8.3 at `C:/Qt/6.8.3/msvc2022_64`).
- Standard: C++20, no extensions.
- `nlohmann/json` is vendored under `third_party/` as a header-only library.
- Four CMake targets:
  - `autocoder_core` -- static library (tools + llm + agent + persistence + diagnostics). Depends on `Qt6::Core`, `Qt6::Gui` (QImage for `analyze_image`; no widgets), `Qt6::Network`, and `nlohmann_json`. On Windows also links `DbgHelp` (crash handler) and `Advapi32` (Credential Manager).
  - `AutoCoder` -- GUI application (WIN32, links `autocoder_core` + `Qt6::Widgets`).
  - `autocoder_cli` -- console CLI executable (links `autocoder_core` only).
  - `autocoder_tests` -- console offline unit-test runner (links `autocoder_core`; also compiles `ui/CodeColorizer` directly for the highlight suite).

### Prerequisites

1. **Qt 6.5+** installed (currently 6.8.3 at `C:/Qt/6.8.3/msvc2022_64`).
2. **CMake** >= 3.21 on PATH (e.g. `C:\Program Files\CMake\bin\cmake.exe`).
3. **Visual Studio 2022** with the "Desktop development with C++" workload (provides MSVC and the Windows SDK).
4. **Git Bash** is not required for `BashTool`; AutoCoder uses its own internal shell. External programs such as `p4`, `cmake`, `npm`, and `python` still need to be installed separately when a project uses them.

### Configure

From any command prompt (or VS Developer Command Prompt):

```
cd C:\DEV\AutoCoder
cmake --preset default
```

This uses the `default` preset from `CMakePresets.json`:
- Generator: `Visual Studio 17 2022` (architecture x64).
- Binary output: `build/default/`.
- Qt path set via `CMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64`.

An alternative `ninja` preset is available for faster incremental builds:

```
cmake --preset ninja
cmake --build --preset ninja
```

Output goes to `build/ninja/`.

### Build

```
cmake --build --preset default
```

This builds the `Debug` configuration (defined in `buildPresets.default.configuration`).
Output files land in `build/default/Debug/`:

| File | Description |
|---|---|
| `AutoCoder.exe` | GUI application (run this) |
| `autocoder_cli.exe` | Headless CLI tool |
| `autocoder_tests.exe` | Offline unit-test runner |
| `autocoder_core.lib` | Static library (no direct execution) |

A `release` build preset also exists (`cmake --build --preset release`), which produces `build/default/Release/`.

### Tests

`autocoder_tests.exe` runs all offline unit tests -- no network, API key, or UI
required. It returns non-zero if any check fails:

```
cmake --build --preset default --target autocoder_tests
build\default\Debug\autocoder_tests.exe
```

Suites (one `test_*.cpp` each under `src/cli/tests/`): `paths` (PathUtil),
`store` (ConversationStore), `edit_tools` (edit/replace/move tools, including
the post-edit result snippets), `policy` (bash CommandPolicy classification +
IgnoreRules), `revert_e2e` (checkpoint revert end-to-end through AgentRunner),
`highlight` (CodeColorizer), `repomap` (system-prompt project map),
`llm_cache` (Anthropic cache_control breakpoint placement), `autoverify`
(check-command gate: skip conditions, takeover, 2-attempt cap, per-turn
reset, unparsable-tool-args diagnostics incl. the output-token truncation
hint), `shell` (wc -c, ls file sizes, deferred ERRORLEVEL/$? expansion,
cmd.exe-isms: del /Q and rmdir /s /q switches, single `&` separator,
one-line `if [not] exist <cmd>`, copy /Y), `image` (analyze_image: stats,
ASCII preview, corrupt-file diagnosis), `debugrunner` (debug-capture runner:
crash summary, OutputDebugString capture, internal-shell routing; uses the
`crashprobe` helper exe). The harness is a minimal check/failure counter in
`TestHarness.h`; suites are registered in `test_main.cpp`.

Test seams: `AgentRunner` exposes `beginTurnForTest` / `dispatchToolForTest` /
`endTurnForTest` / `runAutoVerifyForTest` / `messagesForTest` so turns can be
driven without network, and `LlmClient::addClaudeCacheBreakpoints` is a
public static (pure JSON transform) so caching is testable offline.

#### Whats happening under the hood

On Windows, CMake generates a Visual Studio solution (.sln) inside `build/default/`.
The build then uses MSBuild (the VS build engine). Qt's MOC/UIC/RCC are run
automatically via `CMAKE_AUTOMOC`, `CMAKE_AUTOUIC`, and `CMAKE_AUTORCC`.

After building `AutoCoder.exe`, a `POST_BUILD` step runs `windeployqt` to copy
required Qt DLLs (Core, Gui, Network, Widgets, Svg) and platform plugins into
the same output directory. The CLI shares this directory, so its DLLs are already
present.

#### Common build issues

| Symptom | Likely cause | Fix |
|---|---|---|
| `Could NOT find Qt6` | Qt not installed or wrong `CMAKE_PREFIX_PATH` | Verify Qt is at `C:/Qt/6.8.3/msvc2022_64` or update `CMakePresets.json` |
| `cl.exe` not found | VS environment not loaded | Run from a "Developer Command Prompt for VS 2022", or run `vcvars64.bat` first |
| `AutoCoder.exe` file-lock error on rebuild | A running instance holds the file handle | Close the app before rebuilding, or use `build_update_and_restart.bat` which runs from a copy |
| Linker errors about unresolved Qt symbols | MOC not running | Ensure `CMAKE_AUTOMOC` is ON (it is in `CMakeLists.txt`) |
| `windeployqt` fails | Qt DLLs already locked | Wait a moment after closing the app, then rebuild |

#### Building from WSL (Windows Subsystem for Linux)

When running cmake from WSL, the Windows `cmake.exe` must be invoked with
Windows-style paths (e.g. `C:\DEV\AutoCoder`), not WSL-style paths (e.g.
`/mnt/c/DEV/AutoCoder`). The easiest way is to `cd` into the source directory
and use `.` as the source path:

```bash
cd /mnt/c/DEV/AutoCoder
/mnt/c/Program\ Files/CMake/bin/cmake.exe \
    -S . \
    -B build/default \
    -G "Visual Studio 17 2022" \
    -A x64 \
    -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
```

Then build with:
```bash
cd /mnt/c/DEV/AutoCoder
/mnt/c/Program\ Files/CMake/bin/cmake.exe --build build/default --config Debug
```

Common pitfalls:
- **Wrong path format**: Passing `/mnt/c/DEV/AutoCoder` to Windows `cmake.exe`
  fails because it's a WSL path. Use `.` (relative) from within the source dir,
  or pass `C:\DEV\AutoCoder` (Windows path).
- **Backslash escaping**: In bash, backslashes must be escaped or the path
  quoted. Use forward slashes (`C:/DEV/AutoCoder`) or double-backslashes
  (`C:\\DEV\\AutoCoder`) when passing paths to Windows executables from bash.
- **cmake not on WSL $PATH**: The Windows cmake.exe is at
  `C:\Program Files\CMake\bin\cmake.exe`; reference it via the full path
  `/mnt/c/Program\ Files/CMake/bin/cmake.exe`.

### Build + restart in one step

`build_update_and_restart.bat` (project root) builds and restarts the app:

```
build_update_and_restart.bat
```

How it works:
1. Locates `cmake.exe` (common install paths, or uses PATH).
2. Loads the VS 2022 x64 environment if `cl.exe` isn't on PATH (tries `vcvars64.bat`
   from Community/Professional/Enterprise installs).
3. Builds: `cmake --build build\default --config Debug` (aborts on build failure).
4. Kills any running `AutoCoder.exe`.
5. Copies the fresh EXE (and PDB) from `build\default\Debug\` to
   `build\default\Debug2Release\` and launches the **copy** from there -- this
   keeps the original binary **unlocked** so the next incremental build never
   fails with a file-lock error.
6. Exits.

Run this to rebuild and replace the running instance in one step.

## Architecture

### Library layering

```
+-----------------------------------------+
|  AutoCoder  (GUI .exe)                  |
|  +-----------------------------------+  |
|  | autocoder_core  (static library)  |  |
|  |  tools/   llm/   agent/   persist |  |
|  +-----------------------------------+  |
|  Qt6::Widgets  (MainWindow, ChatView.)  |
+-----------------------------------------+

autocoder_cli  (console .exe)
  +-- autocoder_core (same library)
```

All file I/O, LLM communication, and conversation state live in the core library. The UI layer is pure presentation -- it never touches files or the network directly.

### Agent Loop (`AgentRunner`)

`AgentRunner` lives on a worker `QThread` (GUI) or on the main thread (CLI). The UI communicates with it via Qt signals/slots (queued connections).

```
User text -> submitUserMessage(QString)
  |
  +-- [0] Budget check (before sending):
  |       If over budget, try LLM compaction first (Tier 1):
  |         doCompact() -> split point found -> old part summarized -> retry
  |       Fallback: trimToBudget() drops oldest exchanges (Tier 2)
  |
  +-- [1] Send system prompt + conversation history + tool schemas -> LlmClient
  |
  +-- [2] LLM responds:
  |   +-- Text (content-only) -> emit assistantTextDelta -> assistantTextFinalized -> turnFinished
  |   +-- Tool call(s) -> emit toolCallStarted
  |       +-- Look up tool in ToolRegistry -> execute with ToolContext
  |       |   (tools may emit toolCallOutputDelta for streaming output)
  |       +-- Feed result back to LLM -> go to [2] (up to maxIter_ iterations)
  |       +-- emit toolCallFinished
  |
  +-- [3] When LLM returns plain text -> emit assistantTextDelta/Finalized -> turnFinished
  |
  +-- [4] Call cancel() at any time -> sets cancelled flag, aborts HTTP request
```

Key details:
- `maxIter_` defaults to 25 (configurable via `setMaxIterations()`).
- Tool schemas are generated by `SystemPrompt::buildToolsJson()` and included in every LLM request.
- The system prompt (built once per conversation by `SystemPrompt::build()`)
  also embeds the **project guidance file** (`AutoCoder.md`/`AUTOCODER.md`/
  `CLAUDE.md`/`AGENTS.md`, first found wins) directly, capped at 16 KB with a
  line-boundary truncation marker (`SystemPrompt::readGuidanceCapped()`), so
  the model gets project conventions without spending a tool call.
- The system prompt embeds a **repo map** from `repomap::build()` (src/agent/RepoMap.h/.cpp):
  one line per non-ignored file with top-level symbol names for C-family
  sources, capped at 24 KB (symbol detail is dropped first, then the file
  list is truncated with a marker). This removes most glob/ls/read_outline
  discovery round trips at the start of a task. The map honors the same
  exclusions as the file tools (pathutil::isExcludedDir + IgnoreRules) and
  stays byte-identical for an unchanged tree (provider-cache-friendly).
- **Auto-verify**: when a check command is configured (Settings) and a turn
  modified files, `maybeRunAutoVerify()` runs it via the internal shell
  before the turn closes. On failure the command output is appended to the
  conversation as an `[Auto-verify]` user message and sent back to the model
  to fix, at most 2 attempts per user turn. The check run is surfaced in the
  UI through the normal toolCallStarted/OutputDelta/Finished signals under
  the name `auto_verify`. The command is user-configured, so it bypasses the
  bash approval gate.
- The `ToolContext.cancelled` atomic pointer lets running tools detect cancellation and bail early.
- `ToolContext.onOutput` -- a `std::function` set per dispatch -- allows streaming tools (like `BashTool`) to emit incremental output back to the UI.
- Two tool calls can **pause** the cycle mid-batch and wait for the user:
  `ask_user` (resumed via `provideUserInput()`) and a `bash` command that fails
  the safety gate (resumed via `provideCommandDecision()` -- see "Bash command
  approval" below). Remaining tool calls of the batch are stashed and resumed
  after the user responds.

### AgentRunner Signals (for UI rendering)

| Signal | Purpose |
|---|---|
| `conversationCleared()` | New conversation started |
| `userMessageAdded(text, canRevert, turnIndex)` | User message bubble (with revert info) |
| `assistantTextDelta(fragment)` | Streaming text from assistant |
| `assistantTextFinalized()` | Assistant text block complete |
| `toolCallStarted(name, argsJson)` | Tool call dispatched |
| `toolCallOutputDelta(name, chunk)` | Incremental tool output |
| `toolCallFinished(name, result, ok)` | Tool call completed |
| `userInputRequired(toolCallId, toolName, question, options)` | `ask_user` pause; UI shows AskUserWidget, answers via `provideUserInput()` |
| `commandApprovalRequired(toolCallId, command, reason, explanation)` | Bash safety-gate pause; UI shows CommandApprovalWidget, answers via `provideCommandDecision()` |
| `commandApprovalResolved(command, reason, explanation, decision)` | Replay of a past approval decision (read-only widget) |
| `turnFinished()` | End of one user->assistant turn |
| `errorOccurred(detail)` | Fatal error in the loop |
| `streamRetrying(attempt, maxAttempts)` | Transient stream error; automatic retry in progress |
| `manyFilesChanged(fileCount)` | Warning that a turn touched many files |
| `contextStats(msgCount, approxTokens, bytesTotal)` | Token/budget info |
| `tokenUsageStats(cachedInput, uncachedInput, ...)` | Provider-reported token usage |
| `conversationTrimmed(bytesFreed)` | Exchanges dropped (Tier 2 fallback) |
| `compactStarted()` | LLM compaction beginning (Tier 1) |
| `compactFinished(bytesSaved)` | Manual compaction completed |
| `compactSummaryAppended(summary, bytesSaved)` | Compaction summary inserted into the conversation |
| `conversationsListed(ids, titles, updatedAt, currentId)` | Sidebar refresh |
| `conversationReplayStarted(totalMessages, keepBusy)` | Loading a saved conversation begins |
| `conversationReplayProgress(done, total)` | Replay progress for long conversations |
| `conversationReplayHistoryHidden(hiddenMessages)` | Older history withheld from initial replay (loaded on demand via `loadEarlierConversationHistory()`) |
| `conversationReplayFinished()` | Loading a saved conversation done |

### LlmClient (LLM wrapper)

Wraps DeepSeek's OpenAI-compatible `https://api.deepseek.com/v1/chat/completions`
and Claude's Anthropic Messages API at `https://api.anthropic.com/v1/messages`.
Two modes:
- **`sendOnce`** -- non-streaming, emits `responseReceived` with the full assistant message.
- **`sendStreaming`** -- SSE streaming, emits `tokenDelta`, `toolCallDelta(index, id, name, argumentsFragment)`, then `streamFinished` with the assembled message.

Claude requests are adapted from AutoCoder's internal OpenAI-style conversation
format into Anthropic `system`, `messages`, `tools`, and `tool_result` content
blocks. Claude responses are converted back into `assistant.tool_calls` so the
agent loop and persistence format stay provider-neutral.

Tool-call delta is accumulated in `streamCalls_` (map of `int -> {id, name, arguments}`)
and assembled when the provider's stream terminator is received.

**Prompt caching.** Every tool-loop iteration re-sends the entire conversation,
so `buildClaudeBody()` marks the stable prefix with `cache_control: ephemeral`
breakpoints (`addClaudeCacheBreakpoints()`): one on the system prompt block
(caches tools + system) and one on the last block of each of the final two
top-level messages (the trailing one caches the conversation up to this
request; the second-to-last still matches on the next, longer request). The
Anthropic API then reuses its server-side cache (5-minute TTL) instead of
re-processing the prefix -- cached input is ~90% cheaper and the
time-to-first-token drops sharply from the second iteration on. Prefixes
under the model's 1024-token minimum are silently not cached, so the
breakpoints are unconditional. DeepSeek needs none of this: its context
caching is automatic server-side. Cache hits are visible in the
`tokenUsageStats` signal.

### Tool Contract (`Tool` base class)

```cpp
struct ToolResult {
    bool ok;               // success / failure
    std::string content;   // Markdown-free text for the LLM
};

class Tool {
    virtual std::string name() const = 0;
    virtual nlohmann::json schema() const = 0;  // OpenAI function-calling schema
    virtual ToolResult execute(const nlohmann::json& args, ToolContext& ctx) = 0;
};
```

`ToolContext` carries:

| Field | Type | Purpose |
|---|---|---|
| `projectRoot` | `fs::path` | Open project directory; all file I/O constrained here |
| `readSet` | `set<fs::path>*` | Files Read() this conversation (enforced by Write/Edit) |
| `cancelled` | `atomic<bool>*` | Stop button pressed; tools should bail |
| `onOutput` | `function<void(const string&)>` | Streaming sink (BashTool reports output incrementally) |
| `defaultBashTimeoutMs` | `int` | Default timeout for BashTool (0 = tool's hardcoded 120000) |
| `fileBeforeSnapshots` | `map<fs::path, string>*` | (Optional) File-content snapshots for checkpoint revert. Populated by WriteTool/EditTool before modifying a file; collected by AgentRunner after the turn to build a checkpoint. |

### Bash command approval (safety gate)

Every `bash` tool call is classified by `shell::classifyCommand()`
(`src/tools/shell/CommandPolicy.h/.cpp`) before it runs. The classifier is a
pure function returning a `PolicyResult` with one of three verdicts:

| Verdict | Meaning |
|---|---|
| `Allow` | Known-safe, in-folder, reversible: run silently. |
| `Prompt` | Pause and ask the user. **Fail-closed default** for anything unknown, plus capability/destructive commands even when in-folder. |
| `HardBlock` | Never run, even with user consent (catastrophic/irreversible). |

Flow for a `Prompt` verdict:
1. `AgentRunner` pauses the tool cycle (stashing any remaining tool calls of
   the batch) and emits `commandApprovalRequired(toolCallId, command, reason,
   explanation)`. `explanation` is the LLM's stated purpose for the command.
2. The UI renders an inline `CommandApprovalWidget` with three actions:
   allow once, always allow this exact command, or deny.
3. The UI calls `provideCommandDecision(toolCallId, decision, persist)`.
   On allow the command executes and the cycle resumes; on deny a denial
   result (including the policy reason) is returned to the LLM.
4. When a saved conversation is replayed, past decisions are re-rendered as
   read-only widgets via `commandApprovalResolved(...)`.

User allowlist semantics: "always allow" stores the **full exact command
string** (never a prefix or segment), so approving `build.sh` can never
whitelist `build.sh && rm -rf ~`. Persistence is QSettings-backed:
allowlist entries under the `shell/allowlist` group (SHA-1 of the command as
the key, since commands may contain `/`), plus `shell/consentVersion` /
`shell/consentTimestamp` recording the user's one-time acceptance of the
autonomy disclaimer (`hasShellConsent()` / `recordShellConsent()`).

The classifier is covered by the offline `policy` test suite.

### Internal shell: cmd.exe compatibility forms

Models trained on Windows emit cmd.exe idioms; the internal shell accepts the
common ones so they do not burn agent iterations:

- Single `&` is the unconditional command separator (`del x 2>nul & app.exe`).
  Only a lone trailing `&` is rejected as background execution.
- `2>nul`, `>nul`, `2>&1` are accepted no-ops (stdout/stderr are merged).
- Single-letter slash switches (`/Q`, `/s`, `/Y`, ...) on `del`/`rm`/`erase`,
  `rmdir`/`rd`, `ls`/`dir`, `copy`/`cp`, `move`/`mv` are parsed as flags, not
  paths (previously `del /Q file` resolved `/Q` to drive root `Q:/` and hit
  the sandbox refusal). `del /q` maps to force (missing file does not fail
  the chain), `rmdir /s` is recursive (with full tree snapshot for revert).
- `if exist <path> <command>` and `if not exist <path> <command>` run any
  builtin or external command as the branch; the `(then) else (else)` form
  still works. A false condition with no branch is a successful no-op.
  CommandPolicy classifies the *branch* (worst of then/else), so
  `if exist x echo ok` stays silent while `if exist x del x` prompts as a
  delete.

### Debug-capture runner (`shell/DebugRunner`)

Crashing GUI programs print nothing: no stderr (windowed subsystem), no error
dialog (suppressed via `SetErrorMode` so tool calls cannot hang), and the
D3D12/Vulkan debug layers write to `OutputDebugString`, which a pipe never
sees. That left the agent blind-guessing on bare `-1073741819` exit codes.

`runExternal()` therefore routes executables **inside the project root** (the
agent's own builds) through `runUnderDebugger()`: the child is created with
`DEBUG_ONLY_THIS_PROCESS` and a debug-event loop captures what a plain runner
cannot:

- `OutputDebugString` text (ANSI + Unicode), appended to the tool result as a
  `[debug output (OutputDebugString)]` section (capped at 8 KB, keep-first).
- Unhandled exceptions, summarized as `[process crashed: access violation
  writing address 0x... (code 0xC0000005) at instruction 0x... in foo.exe]`.
  Second-chance records are used when available; fail-fast codes (0xC0000409)
  that never reach a second chance are recovered from the last first-chance
  record matched against the exit status.

Details: stdout/stderr go through an inherited pipe (merged, streamed via
`ctx.onOutput`, same caps as the QProcess path); `_NO_DEBUG_HEAP=1` is set so
the NT debug heap does not alter child behavior; a kill-on-close Job object
covers grandchildren on cancel/timeout; being a debuggee also suppresses WER
crash dialogs. System toolchains (cmake, cl, msbuild, ...) and stdin-fed
pipelines keep the plain QProcess runner, as does any launch where
`CreateProcessW` under the debugger fails (`launched == false` falls back).
Non-Windows builds compile a stub that always falls back. Covered by the
offline `debugrunner` suite via the `crashprobe` helper exe.

### Ignore rules (.autocoderignore)

`pathutil::IgnoreRules` (`src/tools/IgnoreRules.h/.cpp`) implements
gitignore-style matching for the read/context boundary: files the user does
not want shared are never read into context or sent to the LLM provider.
Rules are loaded per project from three sources, in order:

1. A built-in set of secret/credential defaults (e.g. `.env`, `*.pem`, keys).
2. `.gitignore` at the project root.
3. `.autocoderignore` at the project root (additive, same syntax).

Supported syntax is the common gitignore subset: comments (`#`), blank lines,
negation (`!`), directory-only (trailing `/`), anchoring (leading `/` or an
internal slash), and `*`, `?`, `**` globs. Order matters; the last matching
pattern wins. For `grep`, `ripgrepExcludeGlobs()` supplies `--glob` exclude
arguments for the patterns ripgrep does not already honor natively (ripgrep
respects `.gitignore` on its own, so only the built-in defaults and
`.autocoderignore` entries are passed).

### Conversation (in-memory state)

Wraps an OpenAI-style `messages[]` JSON array. Key methods:
- `setSystemPrompt(text)` -- inserts/replaces `role: system` at index 0.
- `addUser(text)` -- appends `role: user`.
- `addAssistant(messageObject)` -- appends a full assistant msg (may include `tool_calls`).
- `addToolResult(toolCallId, content)` -- appends `role: tool`.

Serializable via `toJson()` / `fromJson()`.

### Conversation Persistence

Conversations are stored as individual JSON files at:
```
%APPDATA%/AutoCoder/projects/<projectKey>/<uuid>.json
```

Where `projectKey` is a sanitized filesystem-friendly hash of the project root path.

`ConversationStore` handles:
- `list(projectRoot)` -- returns entries sorted by `updatedAtIso` descending.
- `load(id)` / `save(id, conv)` / `remove(id)` -- standard CRUD.
- `newId()` -- generates a UUID filename.

The title of each conversation is derived from the first user message (truncated).

### CLI (`autocoder_cli`)

A headless executable in `src/cli/cli_main.cpp`. Takes `--project <dir>` and a prompt string. Useful for scripting and debugging the agent loop without the GUI, and is the engine of the `bigtest/` harness.

**Flags:** `--provider deepseek|claude`, `--project <dir>`, `--model <id>`,
`--base-url <url>`, `--max-iter <n>`, `--check "<command>"` (auto-verify),
`--auto-approve`, `--metrics <file.json>`.

**Differences from GUI mode:**
- Uses `QCoreApplication` (no widget loop).
- `AgentRunner` runs on the **main thread** (no worker QThread) -- the process lives only as long as one turn.
- API key resolution: `DEEPSEEK_API_KEY` / `ANTHROPIC_API_KEY` from the
  environment if set (explicit override), otherwise the key saved by the GUI
  in the Windows Credential Manager (same `secretstore` entry,
  `AutoCoder/<provider>`). A machine where the app is configured needs no
  env setup for CLI runs.
- Prints streaming output and tool calls to stdout/stderr in real-time, plus
  a `[usage]` token summary at turn end.
- **Never hangs on the pause points** (required for unattended runs):
  `ask_user` is answered with a canned "no user available -- use your best
  judgment" message, and a bash command that fails the safety gate is allowed
  once under `--auto-approve` (intended for sandboxed test directories) or
  denied otherwise. HardBlock commands stay blocked regardless.
- `--metrics` writes a JSON summary (status, wall ms, LLM request count,
  tool-call/error counts by name, approval/ask_user counts, cached vs
  uncached vs output tokens) for the bigtest harness.

### End-to-end bigtest harness

`bigtest/` contains scripted live-API scenarios (NOT part of the offline
`autocoder_tests`): `run_smoke.ps1` (hello-world from empty dir),
`run_fixbug.ps1` (fix a planted bug in `fixture_calc/` without touching its
tests -- hash-verified), and `run_dx12.ps1` (flagship: DX12 WARP triangle
that self-verifies by writing `out.bmp`, checked for actual pixel content).
Each run sandboxes the project under `%TEMP%`, applies a hard timeout, and
verifies artifacts programmatically; transcripts and `metrics.json` land in
`bigtest/results/`. See `bigtest/README.md` for usage and how to read the
metrics as an efficiency benchmark.

```sh
set DEEPSEEK_API_KEY=sk-...
./build/default/Debug/autocoder_cli.exe \
    --project C:/DEV/SomeProject \
    "List the C++ source files and tell me what AgentRunner does."

set ANTHROPIC_API_KEY=sk-ant-...
./build/default/Debug/autocoder_cli.exe \
    --provider claude \
    --project C:/DEV/SomeProject \
    "List the C++ source files and tell me what AgentRunner does."
```

## Settings

All settings are persisted via `QSettings` (Windows registry under `HKCU\Software\AutoCoder\AutoCoder`), **except API keys**, which are stored in the **Windows Credential Manager** (generic credentials, per-user DPAPI encryption) via `secretstore` (`src/persistence/SecretStore.h/.cpp`). Key lookup order: Credential Manager, then legacy plaintext QSettings (migrated into the secure store and purged from the registry on first read), then the environment variable.

| Display name | QSettings key | Default | Notes |
|---|---|---|---|
| Provider | `llm/provider` | `deepseek` | `deepseek` or `claude`; passed to `AgentRunner::setProvider()` |
| DeepSeek API key | (Credential Manager; legacy `deepseek/apiKey` migrated) | empty (env fallback `DEEPSEEK_API_KEY`) | Read by `AgentRunner::setApiKey()` |
| Claude API key | (Credential Manager; legacy `claude/apiKey` migrated) | empty (env fallback `ANTHROPIC_API_KEY`) | Read by `AgentRunner::setApiKey()` when provider is Claude |
| DeepSeek model | `deepseek/model` | `deepseek-chat` | Passed to `LlmClient::setModel()` |
| Claude model | `claude/model` | `claude-sonnet-4-6` | Passed to `LlmClient::setModel()` |
| DeepSeek base URL | `deepseek/baseUrl` | `https://api.deepseek.com` | Passed to `LlmClient::setBaseUrl()` |
| Claude base URL | `claude/baseUrl` | `https://api.anthropic.com` | Passed to `LlmClient::setBaseUrl()` |
| Max agent iterations | `agent/maxIterations` | `25` | Loop limit in `AgentRunner` |
| Default bash timeout (ms) | `agent/bashTimeoutMs` | `120000` | Stored in `ToolContext::defaultBashTimeoutMs` |
| Check command (auto-verify) | `agent/checkCommand` | empty (disabled) | Run after every turn that modified files; failures are fed back to the model (max 2 fix attempts per turn). Passed to `AgentRunner::setCheckCommand()` |
| DeepSeek max context tokens | `agent/maxContextTokens` | `1048576` | Max tokens the conversation may use before automatic trimming kicks in. Set via `AgentRunner::setMaxContextTokens()`. |
| Claude max context tokens | `claude/maxContextTokens` | `500000` | Lower than DeepSeek's 1M default to reduce Anthropic input-token-per-minute rate-limit hits during repeated tool-loop sends; user-editable up to 1M. |
| DeepSeek reserved send tokens | `agent/sendTokens` | `16384` | Headroom kept for the new user message + assistant reply (deducted from `maxContextTokens` before capping). Set via `AgentRunner::setSendTokens()`; also forwarded to `LlmClient::setMaxTokens()`, which clamps per model (`deepseek-chat` 8K output cap, `deepseek-reasoner` 64K, generic 128K). |
| Claude reserved send tokens | `claude/sendTokens` | `16384` | Same meaning as DeepSeek reserved send tokens, stored separately per provider. |
| DeepSeek temperature | `deepseek/temperature` | `0.0` | Sampling temperature sent with every request. DeepSeek's API default is 1.0, which is poor for coding; its docs recommend 0.0 for coding/math. Spin box minimum (`-0.1`, shown as "provider default") omits the field. Suppressed automatically for `deepseek-reasoner` models, which reject sampling params. Set via `AgentRunner::setTemperature()`. |
| Claude temperature | `claude/temperature` | provider default (`-0.1`) | Same control for Claude (clamped to Anthropic's 0..1 range when set). Left at provider default because Claude's agentic default behaves well. |
| Font size | `ui/fontSize` | `12` | Applied via `QApplication::setFont()` in `MainWindow::applyFontSize()` |
| (not in dialog) | `shell/allowlist/<sha1>` | empty | Full exact bash commands the user chose "Always allow" for (see "Bash command approval") |
| (not in dialog) | `shell/consentVersion`, `shell/consentTimestamp` | unset | One-time shell autonomy consent (app version + UTC ISO-8601 timestamp) |

The Claude model combo box lists the main known Claude API model IDs:
`claude-opus-4-8`, `claude-opus-4-7`, `claude-opus-4-6`,
`claude-sonnet-4-6`, `claude-sonnet-4-5`,
`claude-sonnet-4-5-20250929`, `claude-haiku-4-5`, and
`claude-haiku-4-5-20251001`. The combo box is editable, so users can enter any
other model ID their Anthropic account supports.

### How token capping works (automatic trimming + LLM compaction)

Instead of rejecting a message when the conversation grows too large, `AgentRunner`
uses a **two-tier strategy** to stay within budget:

1. Before each LLM request, `AgentRunner::sendToLlm()` computes the token budget:
   `budgetBytes = (maxContextTokens_ - sendTokens_) * 4`
2. It deducts the tool-schema JSON size. If the conversation is still over budget,
   it first tries **LLM-based compaction** (Tier 1):
   - If there are at least 2 user exchanges and compaction hasn't been tried this turn,
     `doCompact()` is called.
   - `doCompact()` walks exchanges from newest to oldest, finding the split point
     where the recent messages (kept part) fit within budget.
   - Only the **old part** (messages before the split) is sent to the LLM for
     summarization as a detailed handoff: user goal, constraints, exact files,
     implementation details, decisions, command/test outcomes, known risks,
     current state, and next steps.
   - The conversation is rebuilt as: `[system prompt] + [summary as user message] + [kept part]`.
   - The LLM request is retried on the compacted conversation.
3. If compaction was already attempted this turn or there aren't enough exchanges,
   **exchange dropping** is used as a fallback (Tier 2):
   - `Conversation::trimToBudget()` identifies whole "exchanges" (user message + assistant
     response + any tool-call results that follow) starting from index 1 (after the system
     prompt) and removes the oldest ones until the serialized size fits the budget.
4. The system prompt (index 0) is always preserved.
5. At least `minMessages` messages (default 3: system + user + assistant) are
   always kept, even if still over budget.
6. The UI receives a `conversationTrimmed(bytesFreed)` signal when exchanges are dropped,
   or a `compactStarted()` / `conversationTrimmed` sequence when LLM compaction runs.
7. If the conversation is still over budget after both tiers, a soft warning is emitted
   but the request is still attempted. The API will return a 400 error if truly over limit.

A **manual compact button** is also available in the UI (via `InputBar`). It uses the
exact same `doCompact()` algorithm with `approxBytes() / 2` as the budget, so it
compacts roughly the oldest half of the conversation, keeping the most recent
exchanges intact. The button emits `compactStarted()` and `compactFinished(bytesSaved)`
signals for status-bar feedback.

This replaces the old hard guardrail which simply rejected the user's message
with an error dialog. The user can now continue chatting indefinitely -- old
context is silently compacted or dropped, keeping the conversation responsive.

## Checkpoints and Revert

Before every user message is sent to the LLM, a **checkpoint** is automatically
saved. Each checkpoint captures:

1. The conversation state before that user message is appended.
2. The original content of every file modified by the resulting turn. The
   checkpoint file is created before the model runs, then updated with file
   snapshots as tools modify files and the turn finishes.

### On-disk layout

Conversations are stored by `ConversationStore`, partitioned by project root.
The base directory is `QStandardPaths::AppDataLocation`; on Windows for the
desktop app, with organization and application both set to `AutoCoder`, this
normally resolves to `%APPDATA%/AutoCoder/AutoCoder`. If Qt cannot provide an
app-data path, the fallback is `$HOME/.autocoder/projects`.

`<projectKey>` is the first 32 hex characters of the SHA-1 hash of the weakly
canonical project root path. Older 16-character project-key directories are
migrated or reused by `ConversationStore::projectDir()`.

```
<Qt AppDataLocation>/projects/<projectKey>/
+-- <convId>.json                       # One JSON file per saved conversation
+-- checkpoints/
    +-- <convId>/
        +-- 0.json  (checkpoint before turn 0)
        +-- 1.json  (checkpoint before turn 1)
        +-- ...
```

Typical Windows desktop path:

```
%APPDATA%/AutoCoder/AutoCoder/projects/<projectKey>/
```

The conversation file is `Conversation::toJson()` plus metadata from
`ConversationStore::save()`:

- `project_root` -- project path recorded in the conversation
- `model` -- provider model id used for the conversation
- `messages` -- OpenAI-style message array (`system`, `user`, `assistant`,
  `tool`)
- `approvals` -- command approval decisions keyed by tool-call id; this is
  persisted for UI replay but is not sent to the LLM
- `id`, `title`, `created_at`, `updated_at` -- save/list metadata

New empty conversations are displayed in the sidebar but are not written to
disk until they contain more than the system prompt. `AgentRunner::saveCurrent()`
skips conversations with `messages().size() <= 1`.

Each checkpoint JSON file contains `turn_index`, `user_message`, `messages`
(the pre-user-message conversation snapshot), and `file_snapshots`. File
snapshots are stored as an array of objects:

```
{
  "path": "relative/or/absolute/path/as-recorded",
  "content_encoding": "base64",
  "content_b64": "..."
}
```

Older checkpoint files may contain a plaintext `content` field; loading keeps
backward compatibility for those files.

### Revert button

Each user message bubble in the chat view has a small **"Revert to here"**
button. Clicking it:

1. Restores every modified file to its content at that checkpoint (files
   created after the checkpoint are deleted; files edited after the
   checkpoint are reverted to their pre-edit state).
2. Restores the conversation messages to exactly the state they were in
   before that user message was sent.
3. Removes all checkpoints with a turn index >= the reverted-to checkpoint
   (they are no longer valid).
4. Replays the conversation in the UI so the user sees the restored state.

A confirmation dialog is shown first, warning that subsequent messages and
file changes will be lost.

### How it works

- `AgentRunner::submitUserMessage()` calls `takeCheckpoint()` before sending
  the message to the LLM.
- `takeCheckpoint()` serialises `conv_.messages()` and collects any file
  snapshots that were recorded by `WriteTool` / `EditTool` during the
  previous turn.
- `WriteTool` and `EditTool` check `ToolContext::fileBeforeSnapshots` -- if
  set, they snapshot the old content of a file the first time they touch it
  during a turn, before any modifications.
- `AgentRunner::revertToCheckpoint()` loads the checkpoint, restores files,
  rebuilds the conversation, and cleans up stale checkpoints.

### Limitations

- Checkpoints are not automatically cleaned up unless the user reverts.
  Deleting a conversation from the sidebar removes its checkpoints.
- File snapshots store the complete original content of each modified file.
  For very large files this could consume significant disk space over many
  turns.
- LLM compaction (Tier 1) makes an extra API call to summarize old context.
  This adds latency and costs tokens (though far fewer than the full conversation).
  If the API is unavailable or returns an empty summary, compaction falls back
  to exchange dropping (Tier 2).
- Exchange dropping (Tier 2) removes whole exchanges (user + assistant + tool
  results) from the middle of the conversation. This is coarse -- a smarter
  strategy could selectively prune long tool outputs (e.g. truncating long
  `grep` or `read` results) rather than dropping the entire exchange.
- Checkpoints store the full original content of each modified file, not diffs.
  For projects with very large files modified across many turns, this can consume
  significant disk space.

## Key Dependencies

| Dependency | Where | Notes |
|---|---|---|
| Qt 6.5+ (Core, Gui, Network, Widgets) | System | Find via CMake's `find_package(Qt6 ..)` |
| nlohmann/json | `third_party/nlohmann/json.hpp` | Vendored, header-only. Included as `nlohmann_json` interface library. |
| DeepSeek API | Network (`api.deepseek.com`) | OpenAI-compatible chat-completions provider. |
| Claude API | Network (`api.anthropic.com`) | Anthropic Messages API provider. |

## Limitations (v1)

- No MCP server support or embeddings/RAG.
- No incremental file-tree sidebar (conversations list only).
- Exchange dropping (Tier 2) removes whole exchanges (user + assistant + tool
  results) from the middle of the conversation. This is coarse -- a smarter
  strategy could selectively prune long tool outputs (e.g. truncating long
  `grep` or `read` results) rather than dropping the entire exchange.
- Checkpoints store the full original content of each modified file, not diffs.
  For projects with very large files modified across many turns, this can consume
  significant disk space.

## Tools Available to the LLM

All file tools are constrained to the project root and honor the ignore rules
(`.autocoderignore` + `.gitignore` + built-in secret defaults -- see "Ignore
rules"). Bash commands additionally pass through the safety gate (see "Bash
command approval").

| Tool | Purpose |
|------|---------|
| `bash` | Run commands through AutoCoder's internal cross-platform shell in the project root. Configurable timeout via `defaultBashTimeoutMs`. Supports streaming output via `ToolContext::onOutput`. Supports simple commands, quotes, `$VAR` / `${VAR}` / `%VAR%` (plus `%ERRORLEVEL%` / `$?`, resolved at execution time to the previous segment's exit code), `;`, `&&`, `||`, single `&` as the cmd.exe-style unconditional separator, `cd`, `if [not] exist <path> <command> [else <command>]`, simple internal pipelines such as `wc -l ... | sort -rn | head -40`, and common built-ins: `pwd`, `echo`, `ls`/`dir` (single-file targets include the byte size), `cat`/`type`, `mkdir`, `rm`/`del`/`erase`, `rmdir`/`rd` (`/s` recursive), `cp`/`copy`, `mv`/`move`, `touch`, `which`/`where`, `wc` (`-l` lines, `-c` bytes, `-lc` both), `sort`, `head`, `true`, `false`, and `find_large_files`/`large_files` for recursive size scans with line counts. Single-letter cmd.exe slash switches (`/Q`, `/Y`, ...) are parsed as flags, not paths. Non-built-ins such as `p4`, `cmake`, `npm`, `python`, and test runners are launched directly with `QProcess`, not through Git Bash, WSL, cmd.exe, or PowerShell -- except executables inside the project root, which run under the debug-capture runner (see "Debug-capture runner") so crashes and `OutputDebugString` text are reported. Output already combines stdout and stderr; common merge suffixes such as `2>&1` and `2>nul` are accepted as no-ops. Unsupported in v1: arbitrary external pipelines, file redirection, heredocs, general loops, subshells, and command substitution. Use dedicated tools (`read`, `write`, `append`, `edit`, `grep`, `glob`) for those workflows. |
| `read` | Read a file with line numbers, supports `offset`/`limit` for slicing. Records the path in `readSet` (used by write/edit for safety). **Strips trailing `\r`** from each line in the display so the LLM sees clean LF-only output regardless of the file's actual line endings. |
| `write` | Write a file (creates parent dirs). **Refuses overwrite** if the file wasn't `read` first in this conversation. Content is written as-is (no line-ending normalisation). For large generated files, use `write` for the first chunk only. |
| `append` | Append content to the end of a file. Use after `write` to finish large generated files that are too big for one tool-call argument. Supports `expected_offset` so chunks fail safely if repeated or out of order. |
| `edit` | Exact string replace; requires uniqueness unless `replace_all: true`. **Refuses** to edit files not in `readSet`. **CRLF/LF tolerant**: both `old_string` and the file content are normalised to LF-only for matching, so mismatches between what the LLM sees (from `read`) and the on-disk encoding do not cause failures. The replacement is written back using the original line-ending style detected in the file. The result echoes a numbered snippet of the post-edit file around the change (3 context lines, capped at 40 lines) so the model can verify the outcome without re-reading. |
| `copy_file` | Copy a file byte-for-byte between two in-project paths. Avoids routing large file contents through the model (which breaks `write` for big files). Clears the destination's read-only attribute first. |
| `glob` | `fnmatch`-style pattern search (e.g. `**/*.cpp`); sorted by modification time (newest first). Skips `.git`, `node_modules`, `build`, `.vscode`, etc. Uses UTF-8 path conversion on Windows so Unicode paths do not depend on the active console code page, and accepts `/mnt/c/...`, `/c/...`, and `/cygdrive/c/...` drive aliases. |
| `ls` | List directory contents with type indicators (`[DIR]`, `[FILE]`), size, and modification time. Supports `recursive` and `pattern` (glob filter) options. Directories listed first, then alphabetically. Uses UTF-8 path conversion on Windows so Unicode paths do not depend on the active console code page, and accepts `/mnt/c/...`, `/c/...`, and `/cygdrive/c/...` drive aliases. |
| `replace_lines` | Replace a contiguous range of lines by line number (1-based, inclusive). More robust than `edit` when exact indentation is uncertain. Supports insertion via `end_line=start_line-1`. CRLF/LF tolerant. The result embeds old/new content blocks (rendered as a diff by the UI) and reports post-edit numbering: the new content's line span and how lines below the edit shifted, so chained replace_lines calls do not need a re-read. |
| `read_outline` | Show a structural outline of a source file: classes, structs, enums, functions, methods, namespaces, typedefs, and macros with line spans, nesting levels, names, and stable span hashes. Marks the file as read for safe structural edits. Saves tokens by avoiding a full file read. |
| `move_span` | Move a line span from one file/location to another without returning the moved text to the LLM. Supports `expected_hash` verification and `dry_run`. Requires source and target files to have been read or outlined first. |
| `move_func` | Move a function/method by symbol name using outline spans, optionally inserting after another symbol. Delegates the actual edit to `move_span`, so large function bodies do not need to be sent through the conversation. |
| `find_callers` | Find all (file:line) call sites of a function/method in the project. Uses regex search that filters out most definitions. Supports qualified calls (`obj.funcName(`, `ns::funcName(`). Supports optional `glob` filter and `path` override. |
| `find_callees` | Find likely function/method calls inside a symbol or explicit line span. Heuristic scan intended for refactoring impact checks. |
| `grep` | Regex content search across files; uses `rg` (ripgrep) if on PATH, else `std::regex` fallback. Supports `glob` filter (e.g. `**/*.cpp`), `output_mode` (`content`, `files_with_matches`, `count`). |
| `analyze_image` | Image analysis without vision: parse validity (with a header hex dump when corrupt -- catches struct-padding bugs in hand-written headers), dimensions/format/depth/alpha, color statistics (distinct colors, uniformity flagged loudly, dominant colors with percentages, mean RGB), and a coarse ASCII luminance preview (`ascii_width` 16-120, default 48; terminal aspect corrected) that makes shapes perceivable as text. Intended for verifying generated images (render output, screenshots, icons) -- a program can exit 0 and still produce an empty or corrupt image. Honors ignore rules. |
| `ask_user` | Ask the user a question and pause for their response. The tool returns immediately with a placeholder; the user's answer is injected back into the conversation to resume the loop. Useful for clarifications or confirmation before destructive operations. Supports an optional `options: string[]` parameter -- when provided, the UI renders clickable choice buttons instead of a free-text input. |
