#include "LlmClient.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

using nlohmann::json;

namespace {

constexpr const char* kClaudeApiVersion = "2023-06-01";

json contentAsBlocks(const json& content) {
    if (content.is_array()) return content;
    if (content.is_string()) {
        std::string text = content.get<std::string>();
        if (!text.empty())
            return json::array({{{"type", "text"}, {"text", std::move(text)}}});
    }
    return json::array();
}

void appendAnthropicMessage(json& messages, json msg) {
    if (msg.value("role", "").empty() || !msg.contains("content")) return;

    if (!messages.empty()
        && messages.back().value("role", "") == msg.value("role", "")) {
        json merged = contentAsBlocks(messages.back()["content"]);
        json extra = contentAsBlocks(msg["content"]);
        for (auto& block : extra)
            merged.push_back(std::move(block));
        messages.back()["content"] = std::move(merged);
        return;
    }

    messages.push_back(std::move(msg));
}

json parseArgumentsObject(const std::string& args) {
    if (args.empty()) return json::object();
    try {
        json parsed = json::parse(args);
        return parsed.is_object() ? parsed : json::object();
    } catch (const std::exception&) {
        return json::object();
    }
}

QString normalizedProvider(QString provider) {
    provider = provider.trimmed().toLower();
    if (provider == QStringLiteral("anthropic"))
        return QStringLiteral("claude");
    if (provider != QStringLiteral("claude"))
        return QStringLiteral("deepseek");
    return provider;
}

int usageInt(const json& usage, const char* key) {
    if (!usage.contains(key) || usage[key].is_null()) return 0;
    if (!usage[key].is_number_integer()) return 0;
    const auto value = usage[key].get<long long>();
    return static_cast<int>(std::clamp<long long>(
        value, 0, std::numeric_limits<int>::max()));
}

int anthropicCacheCreationTokens(const json& usage) {
    int total = usageInt(usage, "cache_creation_input_tokens");
    if (usage.contains("cache_creation") && usage["cache_creation"].is_object()) {
        for (const auto& item : usage["cache_creation"].items()) {
            if (item.value().is_number_integer()) {
                const auto value = item.value().get<long long>();
                const auto clamped = std::clamp<long long>(
                    value, 0, std::numeric_limits<int>::max() - total);
                total += static_cast<int>(clamped);
            }
        }
    }
    return total;
}

}  // namespace

LlmClient::LlmClient(QObject* parent) : QObject(parent) {}

void LlmClient::setProvider(QString provider) { provider_ = normalizedProvider(std::move(provider)); }
void LlmClient::setApiKey(QString key)  { apiKey_  = std::move(key); }
void LlmClient::setModel(QString model) { model_   = std::move(model); }
void LlmClient::setBaseUrl(QString url) { baseUrl_ = std::move(url); }
void LlmClient::setMaxTokens(int tokens) {
    if (tokens > 0) maxTokens_ = tokens;
}

void LlmClient::sendOnce(json messages, json tools) {
    if (provider_ == QStringLiteral("claude")) {
        postClaudeMessages(buildClaudeBody(std::move(messages), std::move(tools), false), false);
        return;
    }
    postChatCompletions(buildOpenAiBody(std::move(messages), std::move(tools), false), false);
}

void LlmClient::sendStreaming(json messages, json tools) {
    if (streamReply_) {
        emit errorOccurred("a streaming request is already in flight");
        return;
    }
    if (provider_ == QStringLiteral("claude")) {
        postClaudeMessages(buildClaudeBody(std::move(messages), std::move(tools), true), true);
        return;
    }
    postChatCompletions(buildOpenAiBody(std::move(messages), std::move(tools), true), true);
}

json LlmClient::buildOpenAiBody(json messages, json tools, bool streaming) const {
    json body = {
        {"model",    model_.toStdString()},
        {"messages", std::move(messages)},
        {"max_tokens", std::clamp(maxTokens_, 1, 128000)},
        {"stream",   streaming}
    };
    if (streaming) {
        body["stream_options"] = {{"include_usage", true}};
    }
    if (!tools.is_null() && !tools.empty()) {
        body["tools"]       = std::move(tools);
        body["tool_choice"] = "auto";
    }
    return body;
}

