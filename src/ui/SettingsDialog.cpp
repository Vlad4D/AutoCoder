#include "SettingsDialog.h"

#include <cstdlib>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include "persistence/SecretStore.h"

namespace {
const char* kProviderKey       = "llm/provider";
const char* kDeepSeekApiKeyKey = "deepseek/apiKey";
const char* kDeepSeekModelKey  = "deepseek/model";
const char* kDeepSeekBaseUrlKey= "deepseek/baseUrl";
const char* kClaudeApiKeyKey   = "claude/apiKey";
const char* kClaudeModelKey    = "claude/model";
const char* kClaudeBaseUrlKey  = "claude/baseUrl";
const char* kClaudeMaxContextTokensKey = "claude/maxContextTokens";
const char* kClaudeSendTokensKey = "claude/sendTokens";
const char* kDeepSeekTemperatureKey = "deepseek/temperature";
const char* kClaudeTemperatureKey   = "claude/temperature";
const char* kMaxIterKey        = "agent/maxIterations";
const char* kBashTimeoutKey    = "agent/bashTimeoutMs";
const char* kCheckCommandKey   = "agent/checkCommand";
const char* kMaxContextTokensKey= "agent/maxContextTokens";
const char* kMaxContextMigratedKey = "agent/maxContextTokensMigratedTo1M";
const char* kSendTokensKey     = "agent/sendTokens";
const char* kFontSizeKey       = "ui/fontSize";
const char* kReasoningGuidanceKey = "ui/reasoningGuidance";

constexpr const char* kDeepSeekProvider = "deepseek";
constexpr const char* kClaudeProvider = "claude";
constexpr const char* kDefaultDeepSeekBaseUrl = "https://api.deepseek.com";
constexpr const char* kDefaultClaudeBaseUrl = "https://api.anthropic.com";
constexpr const char* kDefaultDeepSeekModel = "deepseek-chat";
constexpr const char* kDefaultClaudeModel = "claude-sonnet-4-6";
constexpr int kDefaultMaxIter         = 150;
constexpr int kDefaultBashTimeoutMs   = 120000;
constexpr int kDefaultMaxContextTokens= 507904;
constexpr int kDefaultClaudeMaxContextTokens = 500000;
constexpr int kDefaultSendTokens      = 16384;
constexpr int kDefaultFontSize        = 12;
// DeepSeek's API default temperature is 1.0 -- far too high for coding; its
// docs recommend 0.0 for coding/math. Claude's agentic default is fine, so
// leave it at "provider default" (negative sentinel = omit from requests).
constexpr double kDefaultDeepSeekTemperature = 0.0;
constexpr double kProviderDefaultTemperature = -0.1;  // sentinel: omit field

QString normalizedProvider(QString provider) {
    provider = provider.trimmed().toLower();
    if (provider == QStringLiteral("anthropic"))
        return QString::fromUtf8(kClaudeProvider);
    if (provider != QString::fromUtf8(kClaudeProvider))
        return QString::fromUtf8(kDeepSeekProvider);
    return provider;
}

bool isClaude(const QString& provider) {
    return normalizedProvider(provider) == QString::fromUtf8(kClaudeProvider);
}

const char* apiKeyKeyFor(const QString& provider) {
    return isClaude(provider) ? kClaudeApiKeyKey : kDeepSeekApiKeyKey;
}

const char* modelKeyFor(const QString& provider) {
    return isClaude(provider) ? kClaudeModelKey : kDeepSeekModelKey;
}

const char* baseUrlKeyFor(const QString& provider) {
    return isClaude(provider) ? kClaudeBaseUrlKey : kDeepSeekBaseUrlKey;
}

const char* maxContextTokensKeyFor(const QString& provider) {
    return isClaude(provider) ? kClaudeMaxContextTokensKey : kMaxContextTokensKey;
}

const char* sendTokensKeyFor(const QString& provider) {
    return isClaude(provider) ? kClaudeSendTokensKey : kSendTokensKey;
}

const char* temperatureKeyFor(const QString& provider) {
    return isClaude(provider) ? kClaudeTemperatureKey : kDeepSeekTemperatureKey;
}

double defaultTemperatureFor(const QString& provider) {
    return isClaude(provider) ? kProviderDefaultTemperature : kDefaultDeepSeekTemperature;
}

const char* defaultModelFor(const QString& provider) {
    return isClaude(provider) ? kDefaultClaudeModel : kDefaultDeepSeekModel;
}

const char* defaultBaseUrlFor(const QString& provider) {
    return isClaude(provider) ? kDefaultClaudeBaseUrl : kDefaultDeepSeekBaseUrl;
}

int defaultMaxContextTokensFor(const QString& provider) {
    return isClaude(provider) ? kDefaultClaudeMaxContextTokens : kDefaultMaxContextTokens;
}

QString loadApiKeyForProvider(const QString& provider) {
    // 1. OS secure store (Windows Credential Manager).
    QString k = secretstore::load(provider);

    // 2. Legacy plaintext QSettings -> migrate into the secure store, then purge.
    if (k.isEmpty()) {
        QSettings s;
        const QString legacy = s.value(apiKeyKeyFor(provider)).toString();
        if (!legacy.isEmpty()) {
            if (secretstore::store(provider, legacy)) {
                s.remove(apiKeyKeyFor(provider));
            }
            k = legacy;
        }
    }

    // 3. Environment variable fallback.
    if (k.isEmpty() && !isClaude(provider)) {
        const char* env = std::getenv("DEEPSEEK_API_KEY");
        if (env && *env) k = QString::fromUtf8(env);
    }
    if (k.isEmpty() && isClaude(provider)) {
        const char* env = std::getenv("ANTHROPIC_API_KEY");
        if (env && *env) k = QString::fromUtf8(env);
    }
    return k;
}

QString loadModelForProvider(const QString& provider) {
    QSettings s;
    return s.value(modelKeyFor(provider), QString::fromUtf8(defaultModelFor(provider))).toString();
}

QString loadBaseUrlForProvider(const QString& provider) {
    QSettings s;
    return s.value(baseUrlKeyFor(provider), QString::fromUtf8(defaultBaseUrlFor(provider))).toString();
}

int loadMaxContextTokensForProvider(const QString& provider) {
    QSettings s;
    const int defaultValue = defaultMaxContextTokensFor(provider);
    const int value = s.value(maxContextTokensKeyFor(provider), defaultValue).toInt();
    if (!isClaude(provider)) {
        const bool migrated = s.value(kMaxContextMigratedKey, false).toBool();
        if (s.contains(kMaxContextTokensKey) && value == 131072 && !migrated) {
            s.setValue(kMaxContextTokensKey, kDefaultMaxContextTokens);
            s.setValue(kMaxContextMigratedKey, true);
            return kDefaultMaxContextTokens;
        }
    }
    return value;
}

int loadSendTokensForProvider(const QString& provider) {
    QSettings s;
    return s.value(sendTokensKeyFor(provider), kDefaultSendTokens).toInt();
}

double loadTemperatureForProvider(const QString& provider) {
    QSettings s;
    return s.value(temperatureKeyFor(provider), defaultTemperatureFor(provider)).toDouble();
}

// One-time, per-provider notice about where data is processed, so consent is
// informed. Shown the first time a provider is saved.
void maybeShowProviderDataNotice(QWidget* parent, const QString& provider) {
    QSettings s;
    const QString flag = QStringLiteral("notice/providerDataShown/") + normalizedProvider(provider);
    if (s.value(flag, false).toBool()) return;

    const bool claude = isClaude(provider);
    const QString name = claude ? QStringLiteral("Anthropic (Claude)")
                                : QStringLiteral("DeepSeek");
    const QString url  = claude ? QStringLiteral("https://www.anthropic.com/legal/privacy")
                                : QStringLiteral("https://www.deepseek.com/privacy");

    QMessageBox box(parent);
    box.setWindowTitle(QStringLiteral("Data processing notice"));
    box.setIcon(QMessageBox::Information);
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("You selected <b>%1</b> as your LLM provider.").arg(name));
    box.setInformativeText(QStringLiteral(
        "Your prompts, the file excerpts the agent reads, and command output are sent "
        "to %1's API for processing. Don't use it with data you aren't permitted to "
        "share.<br><br>Privacy policy: <a href=\"%2\">%2</a>").arg(name, url));
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();

