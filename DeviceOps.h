#pragma once

#include <QString>

class QWidget;

inline constexpr const char *kModprobeConf =
    "/etc/modprobe.d/devicemanager-disabled.conf";

// Adds or removes the driver from the blacklist via pkexec and
// immediately loads/unloads the module. Shows an error dialog on failure.
bool setModuleBlacklisted(const QString &driver, bool blacklist, QWidget *parent);

// Looks up the DKMS module name/version for driver, then runs
// pkexec dkms remove --all. Shows result dialogs.
bool dkmsRemove(const QString &driver, QWidget *parent);