json LlmClient::buildClaudeBody(json messages, json tools, bool streaming) const {
    json body = {
        {"model", model_.toStdString()},
        {"messages", json::array()},
        {"max_tokens", std::clamp(maxTokens_, 1, 128000)},
        {"stream", streaming}
    };

    if (messages.is_array()) {
        for (size_t i = 0; i < messages.size(); ++i) {
            const json& m = messages[i];
            const std::string role = m.value("role", "");

            if (role == "system") {
                // Use contentAsBlocks for content that may be array/object.
                json blocks = contentAsBlocks(m.value("content", json()));
                std::string systemText;
                for (const auto& block : blocks) {
                    if (block.value("type", "") == "text") {
                        if (!systemText.empty()) systemText += "\n\n";
                        systemText += block.value("text", "");
                    }
                }
                if (!systemText.empty()) {
                    if (body.contains("system") && body["system"].is_string())
                        body["system"] = body["system"].get<std::string>() + "\n\n" + systemText;
                    else
                        body["system"] = std::move(systemText);
                }
                continue;
            }

            if (role == "tool") {
                json content = json::array();
                while (i < messages.size() && messages[i].value("role", "") == "tool") {
                    json rawContent = messages[i].value("content", json());
                    json toolContent = contentAsBlocks(rawContent);
                    // Anthropic rejects tool_result with empty content.
                    if (toolContent.empty()) {
                        toolContent = "(no output)";
                    }
                    content.push_back({
                        {"type", "tool_result"},
                        {"tool_use_id", messages[i].value("tool_call_id", "")},
                        {"content", std::move(toolContent)}
                    });
                    ++i;
                }
                --i;
                appendAnthropicMessage(body["messages"], {
                    {"role", "user"},
                    {"content", std::move(content)}
                });
                continue;
            }

            if (role == "user") {
                json blocks = contentAsBlocks(m.value("content", json()));
                appendAnthropicMessage(body["messages"], {
                    {"role", "user"},
                    {"content", blocks.empty() ? json("") : std::move(blocks)}
                });
                continue;
            }

            if (role == "assistant") {
                json content = json::array();
                json rawContent = m.contains("content") && !m["content"].is_null()
                    ? contentAsBlocks(m["content"]) : json("");
                if (rawContent.is_array()) {
                    for (const auto& block : rawContent) {
                        if (block.value("type", "") == "text") {
                            std::string text = block.value("text", "");
                            if (!text.empty())
                                content.push_back({{"type", "text"}, {"text", std::move(text)}});
                        }
                    }
                } else if (rawContent.is_string()) {
                    std::string text = rawContent.get<std::string>();
                    if (!text.empty())
                        content.push_back({{"type", "text"}, {"text", std::move(text)}});
                }
                if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                    for (const auto& tc : m["tool_calls"]) {
                        const auto& fn = tc.value("function", json::object());
                        content.push_back({
                            {"type", "tool_use"},
                            {"id", tc.value("id", "")},
                            {"name", fn.value("name", "")},
                            {"input", parseArgumentsObject(fn.value("arguments", ""))}
                        });
                    }
                }
                if (!content.empty()) {
                    appendAnthropicMessage(body["messages"], {
                        {"role", "assistant"},
                        {"content", std::move(content)}
                    });
                }
            }
        }
    }

    if (!tools.is_null() && !tools.empty()) {
        json claudeTools = json::array();
        for (const auto& tool : tools) {
            const json& fn = tool.value("function", json::object());
            if (!fn.contains("name")) continue;
            claudeTools.push_back({
                {"name", fn.value("name", "")},
                {"description", fn.value("description", "")},
                {"input_schema", fn.value("parameters", json::object())}
            });
        }
        if (!claudeTools.empty())
            body["tools"] = std::move(claudeTools);
    }

    addClaudeCacheBreakpoints(body);

    return body;
}

