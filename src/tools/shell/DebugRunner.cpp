#include "DebugRunner.h"

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#if defined(_WIN32)

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "tools/PathUtil.h"

namespace autocoder::shell {

namespace {

constexpr size_t kDebugOutputCap = 8 * 1024;   // OutputDebugString capture cap
constexpr WORD kMaxOdsChars = 4096;            // per-message read limit

// Windows command-line quoting (the CommandLineToArgvW convention).
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// CREATE_UNICODE_ENVIRONMENT block. _NO_DEBUG_HEAP keeps the child's heap
// behavior identical to a non-debugged run (the NT debug heap would otherwise
// change timing and mask/expose heap bugs only under this runner).
std::wstring buildEnvBlock(const QProcessEnvironment& env) {
    QStringList entries = env.toStringList();
    bool hasNoDebugHeap = false;
    for (const QString& e : entries) {
        if (e.startsWith(QStringLiteral("_NO_DEBUG_HEAP="), Qt::CaseInsensitive)) {
            hasNoDebugHeap = true;
            break;
        }
    }
    if (!hasNoDebugHeap) entries << QStringLiteral("_NO_DEBUG_HEAP=1");
    entries.sort(Qt::CaseInsensitive);

    std::wstring block;
    for (const QString& e : entries) {
        block += e.toStdWString();
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::wstring moduleNameFromHandle(HANDLE hFile, const wchar_t* fallback) {
    wchar_t buf[MAX_PATH * 2];
    if (hFile && hFile != INVALID_HANDLE_VALUE
        && GetFinalPathNameByHandleW(hFile, buf, static_cast<DWORD>(std::size(buf)),
                                     FILE_NAME_NORMALIZED) > 0) {
        const wchar_t* name = wcsrchr(buf, L'\\');
        return name ? name + 1 : buf;
    }
    return fallback;
}

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "float divide by zero";
        case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
        case EXCEPTION_IN_PAGE_ERROR:         return "in-page I/O error";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
        case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
        case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
        case 0xC0000409:                      return "fail-fast (stack buffer overrun, abort, or invalid parameter)";
        case 0xE06D7363:                      return "unhandled C++ exception";
        case 0xC0000135:                      return "required DLL not found";
        case 0xC0000142:                      return "DLL initialization failed";
        default:                              return "exception";
    }
}

struct ModuleMap {
    std::map<ULONG_PTR, std::wstring> byBase;

    std::wstring nameFor(ULONG_PTR address) const {
        std::wstring best;
        for (const auto& [base, name] : byBase) {
            if (base > address) break;
            best = name;
        }
        return best.empty() ? L"<unknown module>" : best;
    }
};

std::string describeException(const EXCEPTION_RECORD& rec, const ModuleMap& modules) {
    char buf[512];
    const ULONG_PTR pc = reinterpret_cast<ULONG_PTR>(rec.ExceptionAddress);
    const std::wstring moduleW = modules.nameFor(pc);
    const std::string module = QString::fromStdWString(moduleW).toStdString();

    if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2) {
        const char* op = rec.ExceptionInformation[0] == 0 ? "reading"
                       : rec.ExceptionInformation[0] == 1 ? "writing"
                       : "executing";
        std::snprintf(buf, sizeof(buf),
                      "access violation %s address 0x%016llX (code 0xC0000005) "
                      "at instruction 0x%016llX in %s",
                      op,
                      static_cast<unsigned long long>(rec.ExceptionInformation[1]),
                      static_cast<unsigned long long>(pc),
                      module.c_str());
    } else {
        std::snprintf(buf, sizeof(buf),
                      "%s (code 0x%08lX) at instruction 0x%016llX in %s",
                      exceptionName(rec.ExceptionCode),
                      static_cast<unsigned long>(rec.ExceptionCode),
                      static_cast<unsigned long long>(pc),
                      module.c_str());
    }
    return buf;
}

struct SharedState {
    std::mutex mutex;
    std::string pendingOutput;        // chunks the caller has not drained yet

    std::atomic<bool> started{false};
    std::atomic<bool> startFailed{false};
    std::atomic<bool> exited{false};

    HANDLE hProcess = nullptr;        // owned; closed by the caller at the end
    HANDLE hJob = nullptr;            // kill-on-close job; closed by the caller
    DWORD exitCode = 0;
    std::string startError;

    // Written only by the debugger thread; read by the caller after exit.
    bool crashed = false;
    std::string crashSummary;
    std::string debugOutput;
    size_t droppedDebugBytes = 0;
};

void appendDebugText(SharedState& shared, const std::string& text) {
    if (shared.debugOutput.size() >= kDebugOutputCap) {
        shared.droppedDebugBytes += text.size();
        return;
    }
    const size_t room = kDebugOutputCap - shared.debugOutput.size();
    shared.debugOutput.append(text, 0, std::min(text.size(), room));
    if (text.size() > room) shared.droppedDebugBytes += text.size() - room;
}

void readerThreadMain(HANDLE readEnd, SharedState& shared) {
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.pendingOutput.append(buf, got);
    }
    CloseHandle(readEnd);
}

