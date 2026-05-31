#pragma once
#include <QHash>
#include <QIcon>
#include <QString>
#include <initializer_list>

inline QIcon tryIconName(const QString &name) {
    QIcon icon = QIcon::fromTheme(name);
    if (!icon.isNull()) return icon;
    icon = QIcon::fromTheme(name + QStringLiteral("-symbolic"));
    return icon;
}

inline QIcon themeIcon(std::initializer_list<const char *> names) {
    for (const char *n : names) {
        QIcon icon = tryIconName(QString::fromLatin1(n));
        if (!icon.isNull()) return icon;
    }
    QIcon icon = QIcon::fromTheme(QStringLiteral("preferences-system"));
    if (!icon.isNull()) return icon;
    return QIcon::fromTheme(QStringLiteral("preferences-system-symbolic"));
}

inline QIcon resolveIcon(const QString &name) {
    // Fallback chains for KDE/Breeze-specific names absent on other themes.
    using SL = QStringList;
    static const QHash<QString, SL> fallbacks{
        // kded5 is the KDE daemon icon repurposed for batteries
        {QStringLiteral("kded5"),
         SL{"battery", "battery-good", "battery-caution"}},
        {QStringLiteral("network-bluetooth"),
         SL{"bluetooth-active", "bluetooth"}},
        // input_devices_settings is KDE-specific (underscore name)
        {QStringLiteral("input_devices_settings"),
         SL{"preferences-desktop-peripherals", "input-gaming", "joystick"}},
        // kmix is the KDE mixer app icon
        {QStringLiteral("kmix"),
         SL{"audio-card", "audio-speakers", "audio-volume-high"}},
        {QStringLiteral("network-card"),
         SL{"network-wired", "network"}},
        {QStringLiteral("smartphone"),
         SL{"phone", "multimedia-player", "pda"}},
        {QStringLiteral("cpu"),
         SL{"processor", "computer"}},
        {QStringLiteral("drive-harddisk-encrypted"),
         SL{"security-high", "security-medium"}},
        {QStringLiteral("drive-removable-media-usb"),
         SL{"media-removable", "drive-removable-media"}},
        {QStringLiteral("video-display"),
         SL{"display", "monitor"}},
        {QStringLiteral("computer-apple-ipad"),
         SL{"tablet", "smartphone", "phone", "pda"}},
        {QStringLiteral("phone-apple-iphone"),
         SL{"phone", "smartphone", "pda"}},
        {QStringLiteral("multimedia-player-apple-ipod-touch"),
         SL{"multimedia-player", "phone", "smartphone"}},
        {QStringLiteral("input-gaming"),
         SL{"joystick"}},
        {QStringLiteral("media-optical"),
         SL{"media-cdrom", "drive-optical"}},
        {QStringLiteral("view-list-tree"),
         SL{"format-justify-fill", "format-list-unordered"}},
        {QStringLiteral("system-software-update"),
         SL{"software-update-available", "update-notifier"}},
        {QStringLiteral("applications-system-symbolic"),
         SL{"applications-system"}},
        // stock_next is an old GNOME 2 icon; go-next is the modern equivalent
        {QStringLiteral("stock_next"),
         SL{"go-next", "go-forward", "arrow-right"}},
    };

    if (!name.isEmpty()) {
        QIcon icon = tryIconName(name);
        if (!icon.isNull()) return icon;

        auto it = fallbacks.constFind(name);
        if (it != fallbacks.constEnd()) {
            for (const QString &fb : it.value()) {
                icon = tryIconName(fb);
                if (!icon.isNull()) return icon;
            }
        }
    }

    // Absolute last resort — works on Adwaita (symbolic-only) and Breeze alike.
    QIcon icon = QIcon::fromTheme(QStringLiteral("preferences-system"));
    if (!icon.isNull()) return icon;
    return QIcon::fromTheme(QStringLiteral("preferences-system-symbolic"));
}
