#pragma once

#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QString>

inline QString formatDate(const QDate &date) {
    return QLocale::system().toString(date, QLocale::ShortFormat);
}

inline QString formatDate(const QDateTime &dt) {
    return formatDate(dt.date());
}

inline QString readSysFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.read(65536)).trimmed();
}

inline bool isValidModuleName(const QString &name) {
    static const QRegularExpression kRe("^[a-zA-Z0-9_-]+$");
    return !name.isEmpty() && kRe.match(name).hasMatch();
}

inline bool isSafeToDisable(const QString &driver) {
    static const QSet<QString> blocked{
        // GPU drivers — disabling loses all display output
        "amdgpu", "radeon", "nouveau", "i915", "xe",
        "nvidia", "nvidia_drm", "nvidia_modeset",
        "ast", "mgag200", "efifb", "vesafb", "simpledrm",
        // Primary storage — disabling makes the system unbootable
        "nvme", "ahci", "sd_mod", "usb_storage", "libata",
        "virtio_blk", "mmc_block",
        // USB host controllers — disabling kills USB keyboards/mice
        // even though usbhid is also blocked
        "xhci_hcd", "ehci_hcd", "ohci_hcd", "uhci_hcd",
        // Input — disabling leaves the user with no keyboard or mouse
        "i8042", "atkbd", "psmouse", "usbhid", "hid_generic",
        // Power management — disabling can cause thermal runaway or
        // prevent the battery/AC subsystem from working
        "acpi", "acpi_cpufreq", "battery", "ac",
        "thermal", "processor", "intel_pstate",
        // Device mapper / RAID — disabling breaks LVM and dm-crypt
        "dm_mod", "md_mod",
        // Core networking (VM host)
        "virtio_net",
        // Bluetooth stack core — disabling breaks all BT devices
        "bluetooth",
    };
    return !blocked.contains(driver);
}