// Prompt caching: every tool-loop iteration re-sends the whole conversation,
// so mark the stable prefix with cache_control breakpoints and let the API
// reuse its server-side cache (5-minute TTL) instead of re-processing it.
// Breakpoints (max 4 allowed; we use up to 3):
//   1. The system prompt block -- caches tools + system, which never change
//      within a conversation.
//   2-3. The last block of the final two top-level messages. The trailing
//      breakpoint caches the full conversation up to this request; the
//      second-to-last one still matches on the NEXT request (where the tail
//      has grown), so consecutive iterations always find a cached prefix.
// Prefixes shorter than the model's minimum (1024 tokens) are silently not
// cached -- no error, so this is safe unconditionally.
void LlmClient::addClaudeCacheBreakpoints(json& body) {
    static const json kEphemeral = {{"type", "ephemeral"}};

    if (body.contains("system") && body["system"].is_string()) {
        body["system"] = json::array({{
            {"type", "text"},
            {"text", body["system"].get<std::string>()},
            {"cache_control", kEphemeral}
        }});
    }

    if (!body.contains("messages") || !body["messages"].is_array()) return;
    auto& msgs = body["messages"];
    int marked = 0;
    for (auto it = msgs.rbegin(); it != msgs.rend() && marked < 2; ++it) {
        if (!it->contains("content")) continue;
        json& content = (*it)["content"];
        // String-content messages can't carry cache_control; skip them. The
        // adapter above emits block arrays for everything substantial.
        if (!content.is_array() || content.empty()) continue;
        json& last = content.back();
        if (!last.is_object()) continue;
        last["cache_control"] = kEphemeral;
        ++marked;
    }
}

void LlmClient::abort() {
    if (pendingReply_) {
        QNetworkReply* reply = pendingReply_;
        pendingReply_ = nullptr;
        if (reply == streamReply_)
            streamReply_ = nullptr;
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
        resetStreamState();
        emit errorOccurred("cancelled");
    }
}

void LlmClient::postChatCompletions(json body, bool streaming) {
    if (apiKey_.isEmpty()) {
        emit errorOccurred("DEEPSEEK_API_KEY is not set");
        return;
    }

    QUrl url(baseUrl_ + "/v1/chat/completions");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey_.toUtf8());
    req.setRawHeader("Accept", streaming ? "text/event-stream" : "application/json");
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

    QByteArray payload = QByteArray::fromStdString(body.dump());
    QNetworkReply* reply = nm_.post(req, payload);

    pendingReply_ = reply;

    if (streaming) {
        resetStreamState();
        streamReply_ = reply;
        connect(reply, &QNetworkReply::readyRead, this, &LlmClient::onStreamReadyRead);
        connect(reply, &QNetworkReply::finished,  this, &LlmClient::onStreamFinished);
    } else {
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            if (pendingReply_ == reply)
                pendingReply_.clear();
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                QByteArray errBody = reply->readAll();
                emit errorOccurred(QStringLiteral("HTTP error: %1\n%2")
                                       .arg(reply->errorString(), QString::fromUtf8(errBody)));
                return;
            }
            QByteArray data = reply->readAll();
            try {
                json parsed = json::parse(std::string(data.constData(), data.size()));
                if (!parsed.contains("choices") || parsed["choices"].empty()) {
                    emit errorOccurred("response missing choices: " + QString::fromUtf8(data));
                    return;
                }
                if (parsed.contains("usage") && parsed["usage"].is_object()) {
                    streamUsage_ = json::object();
                    mergeAndEmitUsage(parsed["usage"]);
                }
                emit responseReceived(parsed["choices"][0]["message"]);
            } catch (const std::exception& e) {
                emit errorOccurred(QStringLiteral("JSON parse failed: %1\n%2")
                                       .arg(e.what(), QString::fromUtf8(data)));
            }
        });
    }
}

