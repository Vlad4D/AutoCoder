// CLI test harness for the agent loop.
//
// Usage:
//   autocoder_cli [--provider deepseek|claude] [--project <dir>]
//                 [--model <id>] [--base-url <url>] [--max-iter <n>]
//                 [--temperature <t>] [--check "<command>"] [--auto-approve]
//                 [--metrics <file.json>] "<prompt>"
//
// API key: DEEPSEEK_API_KEY / ANTHROPIC_API_KEY env var if set, otherwise the
// key saved by the AutoCoder app (Windows Credential Manager via SecretStore).
//
// Drives a full tool-loop turn against the live API and prints results.
// AgentRunner runs on the main thread here (no worker QThread) — the CLI lives
// only as long as one turn, so there's nothing to keep responsive.
//
// Unattended runs (bigtest harness): the CLI never hangs on the two pause
// points. ask_user gets a canned "no user available" answer; a bash command
// that fails the safety gate is allowed once with --auto-approve (intended
// for sandboxed test directories) and denied otherwise.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <QCoreApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include <nlohmann/json.hpp>

#include "agent/AgentRunner.h"
#include "diagnostics/CrashHandler.h"
#include "persistence/SecretStore.h"
#include "tools/PathUtil.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
QTextStream& cout() { static QTextStream s(stdout); return s; }
QTextStream& cerr() { static QTextStream s(stderr); return s; }

struct Args {
    QString prompt;
    QString provider = QStringLiteral("deepseek");
    QString model;
    QString baseUrl;
    QString checkCommand;
    QString metricsFile;
    int maxIter = 0;          // 0 = AgentRunner default
    double temperature = -1.0; // <0 = unset; default resolved per provider below
    bool temperatureSet = false;
    bool autoApprove = false;
    fs::path projectRoot = fs::current_path();
    bool valid = true;
    QString error;
};

Args parseArgs(const QStringList& args) {
    Args a;
    QStringList positional;
    for (int i = 1; i < args.size(); ++i) {
        const QString& s = args[i];
        if (s == "--project" && i + 1 < args.size()) {
            const QByteArray projectUtf8 = args[++i].toUtf8();
            a.projectRoot = pathutil::fromUtf8(std::string(projectUtf8.constData(), projectUtf8.size()));
        } else if (s == "--provider" && i + 1 < args.size()) {
            a.provider = args[++i].trimmed().toLower();
            if (a.provider == QStringLiteral("anthropic"))
                a.provider = QStringLiteral("claude");
            if (a.provider != QStringLiteral("deepseek") && a.provider != QStringLiteral("claude")) {
                a.valid = false;
                a.error = "unknown provider: " + a.provider;
                return a;
            }
        } else if (s == "--model" && i + 1 < args.size()) {
            a.model = args[++i];
        } else if (s == "--base-url" && i + 1 < args.size()) {
            a.baseUrl = args[++i];
        } else if (s == "--max-iter" && i + 1 < args.size()) {
            bool ok = false;
            a.maxIter = args[++i].toInt(&ok);
            if (!ok || a.maxIter < 1) {
                a.valid = false;
                a.error = "--max-iter expects a positive integer";
                return a;
            }
        } else if (s == "--temperature" && i + 1 < args.size()) {
            bool ok = false;
            a.temperature = args[++i].toDouble(&ok);
            if (!ok || a.temperature < 0.0 || a.temperature > 2.0) {
                a.valid = false;
                a.error = "--temperature expects a number in [0, 2]";
                return a;
            }
            a.temperatureSet = true;
        } else if (s == "--check" && i + 1 < args.size()) {
            a.checkCommand = args[++i];
        } else if (s == "--metrics" && i + 1 < args.size()) {
            a.metricsFile = args[++i];
        } else if (s == "--auto-approve") {
            a.autoApprove = true;
        } else if (s.startsWith("--")) {
            a.valid = false;
            a.error = "unknown flag: " + s;
            return a;
        } else {
            positional << s;
        }
    }
    if (positional.isEmpty()) {
        a.valid = false;
        a.error = "missing prompt";
        return a;
    }
    a.prompt = positional.join(' ');
    return a;
}

// Run metrics, accumulated from AgentRunner signals and dumped as JSON for
// the bigtest harness.
struct Metrics {
    std::map<std::string, int> toolCalls;     // name -> started
    std::map<std::string, int> toolErrors;    // name -> finished with ok=false
    int approvalsRequested = 0;
    int approvalsGranted = 0;
    int askUserCalls = 0;
    long long cachedInputTokens = 0;
    long long uncachedInputTokens = 0;
    long long outputTokens = 0;
    int llmRequests = 0;
    qint64 wallMs = 0;
    std::string status = "error";             // "ok" | "error"
    std::string errorDetail;