// Runs the whole child lifetime: CreateProcess (the thread that creates a
// debuggee must be the one that services its debug events), the pipe reader,
// and the debug-event loop until EXIT_PROCESS.
void debuggerThreadMain(const std::wstring& programW,
                        std::wstring cmdLine,       // mutable: CreateProcessW writes into it
                        const std::wstring& cwdW,
                        std::wstring envBlock,
                        SharedState& shared) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        shared.startError = "CreatePipe failed";
        shared.startFailed = true;
        shared.started = true;
        shared.exited = true;
        return;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNul;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;

    PROCESS_INFORMATION pi = {};
    const BOOL created = CreateProcessW(
        programW.c_str(), cmdLine.data(), nullptr, nullptr,
        TRUE /*inherit handles*/,
        DEBUG_ONLY_THIS_PROCESS | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        envBlock.data(), cwdW.empty() ? nullptr : cwdW.c_str(), &si, &pi);

    // The child owns its copies now; close ours so ReadFile sees EOF on exit.
    CloseHandle(writeEnd);
    if (hNul && hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    if (!created) {
        shared.startError = "CreateProcessW failed (error "
                          + std::to_string(GetLastError()) + ")";
        shared.startFailed = true;
        shared.started = true;
        shared.exited = true;
        CloseHandle(readEnd);
        return;
    }

    // Kill-on-close job so grandchildren die with the child on cancel/timeout.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        AssignProcessToJobObject(job, pi.hProcess);
    }

    shared.hProcess = pi.hProcess;
    shared.hJob = job;
    shared.started = true;
    CloseHandle(pi.hThread);

    std::thread reader(readerThreadMain, readEnd, std::ref(shared));

    ModuleMap modules;
    bool sawCrash = false;
    std::string crashText;
    // Last first-chance exception, kept in case the fatal one never reaches a
    // second chance (fail-fast 0xC0000409 terminates without one, and an SEH
    // filter may call ExitProcess). Matched against the exit code below.
    DWORD lastFirstChanceCode = 0;
    std::string lastFirstChanceText;
    DWORD exitCode = 0;

    for (;;) {
        DEBUG_EVENT ev = {};
        if (!WaitForDebugEvent(&ev, 200)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            // Unexpected failure: make sure the child cannot outlive the loop.
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            GetExitCodeProcess(pi.hProcess, &exitCode);
            break;
        }

        DWORD continueStatus = DBG_CONTINUE;
        bool done = false;

        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                const auto& info = ev.u.CreateProcessInfo;
                modules.byBase[reinterpret_cast<ULONG_PTR>(info.lpBaseOfImage)]
                    = moduleNameFromHandle(info.hFile, programW.c_str());
                if (info.hFile) CloseHandle(info.hFile);
                break;
            }
            case LOAD_DLL_DEBUG_EVENT: {
                const auto& info = ev.u.LoadDll;
                modules.byBase[reinterpret_cast<ULONG_PTR>(info.lpBaseOfDll)]
                    = moduleNameFromHandle(info.hFile, L"<unknown module>");
                if (info.hFile) CloseHandle(info.hFile);
                break;
            }
            case UNLOAD_DLL_DEBUG_EVENT:
                modules.byBase.erase(reinterpret_cast<ULONG_PTR>(ev.u.UnloadDll.lpBaseOfDll));
                break;
            case OUTPUT_DEBUG_STRING_EVENT: {
                const auto& info = ev.u.DebugString;
                const WORD chars = std::min<WORD>(info.nDebugStringLength, kMaxOdsChars);
                if (chars > 0) {
                    std::string text;
                    SIZE_T got = 0;
                    if (info.fUnicode) {
                        std::wstring w(chars, L'\0');
                        if (ReadProcessMemory(pi.hProcess, info.lpDebugStringData,
                                              w.data(), chars * sizeof(wchar_t), &got)) {
                            w.resize(got / sizeof(wchar_t));
                            while (!w.empty() && w.back() == L'\0') w.pop_back();
                            text = QString::fromWCharArray(w.data(), static_cast<int>(w.size()))
                                       .toUtf8().toStdString();
                        }
                    } else {
                        std::string a(chars, '\0');
                        if (ReadProcessMemory(pi.hProcess, info.lpDebugStringData,
                                              a.data(), chars, &got)) {
                            a.resize(got);
                            while (!a.empty() && a.back() == '\0') a.pop_back();
                            text = QString::fromLocal8Bit(a.c_str(), static_cast<int>(a.size()))
                                       .toUtf8().toStdString();
                        }
                    }
                    if (!text.empty()) appendDebugText(shared, text);
                }
                break;
            }
            case EXCEPTION_DEBUG_EVENT: {
                const EXCEPTION_RECORD& rec = ev.u.Exception.ExceptionRecord;
                const bool isBreakpoint = rec.ExceptionCode == EXCEPTION_BREAKPOINT
                                       || rec.ExceptionCode == 0x4000001F;  // WOW64 breakpoint
                if (isBreakpoint) {
                    // The loader breakpoint (and any stray int3): just resume.
                    continueStatus = DBG_CONTINUE;
                } else if (ev.u.Exception.dwFirstChance) {
                    lastFirstChanceCode = rec.ExceptionCode;
                    lastFirstChanceText = describeException(rec, modules);
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;  // let SEH handlers run
                } else {
                    sawCrash = true;
                    crashText = describeException(rec, modules);
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;  // process dies with this code
                }
                break;
            }
            case EXIT_PROCESS_DEBUG_EVENT:
                exitCode = ev.u.ExitProcess.dwExitCode;
                done = true;
                break;
            default:
                break;
        }

        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, continueStatus);
        if (done) break;
    }

    if (!sawCrash && (exitCode & 0xF0000000u) == 0xC0000000u) {
        // Died on an error NTSTATUS without a second-chance event (fail-fast,
        // or an SEH filter that exited). Use the matching first-chance record.
        sawCrash = true;
        crashText = (lastFirstChanceCode == exitCode && !lastFirstChanceText.empty())
            ? lastFirstChanceText
            : std::string(exceptionName(exitCode)) + " (process exit status 0x"
              + [&] { char b[16]; std::snprintf(b, sizeof(b), "%08lX",
                                                static_cast<unsigned long>(exitCode)); return std::string(b); }()
              + ")";
    }

    reader.join();
    shared.exitCode = exitCode;
    shared.crashed = sawCrash;
    shared.crashSummary = std::move(crashText);
    shared.exited = true;
}

}  // namespace