void LlmClient::postClaudeMessages(json body, bool streaming) {
    if (apiKey_.isEmpty()) {
        emit errorOccurred("ANTHROPIC_API_KEY is not set");
        return;
    }

    QUrl url(baseUrl_ + "/v1/messages");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("x-api-key", apiKey_.toUtf8());
    req.setRawHeader("anthropic-version", kClaudeApiVersion);
    req.setRawHeader("Accept", streaming ? "text/event-stream" : "application/json");
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

    QByteArray payload = QByteArray::fromStdString(body.dump());
    QNetworkReply* reply = nm_.post(req, payload);

    pendingReply_ = reply;

    if (streaming) {
        resetStreamState();
        streamReply_ = reply;
        connect(reply, &QNetworkReply::readyRead, this, &LlmClient::onStreamReadyRead);
        connect(reply, &QNetworkReply::finished,  this, &LlmClient::onStreamFinished);
    } else {
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            if (pendingReply_ == reply)
                pendingReply_.clear();
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                QByteArray errBody = reply->readAll();
                emit errorOccurred(QStringLiteral("HTTP error: %1\n%2")
                                       .arg(reply->errorString(), QString::fromUtf8(errBody)));
                return;
            }
            QByteArray data = reply->readAll();
            try {
                json parsed = json::parse(std::string(data.constData(), data.size()));
                if (parsed.contains("usage") && parsed["usage"].is_object()) {
                    streamUsage_ = json::object();
                    mergeAndEmitUsage(parsed["usage"]);
                }
                emit responseReceived(claudeMessageToOpenAi(parsed));
            } catch (const std::exception& e) {
                emit errorOccurred(QStringLiteral("JSON parse failed: %1\n%2")
                                       .arg(e.what(), QString::fromUtf8(data)));
            }
        });
    }
}

void LlmClient::resetStreamState() {
    streamBuffer_.clear();
    streamContent_.clear();
    streamCalls_.clear();
    streamUsage_ = json::object();
    streamSawDone_ = false;
    streamToolCounter_ = 0;
    streamBlockToTool_.clear();
}

void LlmClient::onStreamReadyRead() {
    if (!streamReply_) return;
    streamBuffer_ += streamReply_->readAll();

    int newlineIdx;
    while ((newlineIdx = streamBuffer_.indexOf('\n')) != -1) {
        QByteArray line = streamBuffer_.left(newlineIdx);
        streamBuffer_.remove(0, newlineIdx + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;
        processSseLine(line);
    }
}

void LlmClient::processSseLine(const QByteArray& line) {
    static const QByteArray kPrefix("data: ");
    if (!line.startsWith(kPrefix)) return;
    QByteArray payload = line.mid(kPrefix.size());
    if (provider_ == QStringLiteral("claude"))
        processClaudeSsePayload(payload);
    else
        processOpenAiSsePayload(payload);
}

void LlmClient::processOpenAiSsePayload(const QByteArray& payload) {
    if (payload == "[DONE]") {
        streamSawDone_ = true;
        return;
    }

    json delta;
    try {
        delta = json::parse(std::string(payload.constData(), payload.size()));
    } catch (const std::exception&) {
        return;
    }

    if (delta.contains("usage") && delta["usage"].is_object())
        mergeAndEmitUsage(delta["usage"]);

    if (!delta.contains("choices") || delta["choices"].empty()) return;
    const json& choice = delta["choices"][0];
    if (!choice.contains("delta")) return;
    const json& d = choice["delta"];

    if (d.contains("content") && d["content"].is_string()) {
        std::string fragment = d["content"].get<std::string>();
        if (!fragment.empty()) {
            streamContent_ += fragment;
            emit tokenDelta(QString::fromStdString(fragment));
        }
    }

    if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
        for (const auto& tc : d["tool_calls"]) {
            int idx = tc.value("index", 0);
            AccumulatedCall& acc = streamCalls_[idx];
            if (tc.contains("id") && tc["id"].is_string()) {
                acc.id = tc["id"].get<std::string>();
            }
            if (tc.contains("function")) {
                const auto& fn = tc["function"];
                std::string newArgsFragment;
                if (fn.contains("name") && fn["name"].is_string()) {
                    acc.name = fn["name"].get<std::string>();
                }
                if (fn.contains("arguments") && fn["arguments"].is_string()) {
                    newArgsFragment = fn["arguments"].get<std::string>();
                    acc.arguments += newArgsFragment;
                }
                emit toolCallDelta(idx,
                                   QString::fromStdString(acc.id),
                                   QString::fromStdString(acc.name),
                                   QString::fromStdString(newArgsFragment));
            }
        }
    }
}