    s.setValue(flag, true);
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("AutoCoder settings"));
    setMinimumWidth(520);

    providerCombo_ = new QComboBox(this);
    providerCombo_->addItem(QStringLiteral("DeepSeek"), QString::fromUtf8(kDeepSeekProvider));
    providerCombo_->addItem(QStringLiteral("Claude"), QString::fromUtf8(kClaudeProvider));
    const QString provider = loadProvider();
    int providerIdx = providerCombo_->findData(provider);
    if (providerIdx >= 0) providerCombo_->setCurrentIndex(providerIdx);

    apiKeyEdit_ = new QLineEdit(this);
    apiKeyEdit_->setEchoMode(QLineEdit::Password);

    modelCombo_ = new QComboBox(this);
    modelCombo_->setEditable(true);

    baseUrlEdit_ = new QLineEdit(this);

    refreshProviderFields(providerCombo_->currentData().toString());
    connect(providerCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        refreshProviderFields(providerCombo_->currentData().toString());
    });

    maxIterSpin_ = new QSpinBox(this);
    maxIterSpin_->setRange(1, 200);
    maxIterSpin_->setValue(loadMaxIterations());

    bashTimeoutSpin_ = new QSpinBox(this);
    bashTimeoutSpin_->setRange(1000, 3600000);
    bashTimeoutSpin_->setSingleStep(10000);
    bashTimeoutSpin_->setSuffix(QStringLiteral(" ms"));
    bashTimeoutSpin_->setValue(loadBashTimeoutMs());

    checkCommandEdit_ = new QLineEdit(this);
    checkCommandEdit_->setText(loadCheckCommand());
    checkCommandEdit_->setPlaceholderText(QStringLiteral("e.g. cmake --build build (empty = disabled)"));
    checkCommandEdit_->setToolTip(QStringLiteral(
        "Auto-verify: run this command after every agent turn that modified files.\n"
        "If it fails, the output is sent back to the model so it can fix its own\n"
        "errors (at most 2 attempts per turn). Runs without the approval prompt."));

    maxContextTokensSpin_ = new QSpinBox(this);
    maxContextTokensSpin_->setRange(4096, 1048576);
    maxContextTokensSpin_->setSingleStep(16384);
    maxContextTokensSpin_->setSuffix(QStringLiteral(" tokens"));
    maxContextTokensSpin_->setValue(loadMaxContextTokens());

    sendTokensSpin_ = new QSpinBox(this);
    sendTokensSpin_->setRange(512, 1000000);
    sendTokensSpin_->setSingleStep(1024);
    sendTokensSpin_->setSuffix(QStringLiteral(" tokens"));
    sendTokensSpin_->setValue(loadSendTokens());

    temperatureSpin_ = new QDoubleSpinBox(this);
    temperatureSpin_->setRange(kProviderDefaultTemperature,
                               isClaude(loadProvider()) ? 1.0 : 2.0);
    temperatureSpin_->setDecimals(1);
    temperatureSpin_->setSingleStep(0.1);
    temperatureSpin_->setSpecialValueText(QStringLiteral("provider default"));
    temperatureSpin_->setValue(loadTemperature());
    temperatureSpin_->setToolTip(QStringLiteral(
        "Sampling temperature sent with every request.\n"
        "0.0 = most deterministic (recommended for coding, especially DeepSeek).\n"
        "Set to \"provider default\" to omit the parameter."));

    fontSizeSpin_ = new QSpinBox(this);
    fontSizeSpin_->setRange(8, 32);
    fontSizeSpin_->setSuffix(QStringLiteral(" pt"));
    fontSizeSpin_->setValue(loadFontSize());

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Provider"), providerCombo_);
    form->addRow(QStringLiteral("API key"), apiKeyEdit_);
    form->addRow(QStringLiteral("Model"), modelCombo_);
    form->addRow(QStringLiteral("Base URL"), baseUrlEdit_);
    form->addRow(QStringLiteral("Temperature"), temperatureSpin_);
    form->addRow(QStringLiteral("Max agent iterations"), maxIterSpin_);
    form->addRow(QStringLiteral("Default bash timeout"), bashTimeoutSpin_);
    form->addRow(QStringLiteral("Check command (auto-verify)"), checkCommandEdit_);

    auto* tokenLabel = new QLabel(QStringLiteral(
        "<b>Context budget</b> - when the conversation exceeds "
        "<tt>max context</tt> - <tt>reserved send</tt> tokens, "
        "the oldest exchanges are automatically trimmed."), this);
    tokenLabel->setWordWrap(true);
    form->addRow(tokenLabel);
    form->addRow(QStringLiteral("Max context tokens"), maxContextTokensSpin_);
    form->addRow(QStringLiteral("Reserved for send"), sendTokensSpin_);

    form->addRow(QStringLiteral("Font size"), fontSizeSpin_);

    auto* hint = new QLabel(
        QStringLiteral("DeepSeek uses OpenAI-compatible chat completions. "
                       "Claude uses Anthropic Messages API. If the API key is empty, "
                       "AutoCoder reads DEEPSEEK_API_KEY or ANTHROPIC_API_KEY for the selected provider."),
        this);
    hint->setWordWrap(true);
    QFont f = hint->font(); f.setPointSizeF(f.pointSizeF() * 0.9); hint->setFont(f);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addStretch(1);
    layout->addWidget(buttons);
}