DebugRunResult runUnderDebugger(const std::filesystem::path& program,
                                const std::vector<std::string>& args,
                                const std::filesystem::path& cwd,
                                const QProcessEnvironment& env,
                                int timeoutMs,
                                int maxOutputBytes,
                                std::atomic<bool>* cancelled,
                                const std::function<void(const std::string&)>& onOutput) {
    DebugRunResult result;

    const std::wstring programW = program.native();
    std::wstring cmdLine = quoteArg(programW);
    for (const std::string& a : args) {
        cmdLine.push_back(L' ');
        cmdLine += quoteArg(QString::fromUtf8(a.c_str()).toStdWString());
    }

    SharedState shared;
    std::thread debugger(debuggerThreadMain, programW, std::move(cmdLine),
                         cwd.native(), buildEnvBlock(env), std::ref(shared));

    while (!shared.started) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    if (shared.startFailed) {
        debugger.join();
        result.launched = false;  // caller falls back to its normal runner
        return result;
    }
    result.launched = true;

    bool truncationNoticeEmitted = false;
    auto drain = [&]() {
        std::string chunk;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            chunk.swap(shared.pendingOutput);
        }
        if (chunk.empty()) return;
        const size_t remaining = result.output.size() < static_cast<size_t>(maxOutputBytes)
            ? static_cast<size_t>(maxOutputBytes) - result.output.size()
            : 0;
        const size_t visibleLen = std::min(chunk.size(), remaining);
        if (visibleLen > 0) {
            result.output.append(chunk, 0, visibleLen);
            if (onOutput) onOutput(chunk.substr(0, visibleLen));
        }
        if (visibleLen < chunk.size()) {
            result.droppedOutputBytes += chunk.size() - visibleLen;
            if (!truncationNoticeEmitted) {
                truncationNoticeEmitted = true;
                if (onOutput) {
                    onOutput("\n[... output truncated at " + std::to_string(maxOutputBytes)
                             + " bytes; process output continues to be drained ...]\n");
                }
            }
        }
    };

    const auto start = std::chrono::steady_clock::now();
    bool terminated = false;
    while (!shared.exited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain();
        if (terminated) continue;
        if (cancelled && cancelled->load()) {
            result.cancelled = true;
            terminated = true;
            TerminateProcess(shared.hProcess, 1);
            continue;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeoutMs) {
            result.timedOut = true;
            terminated = true;
            TerminateProcess(shared.hProcess, 1);
        }
    }

    debugger.join();
    drain();

    result.exitCode = static_cast<int>(shared.exitCode);
    result.crashed = shared.crashed && !result.cancelled && !result.timedOut;
    result.crashSummary = shared.crashSummary;
    result.debugOutput = shared.debugOutput;
    result.droppedDebugBytes = shared.droppedDebugBytes;

    if (shared.hProcess) CloseHandle(shared.hProcess);
    if (shared.hJob) CloseHandle(shared.hJob);  // kills any leftover grandchildren
    return result;
}

}  // namespace autocoder::shell

#else  // !_WIN32

namespace autocoder::shell {

DebugRunResult runUnderDebugger(const std::filesystem::path&,
                                const std::vector<std::string>&,
                                const std::filesystem::path&,
                                const QProcessEnvironment&,
                                int, int,
                                std::atomic<bool>*,
                                const std::function<void(const std::string&)>&) {
    return {};  // launched == false: caller uses its normal runner
}

}  // namespace autocoder::shell

#endif
