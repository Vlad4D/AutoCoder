#pragma once

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

// Settings dialog. Values are persisted via QSettings.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    static QString loadProvider();         // QSettings -> "deepseek"
    static QString loadApiKey();           // Provider QSettings -> provider env -> empty
    static QString loadModel();            // Provider QSettings -> provider default
    static QString loadBaseUrl();          // Provider QSettings -> provider default
    static int     loadMaxIterations();    // QSettings -> 25
    static int     loadBashTimeoutMs();    // QSettings -> 120000
    static QString loadCheckCommand();     // QSettings -> empty (auto-verify off)
    static int     loadMaxContextTokens(); // QSettings -> 1048576
    static int     loadSendTokens();       // QSettings -> 16384
    static int     loadFontSize();         // QSettings -> 12

    // Reasoning-guidance toggle (persisted; not part of the dialog form).
    static bool    loadReasoningGuidance();      // QSettings -> true
    static void    saveReasoningGuidance(bool enabled);

private slots:
    void onAccepted();

private:
    void refreshProviderFields(const QString& provider);
    void resetModelChoices(const QString& provider, const QString& selectedModel);

    QComboBox* providerCombo_ = nullptr;
    QLineEdit* apiKeyEdit_ = nullptr;
    QComboBox* modelCombo_ = nullptr;
    QLineEdit* baseUrlEdit_ = nullptr;
    QSpinBox*  maxIterSpin_ = nullptr;
    QSpinBox*  bashTimeoutSpin_ = nullptr;
    QLineEdit* checkCommandEdit_ = nullptr;
    QSpinBox*  maxContextTokensSpin_ = nullptr;
    QSpinBox*  sendTokensSpin_ = nullptr;
    QSpinBox*  fontSizeSpin_ = nullptr;
};
