#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>

#include "agent/AgentRunner.h"
#include "agent/SystemPrompt.h"
#include "diagnostics/CrashHandler.h"
#include "tools/PathUtil.h"

#include "AskUserWidget.h"
#include "ChatView.h"
#include "InputBar.h"
#include "SettingsDialog.h"
#include "ToolCallWidget.h"

#include "build_date.h"
#include "diagnostics/TraceTimer.h"

namespace fs = std::filesystem;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString("AutoCoder [built %1 @ %2]").arg(kBuildDate, kBuildTime));
    // Initial guidance check for cwd -- this is a placeholder; setProjectRoot
    // will update the title once the project is fully initialized.
    if (auto guidanceFile = SystemPrompt::findProjectGuidanceFile(fs::current_path())) {
        const QString guidanceName =
            QString::fromUtf8(pathutil::toUtf8(guidanceFile->filename()).c_str());
        setWindowTitle(windowTitle() + QStringLiteral(" - guidance: %1").arg(guidanceName));
    }
    resize(1300, 850);

    // ----- Sidebar -----
    sidebar_ = new QListWidget(this);
    sidebar_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    sidebar_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sidebar_, &QListWidget::itemActivated, this, &MainWindow::onSidebarItemClicked);
    connect(sidebar_, &QListWidget::itemClicked,   this, &MainWindow::onSidebarItemClicked);
    connect(sidebar_, &QListWidget::customContextMenuRequested,
            this, &MainWindow::onSidebarContextMenu);

    // Delete key removes the selected conversation(s) when the sidebar has focus.
    auto* deleteShortcut = new QAction(this);
    deleteShortcut->setShortcut(QKeySequence::Delete);
    deleteShortcut->setShortcutContext(Qt::WidgetShortcut);
    sidebar_->addAction(deleteShortcut);
    connect(deleteShortcut, &QAction::triggered, this, &MainWindow::deleteSelectedConversations);

    auto* newBtn = new QPushButton(QStringLiteral("+ New conversation"), this);
    connect(newBtn, &QPushButton::clicked, this, &MainWindow::onNewConversation);

    auto* sideCol = new QWidget(this);
    auto* sideLayout = new QVBoxLayout(sideCol);
    sideLayout->setContentsMargins(8, 8, 8, 8);
    sideLayout->addWidget(newBtn);
    sideLayout->addWidget(sidebar_, 1);

    // ----- Chat column -----
    chat_  = new ChatView(this);
    input_ = new InputBar(this);

    auto* chatCol = new QWidget(this);
    auto* col = new QVBoxLayout(chatCol);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(chat_, 1);
    col->addWidget(input_, 0);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(sideCol);
    split->addWidget(chatCol);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes(QList<int>{ 260, 1040 });
    setCentralWidget(split);

    connect(input_, &InputBar::send, this, &MainWindow::onSendUserMessage);
    connect(input_, &InputBar::stop, this, &MainWindow::onStop);
    connect(input_, &InputBar::compactRequested, this, [this]() {
        emit requestCompactConversation();
    });
    // ----- Status bar context label -----
    contextLabel_ = new QLabel(this);
    contextLabel_->setToolTip(QStringLiteral(
        "Approximate size of the conversation context that is sent to the model on each turn.\n"
        "Tokens are estimated as bytes / 4. The percentage uses the configured max context minus reserved send tokens.\n"
        "Last request usage is reported by the provider when available and separates cached from uncached input tokens."));
    statusBar()->addPermanentWidget(contextLabel_);
    replayProgress_ = new QProgressBar(this);
    replayProgress_->setTextVisible(true);
    replayProgress_->setFixedWidth(220);
    replayProgress_->hide();
    statusBar()->addPermanentWidget(replayProgress_);

    // ----- Worker thread + agent -----
    worker_ = new QThread(this);
    agent_  = new AgentRunner;            // no parent so moveToThread is unambiguous
    agent_->moveToThread(worker_);
    connect(worker_, &QThread::finished, agent_, &QObject::deleteLater);
    worker_->start();

    // Outbound (UI -> agent) -- queued automatically because of thread affinity.
    connect(this, &MainWindow::requestSetProvider,        agent_, &AgentRunner::setProvider);
    connect(this, &MainWindow::requestSetApiKey,          agent_, &AgentRunner::setApiKey);
    connect(this, &MainWindow::requestSetModel,           agent_, &AgentRunner::setModel);
    connect(this, &MainWindow::requestSetBaseUrl,         agent_, &AgentRunner::setBaseUrl);
    connect(this, &MainWindow::requestSetMaxIterations,   agent_, &AgentRunner::setMaxIterations);
    connect(this, &MainWindow::requestSetBashTimeoutMs,   agent_, &AgentRunner::setDefaultBashTimeoutMs);
    connect(this, &MainWindow::requestSetCheckCommand,    agent_, &AgentRunner::setCheckCommand);
    connect(this, &MainWindow::requestSetMaxContextTokens,agent_, &AgentRunner::setMaxContextTokens);
    connect(this, &MainWindow::requestSetSendTokens,      agent_, &AgentRunner::setSendTokens);
    connect(this, &MainWindow::requestSetReasoningGuidance, agent_, &AgentRunner::setReasoningGuidance);
    connect(this, &MainWindow::requestSetProject,         agent_, &AgentRunner::setProject);
    connect(this, &MainWindow::requestNewConversation,    agent_, &AgentRunner::newConversation);
    connect(this, &MainWindow::requestLoadConversation,   agent_, &AgentRunner::loadConversation);
    connect(this, &MainWindow::requestDeleteConversation, agent_, &AgentRunner::deleteConversation);
    connect(this, &MainWindow::requestDeleteConversations, agent_, &AgentRunner::deleteConversations);
    connect(this, &MainWindow::requestSubmitUserMessage,  agent_, &AgentRunner::submitUserMessage);
    connect(this, &MainWindow::requestCancel,             agent_, &AgentRunner::cancel);
    // Also flip the atomic cancel flag synchronously on the UI thread, so a tool
    // running on the (now-blocked) worker thread observes cancellation immediately
    // rather than waiting for the queued cancel() slot to run.
    connect(this, &MainWindow::requestCancel,             agent_, &AgentRunner::flagCancelled,
            Qt::DirectConnection);
    connect(this, &MainWindow::requestRevertToCheckpoint, agent_, &AgentRunner::revertToCheckpoint);
    connect(this, &MainWindow::requestCompactConversation, agent_, &AgentRunner::compactConversation);
    connect(this, &MainWindow::requestLoadEarlierConversationHistory,
            agent_, &AgentRunner::loadEarlierConversationHistory);

    // Inbound (agent -> UI) -- also queued.
    connect(agent_, &AgentRunner::conversationCleared,    this, &MainWindow::onConversationCleared);
    connect(agent_, &AgentRunner::conversationReplayStarted,
            this, &MainWindow::onConversationReplayStarted);
    connect(agent_, &AgentRunner::conversationReplayProgress,
            this, &MainWindow::onConversationReplayProgress);
    connect(agent_, &AgentRunner::conversationReplayHistoryHidden,
            chat_, &ChatView::appendHiddenHistoryNotice);
    connect(agent_, &AgentRunner::conversationReplayFinished,
            this, &MainWindow::onConversationReplayFinished);
    connect(agent_, &AgentRunner::userMessageAdded,       this, &MainWindow::onUserMessageAdded);
    connect(agent_, &AgentRunner::assistantTextDelta,     this, &MainWindow::onAssistantTextDelta);
    connect(agent_, &AgentRunner::assistantTextFinalized, this, &MainWindow::onAssistantTextFinalized);
    connect(agent_, &AgentRunner::toolCallStarted,        this, &MainWindow::onToolCallStarted);
    connect(agent_, &AgentRunner::toolCallOutputDelta,    this, &MainWindow::onToolCallOutputDelta);
    connect(agent_, &AgentRunner::toolCallFinished,       this, &MainWindow::onToolCallFinished);
    connect(agent_, &AgentRunner::turnFinished,           this, &MainWindow::onTurnFinished);
    connect(agent_, &AgentRunner::errorOccurred,          this, &MainWindow::onAgentError);
    connect(agent_, &AgentRunner::contextStats,           this, &MainWindow::onContextStats);
    connect(agent_, &AgentRunner::tokenUsageStats,        this, &MainWindow::onTokenUsageStats);
    connect(agent_, &AgentRunner::conversationsListed,    this, &MainWindow::onConversationsListed);
    connect(agent_, &AgentRunner::conversationTrimmed,    this, &MainWindow::onConversationTrimmed);
    connect(agent_, &AgentRunner::streamRetrying, this, [this](int attempt, int maxAttempts) {
        chat_->discardActiveAssistant();   // drop the partial reply before retrying
        statusBar()->showMessage(
            QStringLiteral("Connection dropped — retrying (%1/%2)…").arg(attempt).arg(maxAttempts),
            8000);
    });
    connect(agent_, &AgentRunner::manyFilesChanged, this, [this](int count) {
        statusBar()->showMessage(
            QStringLiteral("Heads up: the agent has modified %1 files this turn. "
                           "Review the changes; you can revert this turn if needed.")
                .arg(count),
            10000);
    });
    connect(agent_, &AgentRunner::userInputRequired,      this, &MainWindow::onUserInputRequired);
    connect(agent_, &AgentRunner::commandApprovalRequired, this, &MainWindow::onCommandApprovalRequired);
    connect(agent_, &AgentRunner::commandApprovalResolved, this,
            [this](const QString& /*command*/, const QString& /*reason*/,
                   const QString& /*explanation*/, const QString& decision) {
                // Attach the resolved decision badge to the last completed tool call,
                // instead of creating a separate widget.
                if (lastToolCall_) {
                    lastToolCall_->setApprovalResolved(decision);
                    lastToolCall_ = nullptr;
                }
            });
    connect(agent_, &AgentRunner::compactStarted,         this, &MainWindow::onCompactStarted);
    connect(agent_, &AgentRunner::compactFinished,        this, &MainWindow::onCompactFinished);
    connect(agent_, &AgentRunner::compactSummaryAppended, chat_, &ChatView::appendCompactSummary);
    connect(this,   &MainWindow::requestProvideUserInput, agent_, &AgentRunner::provideUserInput);
    connect(this,   &MainWindow::requestProvideCommandDecision, agent_, &AgentRunner::provideCommandDecision);
    connect(chat_, &ChatView::loadEarlierRequested, this, [this]() {
        scrollToTopAfterReplay_ = true;
        showConversationLoading();
        emit requestLoadEarlierConversationHistory();
    });

    // Apply persisted settings, then point at the cwd and optionally restore
    // the last conversation.
    pushSettingsToAgent();

    // Check for a last-used conversation to restore before creating an empty one.
    QSettings settings;
    QString restoreId = settings.value("ui/lastConversationId").toString();

    if (!restoreId.isEmpty()) {
        // Restore the last conversation. Mark startingUp_ so the intermediate
        // conversationCleared signal (from newConversation inside setProject)
        // does not clear the chat view. The loaded conversation's replay will
        // fill the chat when it arrives.
        startingUp_ = true;
        setProjectRoot(fs::current_path());  // set window title before agent processes request
        const QString root = QString::fromUtf8(pathutil::toUtf8(fs::current_path()).c_str());
        currentConversationId_ = restoreId;
        showConversationLoading();
        emit requestCancel();
        emit requestSetProject(root);
        emit requestLoadConversation(restoreId);
    } else {
        // No conversation to restore: set up the project normally (creates empty
        // new conversation).
        setProjectRoot(fs::current_path());
    }

    applyFontSize();
    buildMenus();
    statusBar()->showMessage(QStringLiteral("Ready"));
    input_->focusComposer();
}