    json toJson() const {
        json calls = json::object();
        for (const auto& [name, n] : toolCalls) calls[name] = n;
        json errors = json::object();
        for (const auto& [name, n] : toolErrors) errors[name] = n;
        return {
            {"status", status},
            {"error", errorDetail},
            {"wall_ms", wallMs},
            {"llm_requests", llmRequests},
            {"tool_calls", calls},
            {"tool_errors", errors},
            {"approvals_requested", approvalsRequested},
            {"approvals_granted", approvalsGranted},
            {"ask_user_calls", askUserCalls},
            {"tokens", {
                {"cached_input", cachedInputTokens},
                {"uncached_input", uncachedInputTokens},
                {"output", outputTokens},
            }},
        };
    }
};
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("autocoder_cli");
    QCoreApplication::setOrganizationName("AutoCoder");
    QCoreApplication::setApplicationVersion("0.1.0");
    diagnostics::installCrashHandler();

    Args parsed = parseArgs(app.arguments());
    if (!parsed.valid) {
        cerr() << "Error: " << parsed.error << "\n"
               << "Usage: autocoder_cli [--provider deepseek|claude] [--project <dir>]\n"
               << "                     [--model <id>] [--base-url <url>] [--max-iter <n>]\n"
               << "                     [--temperature <t>] [--check \"<command>\"] [--auto-approve]\n"
               << "                     [--metrics <file.json>] \"<prompt>\"\n";
        return 2;
    }

    // API key: environment variable first (explicit override), then the key
    // the AutoCoder app stored in the Windows Credential Manager (same
    // SecretStore the GUI uses), so a configured machine needs no env setup.
    const bool useClaude = parsed.provider == QStringLiteral("claude");
    const char* envName = useClaude ? "ANTHROPIC_API_KEY" : "DEEPSEEK_API_KEY";
    const char* envKey = std::getenv(envName);
    QString apiKey = (envKey && *envKey) ? QString::fromUtf8(envKey) : QString();
    QString keySource = QStringLiteral("environment (%1)").arg(QLatin1String(envName));
    if (apiKey.isEmpty()) {
        apiKey = secretstore::load(parsed.provider);
        keySource = QStringLiteral("Credential Manager (stored by the AutoCoder app)");
    }
    if (apiKey.isEmpty()) {
        cerr() << "No API key for provider '" << parsed.provider << "': set "
               << envName << " or save a key once in the AutoCoder app's Settings.\n";
        return 2;
    }

    cout() << "Project root: " << QString::fromUtf8(pathutil::toUtf8(parsed.projectRoot).c_str()) << "\n";
    cout() << "Provider: " << parsed.provider << " (key from " << keySource << ")\n";
    cout() << "User: " << parsed.prompt << "\n\n";
    cout().flush();

    AgentRunner agent;
    Metrics metrics;
    QElapsedTimer wall;
    wall.start();

    int exitCode = 1;
    bool inAssistantBlock = false;
    auto endAssistantLine = [&]() {
        if (inAssistantBlock) {
            cout() << "\n";
            cout().flush();
            inAssistantBlock = false;
        }
    };

    auto writeMetrics = [&]() {
        if (parsed.metricsFile.isEmpty()) return;
        metrics.wallMs = wall.elapsed();
        try {
            std::ofstream out(parsed.metricsFile.toStdString(),
                              std::ios::binary | std::ios::trunc);
            out << metrics.toJson().dump(2) << "\n";
        } catch (...) {
            cerr() << "WARNING: could not write metrics file\n";
        }
    };

    QObject::connect(&agent, &AgentRunner::assistantTextDelta, &app,
                     [&](const QString& fragment) {
                         if (!inAssistantBlock) {
                             cout() << "[assistant] ";
                             inAssistantBlock = true;
                         }
                         cout() << fragment;
                         cout().flush();
                     });
    QObject::connect(&agent, &AgentRunner::assistantTextFinalized, &app,
                     [&]() { endAssistantLine(); });
    QObject::connect(&agent, &AgentRunner::toolCallStarted, &app,
                     [&](const QString& name, const QString& args) {
                         endAssistantLine();
                         ++metrics.toolCalls[name.toStdString()];
                         cout() << "[tool >] " << name << " " << args << "\n";
                         cout().flush();
                     });
    QObject::connect(&agent, &AgentRunner::toolCallFinished, &app,
                     [&](const QString& name, const QString& result, bool ok) {
                         if (!ok) ++metrics.toolErrors[name.toStdString()];
                         QString preview = result.left(400);
                         if (result.size() > 400)
                             preview += "\n[... +" + QString::number(result.size() - 400) + " bytes]";
                         cout() << "[tool <] " << name << " (" << (ok ? "ok" : "err") << ")\n"
                                << preview << "\n";
                         cout().flush();
                     });
    QObject::connect(&agent, &AgentRunner::tokenUsageStats, &app,
                     [&](int cached, int uncached, int output, int /*total*/) {
                         ++metrics.llmRequests;
                         metrics.cachedInputTokens   += cached;
                         metrics.uncachedInputTokens += uncached;
                         metrics.outputTokens        += output;
                     });

    // Pause point 1: ask_user. There is no user; answer with a canned
    // directive so the turn always proceeds. Deferred to the event loop so
    // the resume does not nest inside the signal emission.
    QObject::connect(&agent, &AgentRunner::userInputRequired, &app,
                     [&](const QString& /*id*/, const QString& /*tool*/,
                         const QString& question, const QStringList& /*options*/) {
                         ++metrics.askUserCalls;
                         endAssistantLine();
                         cout() << "[ask_user] " << question << "\n";
                         cout().flush();
                         const QString answer = QStringLiteral(
                             "This is an unattended automated run; no user is available. "
                             "Proceed using your best judgment and do not call ask_user again.");
                         QTimer::singleShot(0, &agent, [&agent, answer]() {
                             agent.provideUserInput(answer);
                         });
                     });

    // Pause point 2: bash command approval. --auto-approve allows the exact
    // command once (sandboxed test dirs); otherwise deny so the model can
    // pick a safer route. HardBlock commands never reach this point.
    QObject::connect(&agent, &AgentRunner::commandApprovalRequired, &app,
                     [&](const QString& id, const QString& command,
                         const QString& reason, const QString& /*explanation*/) {
                         ++metrics.approvalsRequested;
                         endAssistantLine();
                         const bool allow = parsed.autoApprove;
                         if (allow) ++metrics.approvalsGranted;
                         cout() << "[approval] " << (allow ? "ALLOW" : "DENY")
                                << " (" << reason << "): " << command << "\n";
                         cout().flush();
                         const QString decision = allow ? QStringLiteral("allow_once")
                                                        : QStringLiteral("deny");
                         QTimer::singleShot(0, &agent, [&agent, id, decision]() {
                             agent.provideCommandDecision(id, decision, false);
                         });
                     });

    QObject::connect(&agent, &AgentRunner::turnFinished, &app, [&]() {
        endAssistantLine();
        cout() << "\n[usage] llm_requests=" << metrics.llmRequests
               << " cached_input=" << metrics.cachedInputTokens
               << " uncached_input=" << metrics.uncachedInputTokens
               << " output=" << metrics.outputTokens << "\n";
        cout().flush();
        metrics.status = "ok";
        writeMetrics();
        exitCode = 0;
        app.quit();
    });
    QObject::connect(&agent, &AgentRunner::errorOccurred, &app, [&](const QString& detail) {
        endAssistantLine();
        cerr() << "ERROR: " << detail << "\n";
        cerr().flush();
        metrics.status = "error";
        metrics.errorDetail = detail.toStdString();
        writeMetrics();
        exitCode = 1;
        app.quit();
    });

    agent.setProvider(parsed.provider);
    agent.setApiKey(apiKey);
    if (!parsed.model.isEmpty()) {
        agent.setModel(parsed.model);
    } else if (useClaude) {
        agent.setModel(QStringLiteral("claude-sonnet-4-6"));
    }
    if (!parsed.baseUrl.isEmpty()) {
        agent.setBaseUrl(parsed.baseUrl);
    } else if (useClaude) {
        agent.setBaseUrl(QStringLiteral("https://api.anthropic.com"));
    }
    if (parsed.maxIter > 0) agent.setMaxIterations(parsed.maxIter);
    // Default: deterministic sampling for DeepSeek coding runs (its API
    // default of 1.0 is poor for code); Claude keeps its provider default.
    if (parsed.temperatureSet)
        agent.setTemperature(parsed.temperature);
    else if (!useClaude)
        agent.setTemperature(0.0);
    if (!parsed.checkCommand.isEmpty()) agent.setCheckCommand(parsed.checkCommand);
    agent.setProject(QString::fromUtf8(pathutil::toUtf8(parsed.projectRoot).c_str()));
    agent.submitUserMessage(parsed.prompt);

    return app.exec() == 0 ? exitCode : 1;
}
