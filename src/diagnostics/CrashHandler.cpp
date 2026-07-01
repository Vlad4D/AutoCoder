#include "diagnostics/CrashHandler.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMessageLogContext>
#include <QStandardPaths>
#include <QString>
#include <QSysInfo>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#  define NOMINMAX
#  include <windows.h>
#  include <dbghelp.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "DbgHelp.lib")
#  endif
#endif

namespace diagnostics {
namespace {

std::filesystem::path g_crashLogPath;
std::terminate_handler g_previousTerminateHandler = nullptr;
QtMessageHandler g_previousQtMessageHandler = nullptr;
std::atomic_bool g_handlingCrash{false};

std::filesystem::path pathFromQString(const QString& path) {
#if defined(Q_OS_WIN)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

QString pathToQString(const std::filesystem::path& path) {
#if defined(Q_OS_WIN)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

std::filesystem::path defaultCrashLogPath() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::currentPath();

    QDir dir(base);
    dir.mkpath("logs");
    return pathFromQString(dir.filePath("logs/crashes.log"));
}

std::string narrow(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

std::string qtMessageTypeName(QtMsgType type) {
    switch (type) {
    case QtDebugMsg: return "debug";
    case QtInfoMsg: return "info";
    case QtWarningMsg: return "warning";
    case QtCriticalMsg: return "critical";
    case QtFatalMsg: return "fatal";
    }
    return "unknown";
}

std::string currentTimestamp() {
    return narrow(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
}

std::string appMetadata() {
    std::ostringstream out;
    out << "Application: " << narrow(QCoreApplication::applicationName()) << '\n';
    out << "Version: " << narrow(QCoreApplication::applicationVersion()) << '\n';
    out << "Organization: " << narrow(QCoreApplication::organizationName()) << '\n';
    out << "Qt: " << qVersion() << '\n';
    out << "OS: " << narrow(QSysInfo::prettyProductName()) << '\n';
    out << "Kernel: " << narrow(QSysInfo::kernelType()) << ' ' << narrow(QSysInfo::kernelVersion()) << '\n';
    out << "CPU architecture: " << narrow(QSysInfo::currentCpuArchitecture()) << '\n';
    out << "Process id: " << QCoreApplication::applicationPid() << '\n';
    out << "Thread id: " << std::this_thread::get_id() << '\n';
    return out.str();
}

std::ofstream openCrashLog() {
    if (g_crashLogPath.empty())
        g_crashLogPath = defaultCrashLogPath();

    std::error_code ec;
    std::filesystem::create_directories(g_crashLogPath.parent_path(), ec);
    return std::ofstream(g_crashLogPath, std::ios::app);
}

#if defined(Q_OS_WIN)
std::string hex64(DWORD64 value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

void ensureSymbolsInitialized() {
    static std::atomic_bool initialized{false};
    bool expected = false;
    if (!initialized.compare_exchange_strong(expected, true))
        return;

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
}

std::string symbolForAddress(DWORD64 address) {
    ensureSymbolsInitialized();

    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    std::ostringstream out;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        out << symbol->Name;
        if (displacement != 0)
            out << "+" << hex64(displacement);
    } else {
        out << "<unknown>";
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line)) {
        out << " (" << line.FileName << ':' << line.LineNumber << ')';
    }

    out << " [" << hex64(address) << "]";
    return out.str();
}

std::string stackTraceFromCurrentThread() {
    void* frames[64] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, static_cast<DWORD>(sizeof(frames) / sizeof(frames[0])),
                                                    frames, nullptr);

    std::ostringstream out;
    out << "Stack trace:\n";
    for (USHORT i = 0; i < frameCount; ++i)
        out << "  #" << std::setw(2) << i << ' ' << symbolForAddress(reinterpret_cast<DWORD64>(frames[i])) << '\n';
    return out.str();
}

std::string stackTraceFromContext(const CONTEXT* originalContext) {
    if (!originalContext)
        return stackTraceFromCurrentThread();

    ensureSymbolsInitialized();

    CONTEXT context = *originalContext;
    STACKFRAME64 frame = {};
    DWORD machine = 0;

#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#else
    return stackTraceFromCurrentThread();
#endif

    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    std::ostringstream out;
    out << "Stack trace:\n";
    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(machine, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0)
            break;
        out << "  #" << std::setw(2) << i << ' ' << symbolForAddress(frame.AddrPC.Offset) << '\n';
    }
    return out.str();
}

std::string exceptionCodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP: return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    default: return "UNKNOWN_EXCEPTION";
    }
}