MainWindow::~MainWindow() {
    // Sever every connection that could call back into this window during
    // teardown. Two sources trip Qt's "class destructor may have already
    // run" assert (qobjectdefs_impl.h): child widgets emitting cleanup
    // signals while being destroyed, and the worker-thread agent activating
    // connections concurrently if it outlives the wait below. Both would
    // invoke a slot on a partially destructed MainWindow.
    if (agent_) QObject::disconnect(agent_, nullptr, this, nullptr);
    for (QObject* sender : findChildren<QObject*>()) {
        QObject::disconnect(sender, nullptr, this, nullptr);
    }

    if (worker_) {
        if (agent_) {
            agent_->flagCancelled();  // atomic; safe to call across threads
            QMetaObject::invokeMethod(agent_, "cancel", Qt::QueuedConnection);
        }
        worker_->quit();
        if (!worker_->wait(10000)) {
            // Last resort: a runaway worker must not outlive the window and
            // touch destroyed objects during QApplication teardown.
            worker_->terminate();
            worker_->wait(2000);
        }
    }
}

void MainWindow::buildMenus() {
    QMenu* projectMenu = menuBar()->addMenu(QStringLiteral("&Project"));

    QAction* openAction = projectMenu->addAction(QStringLiteral("&Open folder..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenProject);

    QAction* newConv = projectMenu->addAction(QStringLiteral("&New conversation"));
    newConv->setShortcut(QKeySequence::New);
    connect(newConv, &QAction::triggered, this, &MainWindow::onNewConversation);

    projectMenu->addSeparator();
    QAction* quit = projectMenu->addAction(QStringLiteral("E&xit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QMainWindow::close);

    QMenu* prefs = menuBar()->addMenu(QStringLiteral("&Preferences"));
    QAction* settings = prefs->addAction(QStringLiteral("&Settings..."));
    connect(settings, &QAction::triggered, this, [this]() {
        SettingsDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            pushSettingsToAgent();
            applyFontSize();
            statusBar()->showMessage(QStringLiteral("Settings saved"));
        }
    });

    // Reasoning guidance: include step-by-step planning guidance in the system
    // prompt (helps smaller models). Persisted; applied to the next LLM request.
    QAction* reasoning = prefs->addAction(QStringLiteral("Reasoning &guidance"));
    reasoning->setCheckable(true);
    reasoning->setChecked(SettingsDialog::loadReasoningGuidance());
    reasoning->setToolTip(QStringLiteral(
        "Include reasoning guidance in the system prompt (helps smaller models "
        "plan step-by-step). Applies to the next LLM request."));
    connect(reasoning, &QAction::toggled, this, [this](bool enabled) {
        SettingsDialog::saveReasoningGuidance(enabled);
        emit requestSetReasoningGuidance(enabled);
    });

    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* openLogs = help->addAction(QStringLiteral("Open crash &log folder"));
    connect(openLogs, &QAction::triggered, this, [this]() {
        const QString path = diagnostics::crashLogPath();
        const QString dir = QFileInfo(path).absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    QAction* clearLogs = help->addAction(QStringLiteral("&Clear crash logs"));
    connect(clearLogs, &QAction::triggered, this, [this]() {
        if (QMessageBox::question(this, QStringLiteral("Clear crash logs"),
                QStringLiteral("Delete the local crash log file?"))
            == QMessageBox::Yes) {
            const bool ok = diagnostics::clearCrashLog();
            statusBar()->showMessage(ok ? QStringLiteral("Crash logs cleared")
                                        : QStringLiteral("Could not clear crash logs"));
        }
    });
}

void MainWindow::pushSettingsToAgent() {
    emit requestSetProvider(SettingsDialog::loadProvider());
    QString key = SettingsDialog::loadApiKey();
    emit requestSetApiKey(key);
    emit requestSetModel(SettingsDialog::loadModel());
    emit requestSetBaseUrl(SettingsDialog::loadBaseUrl());
    emit requestSetMaxIterations(SettingsDialog::loadMaxIterations());
    emit requestSetBashTimeoutMs(SettingsDialog::loadBashTimeoutMs());
    emit requestSetCheckCommand(SettingsDialog::loadCheckCommand());
    maxContextTokens_ = SettingsDialog::loadMaxContextTokens();
    reservedSendTokens_ = SettingsDialog::loadSendTokens();
    emit requestSetMaxContextTokens(maxContextTokens_);
    emit requestSetSendTokens(reservedSendTokens_);
    emit requestSetReasoningGuidance(SettingsDialog::loadReasoningGuidance());
}

void MainWindow::setProjectRoot(const fs::path& p) {
    projectRoot_ = p;
    const QString projectRootText = QString::fromUtf8(pathutil::toUtf8(projectRoot_).c_str());
    chat_->setWorkingDir(projectRootText);
    QString title = QStringLiteral("AutoCoder [built %1 @ %2] - %3")
                        .arg(kBuildDate, kBuildTime, projectRootText);
    if (auto guidanceFile = SystemPrompt::findProjectGuidanceFile(projectRoot_)) {
        const QString guidanceName =
            QString::fromUtf8(pathutil::toUtf8(guidanceFile->filename()).c_str());
        title += QStringLiteral(" - guidance: %1").arg(guidanceName);
    }
    setWindowTitle(title);
    emit requestCancel();  // unblock/stop any active run so the switch isn't queued behind it
    emit requestSetProject(projectRootText);
}

void MainWindow::onOpenProject() {
    QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open project folder"),
        QString::fromUtf8(pathutil::toUtf8(projectRoot_).c_str()));
    if (dir.isEmpty()) return;
    const QByteArray dirUtf8 = dir.toUtf8();
    setProjectRoot(pathutil::fromUtf8(std::string(dirUtf8.constData(), dirUtf8.size())));
    emit requestNewConversation();  // start fresh after switching project
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(dir));
}

void MainWindow::onSendUserMessage(const QString& text) {
    if (!ensureApiKey()) return;
    input_->clear();
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("Working..."));
    emit requestSubmitUserMessage(text);
}

void MainWindow::onStop() {
    emit requestCancel();
    statusBar()->showMessage(QStringLiteral("Stopping..."));
}

void MainWindow::onNewConversation() {
    diagnostics::TraceTimer timer("MainWindow::onNewConversation (to UI reaction)");
    // Cancel any active run first. requestCancel is connected both with a
    // DirectConnection to flagCancelled() -- so a tool blocking the worker
    // thread's event loop observes it immediately -- and queued to cancel(),
    // which aborts an in-flight stream before the queued newConversation()
    // slot runs. Without this, the queued request sits undelivered until the
    // current tool or run completes, making the button look dead.
    if (!confirmInterruptIfBusy()) return;
    emit requestCancel();
    emit requestNewConversation();
    statusBar()->showMessage(QStringLiteral("Starting new conversation..."));
}

void MainWindow::onSidebarItemClicked(QListWidgetItem* item) {
    if (!item) return;
    QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return;
    // Already the current conversation: nothing to load. This also covers the
    // unsaved "New conversation" sidebar entry, which has no file on disk yet.
    if (id == currentConversationId_) return;
    // If a turn is in flight, confirm before switching (which interrupts it).
    // On cancel, restore the sidebar selection to the current conversation so
    // the highlighted row keeps matching what's actually shown.
    if (!confirmInterruptIfBusy()) {
        reselectCurrentConversationRow();
        return;
    }
    currentConversationId_ = id;
    showConversationLoading();
    emit requestCancel();  // unblock/stop any active run so the load isn't queued behind it
    emit requestLoadConversation(id);
}

void MainWindow::onSidebarContextMenu(const QPoint& pos) {
    QListWidgetItem* item = sidebar_->itemAt(pos);
    if (!item) return;
    // Right-clicking an unselected row should act on that row alone, matching
    // typical list behavior; otherwise operate on the whole selection.
    if (!item->isSelected()) {
        sidebar_->setCurrentItem(item);
    }

    const int count = sidebar_->selectedItems().size();
    QMenu menu(this);
    QAction* del = menu.addAction(count > 1
        ? QStringLiteral("Delete %1 conversations").arg(count)
        : QStringLiteral("Delete conversation"));
    QAction* chosen = menu.exec(sidebar_->viewport()->mapToGlobal(pos));
    if (chosen != del) return;
    deleteSelectedConversations();
}

void MainWindow::deleteSelectedConversations() {
    const QList<QListWidgetItem*> items = sidebar_->selectedItems();
    QStringList ids;
    QString singleTitle;
    for (QListWidgetItem* item : items) {
        const QString id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) continue;  // skip the unsaved "New conversation" row
        ids << id;
        singleTitle = item->text();
    }
    if (ids.isEmpty()) return;

    QString question;
    if (ids.size() == 1) {
        question = QStringLiteral("Delete \"%1\" permanently?").arg(singleTitle);
    } else {
        question = QStringLiteral("Delete %1 conversations permanently?").arg(ids.size());
    }
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, QStringLiteral("Delete conversation"),
        question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    emit requestCancel();  // unblock/stop any active run so the delete isn't queued behind it
    emit requestDeleteConversations(ids);
}

// ===== Inbound from agent =====

void MainWindow::onConversationCleared() {
    diagnostics::TraceTimer timer("onConversationCleared (UI reacts)");
    // During startup restoration (startingUp_), the intermediate empty
    // conversation's conversationCleared signal should not wipe the chat.
    if (startingUp_) return;
    currentConversationPersisted_ = false;
    chat_->clear();
    activeToolCall_ = nullptr;
    lastToolCall_ = nullptr;
    input_->clearHistory();
    if (!replayInProgress_) setBusy(false);
    currentTurnIndex_ = 0;
    lastTokenPct_ = 0;
    hasTokenUsageStats_ = false;
}

void MainWindow::onConversationReplayStarted(int totalMessages, bool keepBusyAfterReplay) {
    startingUp_ = false;  // any replay clears the startup-restoration guard
    replayInProgress_ = true;
    keepBusyAfterReplay_ = keepBusyAfterReplay;
    setBusy(true);
    chat_->beginBulkAppend();
    replayProgress_->setRange(0, std::max(1, totalMessages));
    replayProgress_->setValue(0);
    replayProgress_->setFormat(QStringLiteral("Loading %v/%m"));
    replayProgress_->show();
    statusBar()->showMessage(QStringLiteral("Loading conversation..."));
}

void MainWindow::onConversationReplayProgress(int messagesDone, int totalMessages) {
    if (!replayProgress_->isVisible()) replayProgress_->show();
    replayProgress_->setRange(0, std::max(1, totalMessages));
    replayProgress_->setValue(std::clamp(messagesDone, 0, std::max(1, totalMessages)));
}

void MainWindow::onConversationReplayFinished() {
    chat_->endBulkAppend();
    if (scrollToTopAfterReplay_) {
        chat_->scrollToTop();
    } else {
        chat_->scrollToBottom();
    }
    replayInProgress_ = false;
    const bool keepBusy = keepBusyAfterReplay_;
    keepBusyAfterReplay_ = false;
    scrollToTopAfterReplay_ = false;
    hideConversationLoading();
    setBusy(keepBusy);
    statusBar()->showMessage(keepBusy ? QStringLiteral("Working...")
                                      : QStringLiteral("Ready"));
}

void MainWindow::onUserMessageAdded(const QString& text, bool canRevert, int turnIndex) {
    chat_->endAssistant();
    activeToolCall_ = nullptr;

    const int fallbackTurn = currentTurnIndex_++;
    const int turnForRevert = turnIndex >= 0 ? turnIndex : fallbackTurn;
    if (turnIndex >= currentTurnIndex_) {
        currentTurnIndex_ = turnIndex + 1;
    }

    // Save the conversation id at the time the message was added, since
    // currentConversationId_ may have changed by the time revert is clicked.
    QString convIdAtMessage = currentConversationId_;

    std::function<void()> revertCb;
    if (canRevert) {
        revertCb = [this, convIdAtMessage, turnForRevert, text]() {
        // Confirmation dialog.
        auto reply = QMessageBox::question(
            this,
            QStringLiteral("Revert conversation"),
            QStringLiteral(
                "This will restore the conversation and all modified files to the "
                "state they were in before this message was sent.\n\n"
                "Any subsequent messages and file changes will be lost.\n\n"
                "Note: this reverts in-folder file changes only. It cannot undo shell "
                "side effects such as pushed commits, deleted external files, network "
                "calls, or spent API credits. Continue?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        emit requestRevertToCheckpoint(convIdAtMessage, turnForRevert);
        input_->setDraft(text);
        };
    }

    chat_->appendUser(text, std::move(revertCb));

    input_->pushHistory(text);
    saveLastConversationId();
}

void MainWindow::onAssistantTextDelta(const QString& fragment) {
    chat_->appendToActiveAssistant(fragment);
}

void MainWindow::onAssistantTextFinalized() {
    chat_->endAssistant();
}

void MainWindow::onToolCallStarted(const QString& name, const QString& argsJson) {
    chat_->endAssistant();
    activeToolCall_ = chat_->appendToolCall(
        name, argsJson,
        QString::fromUtf8(pathutil::toUtf8(projectRoot_).c_str()));
    lastToolCall_ = activeToolCall_;
}

void MainWindow::onToolCallOutputDelta(const QString& /*name*/, const QString& chunk) {
    if (activeToolCall_) activeToolCall_->appendOutput(chunk);
}

void MainWindow::onToolCallFinished(const QString& /*name*/, const QString& result, bool ok) {
    if (activeToolCall_) {
        activeToolCall_->setResult(result, ok);
        // Keep lastToolCall_ pointing to this widget so replay can attach
        // a resolved approval status to it.
        lastToolCall_ = activeToolCall_;
        activeToolCall_ = nullptr;
    }
}

void MainWindow::onTurnFinished() {
    chat_->endAssistant();
    setBusy(false);

    // Suggest /compact when context usage is getting high (>= 70%).
    if (lastTokenPct_ >= 70) {
        QString suggestion = lastTokenPct_ >= 90
            ? QStringLiteral("Context at %1%% — the conversation may need trimming soon.  Try /compact to free up space.")
            : QStringLiteral("Context at %1%% — consider typing /compact to keep things running smoothly.");
        statusBar()->showMessage(
            suggestion.arg(lastTokenPct_),
            /*timeout=*/ 15000);
    } else {
        statusBar()->showMessage(QStringLiteral("Ready"));
    }

    input_->focusComposer();
}

void MainWindow::onAgentError(const QString& detail) {
    chat_->endAssistant();
    if (replayInProgress_) {
        chat_->endBulkAppend();
        replayInProgress_ = false;
        keepBusyAfterReplay_ = false;
        scrollToTopAfterReplay_ = false;
    }
    hideConversationLoading();
    setBusy(false);

    if (detail.startsWith(QStringLiteral("Saved conversation no longer exists: "))) {
        startingUp_ = false;
        currentConversationPersisted_ = false;
        QSettings s;
        if (s.value("ui/lastConversationId").toString() == currentConversationId_) {
            s.remove("ui/lastConversationId");
        }
        statusBar()->showMessage(
            QStringLiteral("Previous conversation was missing; started a new one"),
            8000);
        return;
    }

    // A user-initiated interrupt (Stop button, or switching/creating a
    // conversation mid-turn) surfaces as "cancelled". That's expected, not an
    // error -- the UI cleanup above is enough; don't pop a dialog for it.
    if (detail == QStringLiteral("cancelled")) {
        statusBar()->showMessage(QStringLiteral("Stopped"), 4000);
        return;
    }

    statusBar()->showMessage(QStringLiteral("Error"));
    QMessageBox::warning(this, QStringLiteral("AutoCoder error"), detail);
}

void MainWindow::updateContextLabel() {
    auto fmt = [](int n) {
        QString s = QString::number(n);
        for (int i = s.size() - 3; i > 0; i -= 3) s.insert(i, ',');
        return s;
    };

    QString color = "#9CA3AF";   // default cool gray
    if (lastTokenPct_ >= 90)      color = "#F87171";   // red
    else if (lastTokenPct_ >= 70) color = "#FBBF24";   // amber
    contextLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(color));

    QString text = QStringLiteral("Context: %1 msgs - ~%2 tok (%3%) - %4 KB")
                       .arg(lastContextMessages_)
                       .arg(fmt(lastApproxTokens_))
                       .arg(lastTokenPct_)
                       .arg(lastContextBytes_ / 1024);
    if (hasTokenUsageStats_) {
        text += QStringLiteral(" - Last: cached %1 / uncached %2 in - out %3 - total %4")
                    .arg(fmt(lastCachedInputTokens_))
                    .arg(fmt(lastUncachedInputTokens_))
                    .arg(fmt(lastOutputTokens_))
                    .arg(fmt(lastTotalTokens_));
    }
    contextLabel_->setText(text);
}

void MainWindow::onContextStats(int messages, int approxTokens, int bytes) {
    lastContextMessages_ = messages;
    lastApproxTokens_ = approxTokens;
    lastContextBytes_ = bytes;

    const int contextBudgetTokens = std::max(1, maxContextTokens_ - reservedSendTokens_);
    int pct = (approxTokens * 100) / contextBudgetTokens;
    lastTokenPct_ = pct;
    updateContextLabel();
}

void MainWindow::onTokenUsageStats(int cachedInputTokens, int uncachedInputTokens,
                                   int outputTokens, int totalTokens) {
    hasTokenUsageStats_ = true;
    lastCachedInputTokens_ = cachedInputTokens;
    lastUncachedInputTokens_ = uncachedInputTokens;
    lastOutputTokens_ = outputTokens;
    lastTotalTokens_ = totalTokens;
    updateContextLabel();
}

void MainWindow::onConversationTrimmed(int bytesFreed) {
    statusBar()->showMessage(
        QStringLiteral("Conversation trimmed to fit token budget (freed %1 KB)")
            .arg(bytesFreed / 1024),
        5000);
}

void MainWindow::onUserInputRequired(QString toolCallId, QString toolName,
                                     QString question, QStringList options) {
    Q_UNUSED(toolCallId)
    Q_UNUSED(toolName)
    setBusy(false);
    statusBar()->showMessage(QStringLiteral("Waiting for your input..."));

    // Append an inline ask_user widget to the chat.
    auto* askWidget = chat_->appendAskUser(question, options);

    // When the user answers, forward it to the agent.
    connect(askWidget, &AskUserWidget::answered, this, [this](const QString& answer) {
        statusBar()->showMessage(QStringLiteral("Resuming..."));
        setBusy(true);
        emit requestProvideUserInput(answer);
    });
}

void MainWindow::onCommandApprovalRequired(QString toolCallId, QString command,
                                           QString reason, QString explanation) {
    setBusy(false);
    statusBar()->showMessage(QStringLiteral("Waiting for your approval..."));

    // Attach approval buttons to the existing ToolCallWidget for this command,
    // instead of creating a separate widget.
    if (activeToolCall_) {
        activeToolCall_->setApprovalRequired(reason, explanation);
        connect(activeToolCall_, &ToolCallWidget::decided, this,
                [this, toolCallId](const QString& decision, bool persist) {
                    statusBar()->showMessage(QStringLiteral("Resuming..."));
                    setBusy(true);
                    emit requestProvideCommandDecision(toolCallId, decision, persist);
                });
    }
}

void MainWindow::onCompactStarted() {
    setBusy(true);
    statusBar()->showMessage(QStringLiteral("Compacting conversation..."));
}

void MainWindow::onCompactFinished(int bytesSaved) {
    setBusy(false);
    statusBar()->showMessage(
        QStringLiteral("Conversation compacted (freed %1 KB)").arg(bytesSaved / 1024),
        8000);
}

void MainWindow::onConversationsListed(QStringList ids, QStringList titles,
                                       QStringList updatedAt, QString currentId) {
    currentConversationId_ = currentId;
    currentConversationPersisted_ = false;
    for (int i = 0; i < ids.size(); ++i) {
        const QString& upd = i < updatedAt.size() ? updatedAt[i] : QString();
        if (ids[i] == currentId && !upd.isEmpty()) {
            currentConversationPersisted_ = true;
            break;
        }
    }

    // During startup restoration, don't populate the sidebar yet — the
    // temporary "New conversation" entry would flash and then be replaced
    // by the loaded conversation a moment later.
    if (startingUp_) return;

    saveLastConversationId();

    sidebar_->clear();
    for (int i = 0; i < ids.size(); ++i) {
        const QString& id    = ids[i];
        const QString& title = i < titles.size() ? titles[i] : QString();
        const QString& upd   = i < updatedAt.size() ? updatedAt[i] : QString();
        auto* item = new QListWidgetItem(title.isEmpty() ? id : title);
        item->setData(Qt::UserRole, id);
        item->setToolTip(QStringLiteral("%1\n%2").arg(id, upd));
        sidebar_->addItem(item);
        if (id == currentId) item->setSelected(true);
    }
}

void MainWindow::applyFontSize() {
    int pt = SettingsDialog::loadFontSize();
    QFont f = QApplication::font();
    f.setPointSize(pt);
    QApplication::setFont(f);
}

void MainWindow::setBusy(bool busy) {
    input_->setBusy(busy);
}

bool MainWindow::confirmInterruptIfBusy() {
    // isBusy() only reads atomics, so it is safe to call from the UI thread even
    // though agent_ lives on the worker thread.
    if (!agent_->isBusy()) return true;
    const auto btn = QMessageBox::question(
        this,
        QStringLiteral("AutoCoder"),
        QStringLiteral("The agent is still working in this conversation. "
                       "Switching now will stop it.\n\n"
                       "The reply generated so far is saved to this conversation, "
                       "but the turn will not finish.\n\nContinue?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    return btn == QMessageBox::Yes;
}

void MainWindow::reselectCurrentConversationRow() {
    const QSignalBlocker blocker(sidebar_);  // don't re-trigger onSidebarItemClicked
    for (int i = 0; i < sidebar_->count(); ++i) {
        QListWidgetItem* it = sidebar_->item(i);
        const bool match = it->data(Qt::UserRole).toString() == currentConversationId_;
        it->setSelected(match);
        if (match) sidebar_->setCurrentItem(it);
    }
}

bool MainWindow::ensureApiKey() {
    QString key = SettingsDialog::loadApiKey();
    if (key.isEmpty()) {
        SettingsDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted) return false;
        key = SettingsDialog::loadApiKey();
        if (key.isEmpty()) return false;
        pushSettingsToAgent();
    }
    return true;
}

void MainWindow::saveLastConversationId() {
    QSettings s;
    if (currentConversationId_.isEmpty() || !currentConversationPersisted_) {
        s.remove("ui/lastConversationId");
        return;
    }
    s.setValue("ui/lastConversationId", currentConversationId_);
}

void MainWindow::showConversationLoading() {
    replayInProgress_ = true;
    keepBusyAfterReplay_ = false;
    setBusy(true);
    replayProgress_->setRange(0, 0);
    replayProgress_->setFormat(QStringLiteral("Loading..."));
    replayProgress_->show();
    statusBar()->showMessage(QStringLiteral("Loading conversation..."));
}

void MainWindow::hideConversationLoading() {
    replayProgress_->hide();
    replayProgress_->setRange(0, 1);
    replayProgress_->setValue(0);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (agent_->isBusy()) {
        auto btn = QMessageBox::question(
            this,
            QStringLiteral("AutoCoder"),
            QStringLiteral("The agent is still busy (running a tool call, compacting,\n"
                           "or waiting for input). Do you want to cancel and close?"),
            QMessageBox::Cancel | QMessageBox::Close,
            QMessageBox::Cancel);
        if (btn == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        // User chose to close anyway — cancel in-flight work.
        // agent_ lives on the worker thread; route through the queued signal
        // rather than calling cancel() directly across threads.
        emit requestCancel();
    }
    saveLastConversationId();
    QMainWindow::closeEvent(event);
}