void SettingsDialog::refreshProviderFields(const QString& provider) {
    apiKeyEdit_->setPlaceholderText(isClaude(provider)
        ? QStringLiteral("sk-ant-...")
        : QStringLiteral("sk-..."));
    apiKeyEdit_->setText(loadApiKeyForProvider(provider));
    resetModelChoices(provider, loadModelForProvider(provider));
    baseUrlEdit_->setText(loadBaseUrlForProvider(provider));
    baseUrlEdit_->setPlaceholderText(QString::fromUtf8(defaultBaseUrlFor(provider)));
    if (maxContextTokensSpin_)
        maxContextTokensSpin_->setValue(loadMaxContextTokensForProvider(provider));
    if (sendTokensSpin_)
        sendTokensSpin_->setValue(loadSendTokensForProvider(provider));
    if (temperatureSpin_) {
        // Anthropic's temperature range tops out at 1.0; DeepSeek/OpenAI at
        // 2.0. Match the spin box so the UI can't offer a value the request
        // would silently clamp away. Re-set the value after the range change,
        // since setRange may have clipped it.
        temperatureSpin_->setRange(kProviderDefaultTemperature,
                                   isClaude(provider) ? 1.0 : 2.0);
        temperatureSpin_->setValue(loadTemperatureForProvider(provider));
    }
}