std::string windowsExceptionDetails(EXCEPTION_POINTERS* info) {
    std::ostringstream out;
    if (!info || !info->ExceptionRecord) {
        out << "Windows exception pointers were not available.\n";
        return out.str();
    }

    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    out << "Exception code: " << exceptionCodeName(record->ExceptionCode)
        << " (" << hex64(record->ExceptionCode) << ")\n";
    out << "Exception address: " << hex64(reinterpret_cast<DWORD64>(record->ExceptionAddress)) << '\n';
    out << "Exception flags: " << hex64(record->ExceptionFlags) << '\n';
    out << "Exception parameters: " << record->NumberParameters << '\n';
    for (DWORD i = 0; i < record->NumberParameters; ++i)
        out << "  parameter[" << i << "]: " << hex64(record->ExceptionInformation[i]) << '\n';

    if (info->ContextRecord) {
#if defined(_M_X64)
        const CONTEXT* c = info->ContextRecord;
        out << "Registers:\n";
        out << "  RIP=" << hex64(c->Rip) << " RSP=" << hex64(c->Rsp) << " RBP=" << hex64(c->Rbp) << '\n';
        out << "  RAX=" << hex64(c->Rax) << " RBX=" << hex64(c->Rbx)
            << " RCX=" << hex64(c->Rcx) << " RDX=" << hex64(c->Rdx) << '\n';
        out << "  RSI=" << hex64(c->Rsi) << " RDI=" << hex64(c->Rdi)
            << " R8=" << hex64(c->R8) << " R9=" << hex64(c->R9) << '\n';
#elif defined(_M_IX86)
        const CONTEXT* c = info->ContextRecord;
        out << "Registers:\n";
        out << "  EIP=" << hex64(c->Eip) << " ESP=" << hex64(c->Esp) << " EBP=" << hex64(c->Ebp) << '\n';
        out << "  EAX=" << hex64(c->Eax) << " EBX=" << hex64(c->Ebx)
            << " ECX=" << hex64(c->Ecx) << " EDX=" << hex64(c->Edx) << '\n';
#endif
    }

    out << stackTraceFromContext(info->ContextRecord);
    return out.str();
}
#else
std::string stackTraceFromCurrentThread() {
    return "Stack trace: unavailable on this platform.\n";
}
#endif

// Scrub anything resembling an API key / bearer token so secrets never land in
// a crash log. Deliberately conservative: targets the well-known shapes
// (sk-..., Bearer <token>, x-api-key: <token>) rather than any long token, to
// avoid mangling stack addresses/hashes.
std::string redactSecrets(std::string s) {
    static const std::regex patterns[] = {
        std::regex(R"(sk-[A-Za-z0-9_\-]{16,})"),
        std::regex(R"((?:[Bb]earer\s+)[A-Za-z0-9._\-]{12,})"),
        std::regex(R"(((?:x-api-key|api[_-]?key|authorization)\s*[:=]\s*)[^\s]{8,})",
                   std::regex::icase),
    };
    s = std::regex_replace(s, patterns[0], "sk-***REDACTED***");
    s = std::regex_replace(s, patterns[1], "Bearer ***REDACTED***");
    s = std::regex_replace(s, patterns[2], "$1***REDACTED***");
    return s;
}

void appendCrashRecord(const std::string& title, const std::string& details) {
    std::ofstream out = openCrashLog();
    if (!out)
        return;

    const std::string safeTitle   = redactSecrets(title);
    const std::string safeDetails = redactSecrets(details);

    out << "\n============================================================\n";
    out << "Crash captured: " << currentTimestamp() << '\n';
    out << "Reason: " << safeTitle << "\n\n";
    out << appMetadata() << '\n';
    out << safeDetails;
    if (!safeDetails.empty() && safeDetails.back() != '\n')
        out << '\n';
    out.flush();
}

std::string currentCppExceptionDetails() {
    std::ostringstream out;
    std::exception_ptr exception = std::current_exception();
    if (!exception) {
        out << "No active C++ exception was available.\n";
    } else {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& e) {
            out << "C++ exception type: std::exception-derived\n";
            out << "what(): " << e.what() << '\n';
        } catch (...) {
            out << "C++ exception type: non-std exception\n";
        }
    }

    out << stackTraceFromCurrentThread();
    return out.str();
}