void LlmClient::processClaudeSsePayload(const QByteArray& payload) {
    json event;
    try {
        event = json::parse(std::string(payload.constData(), payload.size()));
    } catch (const std::exception&) {
        return;
    }

    const std::string type = event.value("type", "");
    if (event.contains("usage") && event["usage"].is_object())
        mergeAndEmitUsage(event["usage"]);
    if (type == "message_start"
        && event.contains("message")
        && event["message"].contains("usage")
        && event["message"]["usage"].is_object()) {
        mergeAndEmitUsage(event["message"]["usage"]);
    }

    if (type == "message_stop") {
        streamSawDone_ = true;
        return;
    }
    if (type == "error") {
        const json& err = event.value("error", json::object());
        emit errorOccurred(QStringLiteral("Claude stream error: %1")
                               .arg(QString::fromStdString(err.value("message", "unknown error"))));
        return;
    }
    if (type == "content_block_start") {
        const int idx = event.value("index", 0);
        const json& block = event.value("content_block", json::object());
        const std::string blockType = block.value("type", "");
        if (blockType == "text") {
            std::string text = block.value("text", "");
            if (!text.empty()) {
                streamContent_ += text;
                emit tokenDelta(QString::fromStdString(text));
            }
        } else if (blockType == "tool_use") {
            // Use a monotonic tool-call counter rather than Claude's
            // content-block index (which counts text blocks too).
            int toolIdx = streamToolCounter_++;
            // Remember the mapping from content_block index -> tool ordinal
            // so delta events (which carry content_block index) can find the right tool.
            streamBlockToTool_[idx] = toolIdx;
            AccumulatedCall& acc = streamCalls_[toolIdx];
            acc.id = block.value("id", "");
            acc.name = block.value("name", "");
            if (block.contains("input") && block["input"].is_object() && !block["input"].empty())
                acc.arguments = block["input"].dump();
            emit toolCallDelta(toolIdx,
                               QString::fromStdString(acc.id),
                               QString::fromStdString(acc.name),
                               QString());
        }
        return;
    }
    if (type != "content_block_delta") return;

    const int idx = event.value("index", 0);
    // Map content_block index back to tool ordinal.
    auto it = streamBlockToTool_.find(idx);
    const int toolIdx = (it != streamBlockToTool_.end()) ? it->second : idx;
    const json& delta = event.value("delta", json::object());
    const std::string deltaType = delta.value("type", "");
    if (deltaType == "text_delta") {
        std::string fragment = delta.value("text", "");
        if (!fragment.empty()) {
            streamContent_ += fragment;
            emit tokenDelta(QString::fromStdString(fragment));
        }
        return;
    }
    if (deltaType == "input_json_delta") {
        std::string fragment = delta.value("partial_json", "");
        AccumulatedCall& acc = streamCalls_[toolIdx];
        acc.arguments += fragment;
        emit toolCallDelta(toolIdx,
                           QString::fromStdString(acc.id),
                           QString::fromStdString(acc.name),
                           QString::fromStdString(fragment));
    }
}

void LlmClient::onStreamFinished() {
    if (!streamReply_) return;
    QNetworkReply* reply = streamReply_;
    streamReply_ = nullptr;
    if (pendingReply_ == reply)
        pendingReply_.clear();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError && !streamSawDone_) {
        QByteArray errBody = reply->readAll();
        if (errBody.isEmpty()) {
            errBody = streamBuffer_;
        }
        emit errorOccurred(QStringLiteral("stream HTTP error: %1\n%2")
                               .arg(reply->errorString(), QString::fromUtf8(errBody)));
        return;
    }

    if (!streamBuffer_.isEmpty()) {
        QByteArray tail = streamBuffer_;
        streamBuffer_.clear();
        for (const QByteArray& l : tail.split('\n')) {
            QByteArray clean = l.endsWith('\r') ? l.left(l.size() - 1) : l;
            if (!clean.isEmpty()) processSseLine(clean);
        }
    }

    emitFinalStreamMessage();
}