void SettingsDialog::resetModelChoices(const QString& provider, const QString& selectedModel) {
    modelCombo_->clear();
    if (isClaude(provider)) {
        modelCombo_->addItem(QStringLiteral("claude-opus-4-8"));
        modelCombo_->addItem(QStringLiteral("claude-opus-4-7"));
        modelCombo_->addItem(QStringLiteral("claude-opus-4-6"));
        modelCombo_->addItem(QStringLiteral("claude-sonnet-4-6"));
        modelCombo_->addItem(QStringLiteral("claude-sonnet-4-5"));
        modelCombo_->addItem(QStringLiteral("claude-sonnet-4-5-20250929"));
        modelCombo_->addItem(QStringLiteral("claude-haiku-4-5"));
        modelCombo_->addItem(QStringLiteral("claude-haiku-4-5-20251001"));
    } else {
        modelCombo_->addItem(QStringLiteral("deepseek-chat"));
        modelCombo_->addItem(QStringLiteral("deepseek-reasoner"));
    }
    int idx = modelCombo_->findText(selectedModel);
    if (idx >= 0) modelCombo_->setCurrentIndex(idx);
    else modelCombo_->setCurrentText(selectedModel);
}

void SettingsDialog::onAccepted() {
    const QString provider = providerCombo_->currentData().toString();
    QSettings s;
    s.setValue(kProviderKey, provider);
    // The API key goes to the OS secure store, never plaintext QSettings.
    secretstore::store(provider, apiKeyEdit_->text());
    s.remove(apiKeyKeyFor(provider));  // purge any legacy plaintext value
    s.setValue(modelKeyFor(provider), modelCombo_->currentText());
    s.setValue(baseUrlKeyFor(provider),
               baseUrlEdit_->text().isEmpty()
                   ? QString::fromUtf8(defaultBaseUrlFor(provider))
                   : baseUrlEdit_->text());
    s.setValue(kMaxIterKey,          maxIterSpin_->value());
    s.setValue(kBashTimeoutKey,      bashTimeoutSpin_->value());
    s.setValue(kCheckCommandKey,     checkCommandEdit_->text().trimmed());
    s.setValue(maxContextTokensKeyFor(provider), maxContextTokensSpin_->value());
    if (!isClaude(provider))
        s.setValue(kMaxContextMigratedKey, true);
    s.setValue(sendTokensKeyFor(provider), sendTokensSpin_->value());
    s.setValue(temperatureKeyFor(provider), temperatureSpin_->value());
    s.setValue(kFontSizeKey,         fontSizeSpin_->value());

    maybeShowProviderDataNotice(this, provider);
    accept();
}

