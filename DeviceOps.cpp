#include "DeviceOps.h"
#include "DeviceUtils.h"

#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QTextStream>

static void showOpError(QWidget *parent, const QString &title,
                        bool timedOut, const QByteArray &stderr) {
    QString detail = timedOut
        ? "Operation timed out."
        : QString::fromUtf8(stderr).trimmed();
    QMessageBox::warning(parent, title,
        QString("Operation failed:\n\n%1").arg(detail.toHtmlEscaped()));
}

bool setModuleBlacklisted(const QString &driver, bool blacklist, QWidget *parent) {
    const QString opName = blacklist ? "disable" : "enable";

    if (!isValidModuleName(driver)) {
        QMessageBox::warning(parent,
            QString("Could not %1 device").arg(opName),
            "Invalid driver name.");
        return false;
    }

    // Use argument-list form for both steps so no shell interprets the driver
    // name. Two separate pkexec calls are required because pkexec cannot chain
    // commands without a shell.

    QProcess fileProc;
    if (blacklist) {
        fileProc.start("pkexec", {"tee", "-a", kModprobeConf});
        fileProc.write(QString("blacklist %1\n").arg(driver).toUtf8());
        fileProc.closeWriteChannel();
    } else {
        // Read the file as the current user (modprobe.d files are 644),
        // strip the matching line in memory, then overwrite via pkexec tee.
        // Doing the filtering here avoids spawning sed and eliminates the
        // non-atomic temp-file window that sed -i would produce.
        const QString target = QString("blacklist %1").arg(driver);
        QByteArray newContent;
        QFile confFile(kModprobeConf);
        if (confFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&confFile);
            while (!in.atEnd()) {
                QString line = in.readLine();
                if (line.trimmed() != target)
                    newContent.append((line + '\n').toUtf8());
            }
        }
        fileProc.start("pkexec", {"tee", kModprobeConf});
        fileProc.write(newContent);
        fileProc.closeWriteChannel();
    }
    bool ok = fileProc.waitForFinished(10000);
    if (!ok || fileProc.exitCode() != 0) {
        showOpError(parent,
            QString("Could not %1 device").arg(opName),
            !ok, fileProc.readAllStandardError());
        return false;
    }

    // Non-fatal for enable (the module may simply not be installed), but reported for disable.
    QProcess modProc;
    if (blacklist)
        modProc.start("pkexec", {"modprobe", "-r", driver});
    else
        modProc.start("pkexec", {"modprobe", driver});
    ok = modProc.waitForFinished(10000);
    if (!ok || modProc.exitCode() != 0) {
        // The blacklist entry was already written; warn but still return true
        // so the UI reflects the new persistent state.
        QString detail = !ok
            ? "Operation timed out."
            : QString::fromUtf8(modProc.readAllStandardError()).trimmed();
        QMessageBox::warning(parent,
            QString("Could not fully %1 device").arg(opName),
            QString("The blacklist was updated but modprobe reported an "
                    "error:\n\n%1\n\nThe change will take effect on next boot.")
                .arg(detail.toHtmlEscaped()));
    }
    return true;
}

bool dkmsRemove(const QString &driver, QWidget *parent) {
    if (!isValidModuleName(driver)) {
        QMessageBox::warning(parent, "Uninstall failed", "Invalid driver name.");
        return false;
    }

    QProcess proc;
    proc.start("dkms", {"status"});
    bool statusOk = proc.waitForFinished(5000);
    if (!statusOk) {
        QMessageBox::warning(parent, "Uninstall failed",
            "Could not query DKMS status (timed out).");
        return false;
    }
    QString dkmsStatus = QString::fromUtf8(proc.readAllStandardOutput());

    // Match on the module name at the start of each line only.
    // dkms status formats: "name/version, ..." (new) or "name, version, ..." (old).
    // Using contains() would match "foo" inside "foobar", removing the wrong module.
    QString moduleLine;
    for (const QString &line : dkmsStatus.split('\n')) {
        if (line.startsWith(driver + "/") || line.startsWith(driver + ",")) {
            moduleLine = line;
            break;
        }
    }

    QString nameVer;
    if (!moduleLine.isEmpty())
        nameVer = moduleLine.section(',', 0, 0).trimmed();

    if (nameVer.isEmpty()) {
        QMessageBox::warning(parent, "Uninstall failed",
            "Could not find DKMS module information.");
        return false;
    }

    // Validate the parsed token before passing it to a privileged process.
    // DKMS identifiers look like "name/version" or just "name"; permit only
    // alphanumerics, underscores, hyphens, dots, and one optional slash.
    static const QRegularExpression kDkmsRe("^[a-zA-Z0-9_./-]+$");
    if (!kDkmsRe.match(nameVer).hasMatch()) {
        QMessageBox::warning(parent, "Uninstall failed",
            "Unexpected DKMS module name format.");
        return false;
    }

    QProcess uninstall;
    uninstall.start("pkexec", {"dkms", "remove", nameVer, "--all"});
    bool removeOk = uninstall.waitForFinished(30000);
    if (!removeOk || uninstall.exitCode() != 0) {
        showOpError(parent, "Uninstall failed",
            !removeOk, uninstall.readAllStandardError());
        return false;
    }

    QMessageBox::information(parent, "Uninstalled", "The driver has been removed.");
    return true;
}
