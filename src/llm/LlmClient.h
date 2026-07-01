#pragma once

#include <map>
#include <string>

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <nlohmann/json.hpp>

class QNetworkReply;

// Provider-aware LLM HTTP client.
// Supports both non-streaming (sendOnce) and SSE streaming (sendStreaming).
// Handles both DeepSeek (OpenAI-compatible) and Claude (Anthropic Messages API).
class LlmClient : public QObject {
    Q_OBJECT
public:
    explicit LlmClient(QObject* parent = nullptr);

    void setProvider(QString provider);       // "deepseek" or "claude"
    void setApiKey(QString key);
    void setModel(QString model);            // default: "deepseek-chat"
    void setBaseUrl(QString url);            // default: "https://api.deepseek.com"
    void setMaxTokens(int tokens);            // used by providers that require max_tokens

    // Non-streaming: emits responseReceived once with the full assistant message.
    void sendOnce(nlohmann::json messages, nlohmann::json tools = {});

    // Streaming: emits tokenDelta as content arrives, toolCallDelta as tool-call
    // fragments arrive, then streamFinished with the fully-assembled assistant
    // message. Only one stream may be in flight at a time.
    void sendStreaming(nlohmann::json messages, nlohmann::json tools = {});

    // Abort an in-flight streaming request.
    void abort();

    // Add Anthropic prompt-caching breakpoints to a /v1/messages body: the
    // system block plus the last block of the final two messages. Pure JSON
    // transform; public so the offline tests can exercise it directly.
    static void addClaudeCacheBreakpoints(nlohmann::json& body);

signals:
    // Non-streaming: full assistant message (choices[0].message).
    void responseReceived(nlohmann::json assistantMessage);

    // Streaming: incremental events.
    void tokenDelta(QString textFragment);
    void toolCallDelta(int index, QString id, QString name, QString argumentsFragment);
    void streamFinished(nlohmann::json assistantMessage);
    void tokenUsage(int cachedInputTokens, int uncachedInputTokens,
                    int outputTokens, int totalTokens);

    // Either mode: transport / HTTP / parse error.
    void errorOccurred(QString detail);

private slots:
    void onStreamReadyRead();
    void onStreamFinished();

private:
    void postChatCompletions(nlohmann::json body, bool streaming);
    void postClaudeMessages(nlohmann::json body, bool streaming);
    void resetStreamState();
    void processSseLine(const QByteArray& line);
    void processOpenAiSsePayload(const QByteArray& payload);
    void processClaudeSsePayload(const QByteArray& payload);
    void emitFinalStreamMessage();
    void mergeAndEmitUsage(const nlohmann::json& usage);
    nlohmann::json buildOpenAiBody(nlohmann::json messages, nlohmann::json tools, bool streaming) const;
    nlohmann::json buildClaudeBody(nlohmann::json messages, nlohmann::json tools, bool streaming) const;
    nlohmann::json claudeMessageToOpenAi(const nlohmann::json& message) const;

    QNetworkAccessManager nm_;
    QString provider_ = "deepseek";
    QString apiKey_;
    QString model_   = "deepseek-chat";
    QString baseUrl_ = "https://api.deepseek.com";
    int maxTokens_ = 8192;

    // Any in-flight reply (streaming or non-streaming), so abort() can cancel it.
    QPointer<QNetworkReply> pendingReply_;

    // ----- Streaming state -----
    QPointer<QNetworkReply> streamReply_;
    QByteArray streamBuffer_;
    std::string streamContent_;
    struct AccumulatedCall {
        std::string id;
        std::string name;
        std::string arguments;
    };
    std::map<int, AccumulatedCall> streamCalls_;
    nlohmann::json streamUsage_;
    bool streamSawDone_ = false;
    int streamToolCounter_ = 0;  // Monotonic tool-call counter for Claude content_block_index normalization
    std::map<int, int> streamBlockToTool_;  // Claude content_block_index -> tool ordinal
};