QString SettingsDialog::loadProvider() {
    QSettings s;
    return normalizedProvider(s.value(kProviderKey, QString::fromUtf8(kDeepSeekProvider)).toString());
}

QString SettingsDialog::loadApiKey() {
    return loadApiKeyForProvider(loadProvider());
}

QString SettingsDialog::loadModel() {
    return loadModelForProvider(loadProvider());
}

QString SettingsDialog::loadBaseUrl() {
    return loadBaseUrlForProvider(loadProvider());
}

int SettingsDialog::loadMaxIterations() {
    QSettings s;
    return s.value(kMaxIterKey, kDefaultMaxIter).toInt();
}

int SettingsDialog::loadBashTimeoutMs() {
    QSettings s;
    return s.value(kBashTimeoutKey, kDefaultBashTimeoutMs).toInt();
}

QString SettingsDialog::loadCheckCommand() {
    QSettings s;
    return s.value(kCheckCommandKey).toString().trimmed();
}

int SettingsDialog::loadMaxContextTokens() {
    return loadMaxContextTokensForProvider(loadProvider());
}

int SettingsDialog::loadSendTokens() {
    return loadSendTokensForProvider(loadProvider());
}

double SettingsDialog::loadTemperature() {
    return loadTemperatureForProvider(loadProvider());
}

int SettingsDialog::loadFontSize() {
    QSettings s;
    return s.value(kFontSizeKey, kDefaultFontSize).toInt();
}

bool SettingsDialog::loadReasoningGuidance() {
    QSettings s;
    return s.value(kReasoningGuidanceKey, true).toBool();
}

void SettingsDialog::saveReasoningGuidance(bool enabled) {
    QSettings s;
    s.setValue(kReasoningGuidanceKey, enabled);
}
