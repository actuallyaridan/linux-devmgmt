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
        // GPU: no display output
        "amdgpu", "radeon", "nouveau", "i915", "xe",
        "nvidia", "nvidia_drm", "nvidia_modeset",
        "ast", "mgag200", "efifb", "vesafb", "simpledrm",
        // Primary storage: unbootable
        "nvme", "ahci", "sd_mod", "usb_storage", "libata",
        "virtio_blk", "mmc_block",
        // USB host controllers: kills USB keyboards/mice, so blocked
        // alongside usbhid rather than relying on it
        "xhci_hcd", "ehci_hcd", "ohci_hcd", "uhci_hcd",
        // Input: no keyboard or mouse
        "i8042", "atkbd", "psmouse", "usbhid", "hid_generic",
        // Power management: thermal runaway, or a dead battery/AC subsystem
        "acpi", "acpi_cpufreq", "battery", "ac",
        "thermal", "processor", "intel_pstate",
        // Device mapper / RAID: breaks LVM and dm-crypt
        "dm_mod", "md_mod",
        // Core networking (VM host)
        "virtio_net",
        // Bluetooth stack core: breaks all BT devices
        "bluetooth",
    };
    return !blocked.contains(driver);
}