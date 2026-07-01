#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QPushButton>

#include "diagnostics/CrashHandler.h"
#include "tools/shell/CommandPolicy.h"
#include "ui/MainWindow.h"

namespace {

// One-time autonomy disclaimer. Returns false if the user declines (the app
// should then exit without doing anything). Consent is recorded in QSettings so
// this is shown only on first run.
bool ensureFirstRunConsent() {
    if (autocoder::shell::hasShellConsent()) return true;

    QMessageBox box;
    box.setWindowTitle(QStringLiteral("Welcome to AutoCoder"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("AutoCoder is an autonomous coding agent."));
    box.setInformativeText(QStringLiteral(
        "Within the working folder you open, AutoCoder can create, modify, delete, and "
        "execute code on your machine on its own. Commands that touch the network, run "
        "with elevated privileges, or are otherwise destructive will ask for your "
        "approval first — but routine edits, builds, and tests run automatically.\n\n"
        "Use version control and back up important work before you start.\n\n"
        "Note: “Revert” covers in-folder file changes only. It cannot undo shell "
        "side effects such as pushed commits, deleted external files, network calls, or "
        "spent API credits."));
    box.setStyleSheet(QStringLiteral(
        "QMessageBox { background-color: #1E1E1E; }"
        "QMessageBox QLabel { color: #E5E7EB; font-size: 13px; }"
        "QMessageBox QPushButton { background-color: #3B82F6; color: white; border: none;"
        "  border-radius: 6px; padding: 6px 16px; font-size: 13px; min-width: 90px; }"
        "QMessageBox QPushButton:hover { background-color: #2563EB; }"));

    QPushButton* understand = box.addButton(QStringLiteral("I understand"),
                                            QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Quit"), QMessageBox::RejectRole);
    box.setDefaultButton(understand);
    box.exec();

    if (box.clickedButton() == understand) {
        autocoder::shell::recordShellConsent();
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("AutoCoder");
    QApplication::setOrganizationName("AutoCoder");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setWindowIcon(QIcon(":/autocoder.png"));
    diagnostics::installCrashHandler();

    if (!ensureFirstRunConsent()) return 0;

    MainWindow window;
    window.show();
    return app.exec();
}