void LlmClient::emitFinalStreamMessage() {
    json msg = {
        {"role",    "assistant"},
        {"content", streamContent_.empty() ? json(nullptr) : json(streamContent_)}
    };
    if (!streamCalls_.empty()) {
        json arr = json::array();
        for (const auto& [_, acc] : streamCalls_) {
            if (acc.id.empty()) continue;
            arr.push_back({
                {"id",       acc.id},
                {"type",     "function"},
                {"function", {
                    {"name",      acc.name},
                    {"arguments", acc.arguments.empty() ? "{}" : acc.arguments}
                }}
            });
        }
        if (!arr.empty())
            msg["tool_calls"] = std::move(arr);
    }
    emit streamFinished(std::move(msg));
}

void LlmClient::mergeAndEmitUsage(const json& usage) {
    if (!usage.is_object()) return;

    if (!streamUsage_.is_object())
        streamUsage_ = json::object();
    for (const auto& item : usage.items()) {
        streamUsage_[item.key()] = item.value();
    }

    const bool hasDeepSeekCache =
        streamUsage_.contains("prompt_cache_hit_tokens")
        || streamUsage_.contains("prompt_cache_miss_tokens");
    const bool hasAnthropicCache =
        streamUsage_.contains("cache_read_input_tokens")
        || streamUsage_.contains("cache_creation_input_tokens")
        || streamUsage_.contains("cache_creation");

    int cachedInputTokens = 0;
    int uncachedInputTokens = 0;
    if (hasDeepSeekCache) {
        cachedInputTokens = usageInt(streamUsage_, "prompt_cache_hit_tokens");
        uncachedInputTokens = usageInt(streamUsage_, "prompt_cache_miss_tokens");
    } else {
        cachedInputTokens = usageInt(streamUsage_, "cache_read_input_tokens");
        uncachedInputTokens = usageInt(streamUsage_, "uncached_input_tokens");
        if (uncachedInputTokens == 0) {
            uncachedInputTokens = usageInt(streamUsage_, "input_tokens");
            if (hasAnthropicCache)
                uncachedInputTokens += anthropicCacheCreationTokens(streamUsage_);
        } else {
            uncachedInputTokens += anthropicCacheCreationTokens(streamUsage_);
        }
        if (cachedInputTokens == 0 && streamUsage_.contains("prompt_tokens"))
            uncachedInputTokens = usageInt(streamUsage_, "prompt_tokens");
    }

    const int outputTokens = streamUsage_.contains("completion_tokens")
        ? usageInt(streamUsage_, "completion_tokens")
        : usageInt(streamUsage_, "output_tokens");
    int totalTokens = usageInt(streamUsage_, "total_tokens");
    if (totalTokens == 0)
        totalTokens = cachedInputTokens + uncachedInputTokens + outputTokens;

    emit tokenUsage(cachedInputTokens, uncachedInputTokens, outputTokens, totalTokens);
}

json LlmClient::claudeMessageToOpenAi(const json& message) const {
    std::string text;
    json toolCalls = json::array();

    if (message.contains("content") && message["content"].is_array()) {
        int index = 0;
        for (const auto& block : message["content"]) {
            const std::string type = block.value("type", "");
            if (type == "text") {
                text += block.value("text", "");
            } else if (type == "tool_use") {
                toolCalls.push_back({
                    {"id", block.value("id", "")},
                    {"type", "function"},
                    {"function", {
                        {"name", block.value("name", "")},
                        {"arguments", block.value("input", json::object()).dump()}
                    }},
                    {"index", index++}
                });
            }
        }
    }

    json out = {
        {"role", "assistant"},
        {"content", text.empty() ? json(nullptr) : json(text)}
    };
    if (!toolCalls.empty())
        out["tool_calls"] = std::move(toolCalls);
    return out;
}
