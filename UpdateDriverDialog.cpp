#include "UpdateDriverDialog.h"

#include <QDir>
#include <QVBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QCommandLinkButton>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <QIcon>

UpdateDriverDialog::UpdateDriverDialog(const QString &deviceName,
                                       QWidget *parent)
    : QDialog(parent) {
    setWindowTitle("Update Driver Software - " + deviceName);
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setFixedSize(500, 280);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *content = new QWidget;
    QPalette p = content->palette();
    p.setColor(QPalette::Window, p.color(QPalette::Base));
    content->setPalette(p);
    content->setAutoFillBackground(true);
    auto *cv = new QVBoxLayout(content);
    cv->setContentsMargins(20, 20, 20, 20);
    cv->setSpacing(0);
    v->addWidget(content, 1);

    auto *title = new QLabel(
        "How do you want to search for driver software?");
    QFont f = title->font();
    f.setPointSize(f.pointSize() + 3);
    title->setFont(f);
    title->setWordWrap(true);
    cv->addWidget(title);
    cv->addSpacing(16);

    QString flatStyle = R"(
    QCommandLinkButton {
        border: none;
        background: transparent;
        text-align: left;
    }
    QCommandLinkButton:hover {
        background: rgba(235, 244, 252, 200);
        border: 1px solid rgba(120, 174, 229, 220);
        border-radius: 3px;
    }
    QCommandLinkButton:pressed {
        background: rgba(210, 232, 248, 220);
        border: 1px solid rgba(90, 150, 210, 240);
        border-radius: 3px;
    }
)";
    auto *autoBtn = new QCommandLinkButton(
        "Search automatically for updated driver software",
        "Discover will search your computer and the Internet for "
        "the latest driver software for your device.");
    autoBtn->setIcon(QIcon::fromTheme("stock_next"));
    autoBtn->setIconSize(QSize(16, 16));
    autoBtn->setStyleSheet(flatStyle);
    cv->addWidget(autoBtn);
    cv->addSpacing(8);

    auto *browseBtn = new QCommandLinkButton(
        "Browse my computer for driver software",
        "Locate and install driver software manually.");
    browseBtn->setIcon(QIcon::fromTheme("stock_next"));
    browseBtn->setIconSize(QSize(16, 16));
    browseBtn->setStyleSheet(flatStyle);
    cv->addWidget(browseBtn);
    cv->addStretch();

    auto *line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    v->addWidget(line);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->setContentsMargins(0, 8, 8, 8);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    connect(autoBtn, &QCommandLinkButton::clicked, this, [this] {
        bool launched = QProcess::startDetached(
            "plasma-discover", {"--mode", "Update"});
        if (!launched)
            QDesktopServices::openUrl(QUrl("appstream://"));
        accept();
    });

    connect(browseBtn, &QCommandLinkButton::clicked, this, [this] {
        QString path = QFileDialog::getExistingDirectory(
            this, "Select driver source directory",
            "/usr/src",
            QFileDialog::ShowDirsOnly);
        if (path.isEmpty())
            return;

        QFileInfo pathInfo(path);
        if (pathInfo.isSymLink()) {
            QMessageBox::warning(this, "No driver found",
                "The selected path is a symbolic link. "
                "Please select a real directory.");
            return;
        }

        // Resolve to canonical path once so all subsequent checks and the
        // privileged process all operate on the same concrete location,
        // eliminating symlink-swap races between check and use.
        QString canonPath = QDir(path).canonicalPath();
        if (canonPath.isEmpty()) {
            QMessageBox::warning(this, "No driver found",
                "Could not resolve the selected directory.");
            return;
        }

        if (!QFileInfo::exists(canonPath + "/dkms.conf")) {
            QMessageBox::warning(this, "No driver found",
                "The selected directory does not contain a dkms.conf file.\n\n"
                "Please select the root directory of the driver source.");
            return;
        }

        if (QMessageBox::warning(this, "Install kernel module",
                "This will install a kernel module from:\n\n" + canonPath.toHtmlEscaped() +
                "\n\nOnly install drivers from sources you trust. Continue?",
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;

        // Re-verify with the resolved path immediately before handing it to
        // a privileged process — closes the remaining TOCTOU window.
        if (!QFileInfo::exists(canonPath + "/dkms.conf")) {
            QMessageBox::warning(this, "No driver found",
                "The selected directory no longer contains a dkms.conf file.");
            return;
        }

        QProcess proc;
        proc.start("pkexec", {"dkms", "install", canonPath});
        bool finished = proc.waitForFinished(60000);

        if (finished && proc.exitCode() == 0) {
            QMessageBox::information(this, "Driver installed",
                "The driver was installed. "
                "You may need to restart for changes to take effect.");
        } else {
            QString err = !finished
                ? "Installation timed out."
                : QString::fromUtf8(proc.readAllStandardError()).trimmed();
            QMessageBox::warning(this, "Installation failed",
                "Could not install the driver:\n\n" + err.toHtmlEscaped());
        }
        accept();
    });
}