std::string signalName(int signalNumber) {
    switch (signalNumber) {
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGINT: return "SIGINT";
    case SIGSEGV: return "SIGSEGV";
    case SIGTERM: return "SIGTERM";
    default: return "signal " + std::to_string(signalNumber);
    }
}

void signalHandler(int signalNumber) {
    if (!g_handlingCrash.exchange(true)) {
        std::ostringstream details;
        details << "C runtime signal: " << signalName(signalNumber)
                << " (" << signalNumber << ")\n";
        details << stackTraceFromCurrentThread();
        appendCrashRecord("C runtime signal", details.str());
    }

    std::signal(signalNumber, SIG_DFL);
    std::_Exit(128 + signalNumber);
}

void terminateHandler() {
    if (!g_handlingCrash.exchange(true))
        appendCrashRecord("std::terminate", currentCppExceptionDetails());

    std::abort();
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type == QtFatalMsg && !g_handlingCrash.exchange(true)) {
        std::ostringstream details;
        details << "Qt message type: " << qtMessageTypeName(type) << '\n';
        details << "Message: " << narrow(message) << '\n';
        if (context.file)
            details << "File: " << context.file << ':' << context.line << '\n';
        if (context.function)
            details << "Function: " << context.function << '\n';
        details << stackTraceFromCurrentThread();
        appendCrashRecord("Qt fatal message", details.str());
    }

    if (g_previousQtMessageHandler)
        g_previousQtMessageHandler(type, context, message);
    else {
        const QByteArray localMessage = message.toLocal8Bit();
        std::fprintf(stderr, "%s: %s\n", qtMessageTypeName(type).c_str(), localMessage.constData());
    }
}

#if defined(Q_OS_WIN)
LONG WINAPI windowsUnhandledExceptionFilter(EXCEPTION_POINTERS* info) {
    if (!g_handlingCrash.exchange(true))
        appendCrashRecord("Unhandled Windows exception", windowsExceptionDetails(info));

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace

void installCrashHandler() {
    g_crashLogPath = defaultCrashLogPath();

#if defined(Q_OS_WIN)
    // Suppress system error MESSAGE BOXES (WER fault dialog, missing-DLL box,
    // critical-error box) for this process AND every child it launches --
    // QProcess children inherit the error mode. Without this, a crashing
    // program started by the bash tool (e.g. a freshly built demo) pops a
    // modal dialog and the tool call hangs until its timeout instead of
    // returning the failure to the agent. Our own crashes are still captured
    // by the handlers installed below.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                 | SEM_NOOPENFILEERRORBOX);
#endif

    g_previousTerminateHandler = std::set_terminate(terminateHandler);
    Q_UNUSED(g_previousTerminateHandler);
    std::signal(SIGABRT, signalHandler);
    std::signal(SIGFPE, signalHandler);
    std::signal(SIGILL, signalHandler);
    std::signal(SIGSEGV, signalHandler);
    std::signal(SIGTERM, signalHandler);
    g_previousQtMessageHandler = qInstallMessageHandler(qtMessageHandler);

#if defined(Q_OS_WIN)
    SetUnhandledExceptionFilter(windowsUnhandledExceptionFilter);
    // SymInitialize with fInvade=FALSE avoids enumerating every loaded module
    // at startup (which can trigger network symbol-server lookups on some
    // Windows configurations, adding several seconds of delay). We defer
    // symbol loading to the first crash, where ensureSymbolsInitialized will
    // be called by symbolForAddress on demand.
    // ensureSymbolsInitialized();
#endif
}

QString crashLogPath() {
    if (g_crashLogPath.empty())
        g_crashLogPath = defaultCrashLogPath();
    return pathToQString(g_crashLogPath);
}

void logCurrentException(const char* context) {
    appendCrashRecord(context ? context : "C++ exception", currentCppExceptionDetails());
}

bool clearCrashLog() {
    if (g_crashLogPath.empty())
        g_crashLogPath = defaultCrashLogPath();
    std::error_code ec;
    std::filesystem::remove(g_crashLogPath, ec);
    return !ec;
}

} // namespace diagnostics
