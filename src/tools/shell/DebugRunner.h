#pragma once

// Debug-capture runner: executes a child process under the Win32 debugging
// API (DEBUG_ONLY_THIS_PROCESS) so that diagnostics invisible to a normal
// pipe-based runner become part of the tool result:
//
//   - OutputDebugString text (e.g. the D3D12/D3D11 debug-layer validation
//     messages, MFC/ATL traces) is captured into `debugOutput`.
//   - An unhandled (second-chance) exception is summarized into
//     `crashSummary` -- exception name/code, faulting instruction address,
//     owning module, and for access violations the read/write address.
//
// Without this, a crashing windowed program yields only a negative exit code
// (no stderr, no dialog after SetErrorMode), which forces blind guesswork.
// Intended for executables the agent built itself (inside the project root);
// system toolchains report errors fine through normal output.
//
// Only implemented on Windows. On other platforms (or if launching under the
// debugger fails) `launched` is false and the caller must fall back to its
// normal process runner.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class QProcessEnvironment;

namespace autocoder::shell {

struct DebugRunResult {
    bool launched = false;        // false -> use the fallback runner
    int exitCode = 0;
    bool cancelled = false;
    bool timedOut = false;
    bool crashed = false;         // process died on an unhandled exception
    std::string output;           // merged stdout+stderr (first maxOutputBytes)
    size_t droppedOutputBytes = 0;
    std::string debugOutput;      // OutputDebugString text (capped)
    size_t droppedDebugBytes = 0;
    std::string crashSummary;     // human-readable fatal-exception description
};

// `program` must be an absolute path to the executable. `cancelled` and
// `onOutput` may be null; `onOutput` receives incremental stdout/stderr
// chunks on the calling thread.
DebugRunResult runUnderDebugger(const std::filesystem::path& program,
                                const std::vector<std::string>& args,
                                const std::filesystem::path& cwd,
                                const QProcessEnvironment& env,
                                int timeoutMs,
                                int maxOutputBytes,
                                std::atomic<bool>* cancelled,
                                const std::function<void(const std::string&)>& onOutput);

}  // namespace autocoder::shell
