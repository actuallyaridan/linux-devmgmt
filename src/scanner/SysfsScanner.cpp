#include "SysfsScanner.h"
#include "DeviceUtils.h"
#include "DeviceOps.h"
#include "KnownDriverDates.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QSysInfo>
#include <QProcess>
#include <QRegularExpression>

namespace {

// Resolve data-file directories via environment variables so that Nix (and
// other non-FHS layouts) can point us at the right store paths.
QString hwdataPath(const QString &filename) {
    QByteArray dir = qgetenv("HWDATA_DIR");
    if (!dir.isEmpty())
        return QString::fromUtf8(dir) + "/" + filename;
    return QStringLiteral("/usr/share/hwdata/") + filename;
}

QString libdrmPath(const QString &filename) {
    QByteArray dir = qgetenv("LIBDRM_DIR");
    if (!dir.isEmpty())
        return QString::fromUtf8(dir) + "/" + filename;
    return QStringLiteral("/usr/share/libdrm/") + filename;
}

QString kernelRelease() {
    return QSysInfo::kernelVersion();
}

QString runCmd(const QString &cmd, const QStringList &args,
               int timeoutMs = 2000) {
    QProcess proc;
    proc.start(cmd, args);
    if (!proc.waitForFinished(timeoutMs))
        return {};
    if (proc.exitCode() != 0)
        return {};
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

enum class PkgMgr { None, Pacman, Dpkg, Rpm };

PkgMgr detectPkgMgr() {
    // Detect by database directory, not binary, so we correctly identify the
    // host distro even when foreign tools are installed (e.g. via distrobox).
    if (QDir("/var/lib/pacman").exists())
        return PkgMgr::Pacman;
    if (QDir("/var/lib/dpkg").exists())
        return PkgMgr::Dpkg;
    if (QDir("/var/lib/rpm").exists())
        return PkgMgr::Rpm;
    return PkgMgr::None;
}

PkgMgr activePkgMgr() {
    static PkgMgr mgr = detectPkgMgr();
    return mgr;
}

struct DriverInfo {
    QString version;
    QString date;
    QString author;
    bool isDkms = false;
};

struct ModinfoResult {
    QString version;
    QString filename;
    QString author;
};

ModinfoResult queryModinfo(const QString &moduleName) {
    QProcess proc;
    proc.start("modinfo", {"--", moduleName});
    ModinfoResult r;
    if (!proc.waitForFinished(2000) || proc.exitCode() != 0)
        return r;
    for (const QString &line :
         QString::fromUtf8(proc.readAllStandardOutput()).split('\n')) {
        int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        QString key = line.left(colon).trimmed();
        QString val = line.mid(colon + 1).trimmed();
        if (key == "version")
            r.version = val;
        else if (key == "filename")
            r.filename = val;
        else if (key == "author")
            r.author = val;
    }
    return r;
}

QString pacmanFieldFrom(const QString &output, const QString &field) {
    for (const QString &line : output.split('\n')) {
        if (line.startsWith(field)) {
            int colon = line.indexOf(':');
            if (colon > 0)
                return line.mid(colon + 1).trimmed();
        }
    }
    return {};
}

QString pacmanField(const QString &package, const QString &field) {
    return pacmanFieldFrom(runCmd("pacman", {"-Qi", package}), field);
}

QString pacmanBuildDate(const QString &package, const QString &version) {
    static const QRegularExpression kPkgRe("^[a-zA-Z0-9_.+@-]+$");
    if (version.isEmpty()
            || !kPkgRe.match(package).hasMatch()
            || !kPkgRe.match(version).hasMatch())
        return {};
    QString dirPath = QStringLiteral("/var/lib/pacman/local/%1-%2")
                          .arg(package, version);
    QFile f(dirPath + "/desc");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QString content = QString::fromUtf8(f.read(65536));
    int idx = content.indexOf("%BUILDDATE%");
    if (idx < 0)
        return {};
    int nl = content.indexOf('\n', idx);
    if (nl < 0)
        return {};
    QString tsLine = content.mid(nl + 1,
                                 content.indexOf('\n', nl + 1) - nl - 1)
                         .trimmed();
    bool ok = false;
    qint64 ts = tsLine.toLongLong(&ok);
    if (!ok)
        return {};
    return formatDate(QDateTime::fromSecsSinceEpoch(ts));
}

QString knownVersionDate(const QString &version) {
    auto it = knownDriverDates().constFind(version);
    if (it == knownDriverDates().constEnd())
        return {};
    QDate d = QDate::fromString(*it, "yyyy-MM-dd");
    if (!d.isValid())
        return *it;
    return formatDate(d);
}

DriverInfo dkmsInfo(const QString &moduleName) {
    DriverInfo info;
    QDir dkmsRoot("/var/lib/dkms");
    if (!dkmsRoot.exists())
        return info;

    QString kver = kernelRelease();

    for (const QString &source : dkmsRoot.entryList(
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir sourceDir(dkmsRoot.absoluteFilePath(source));
        QStringList versions;
        for (const QString &entry : sourceDir.entryList(
                 QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!entry.startsWith("kernel-"))
                versions.append(entry);
        }
        if (versions.isEmpty())
            continue;
        versions.sort();
        QString version = versions.last();

        QDir kernelDir(sourceDir.absoluteFilePath(version + "/" + kver));
        if (!kernelDir.exists())
            continue;

        for (const QString &arch : kernelDir.entryList(
                 QDir::Dirs | QDir::NoDotAndDotDot)) {
            QDir modDir(kernelDir.absoluteFilePath(arch + "/module"));
            if (!modDir.exists())
                continue;
            QStringList matches = modDir.entryList(
                {moduleName + ".ko",
                 moduleName + ".ko.zst",
                 moduleName + ".ko.xz",
                 moduleName + ".ko.gz"},
                QDir::Files);
            if (!matches.isEmpty()) {
                info.version = version;
                QString known = knownVersionDate(version);
                if (!known.isEmpty()) {
                    info.date = known;
                } else {
                    QFileInfo vfi(sourceDir.absoluteFilePath(version));
                    info.date = formatDate(vfi.lastModified());
                }
                return info;
            }
        }
    }
    return info;
}


struct PkgVersionDate { QString version; QString date; };

QString pkgOwnerOf(const QString &filepath) {
    switch (activePkgMgr()) {
    case PkgMgr::Pacman:
        return runCmd("pacman", {"-Qoq", filepath});
    case PkgMgr::Dpkg: {
        // Output: "pkgname: /path/to/file" (first match wins)
        QString out = runCmd("dpkg", {"-S", filepath});
        int colon = out.indexOf(':');
        if (colon < 0) return {};
        return out.left(colon).trimmed();
    }
    case PkgMgr::Rpm:
        return runCmd("rpm", {"-qf", "--queryformat", "%{NAME}", filepath});
    default:
        return {};
    }
}

PkgVersionDate queryPkgVersionDate(const QString &pkg) {
    switch (activePkgMgr()) {
    case PkgMgr::Pacman: {
        QString out = runCmd("pacman", {"-Qi", pkg});
        QString ver = pacmanFieldFrom(out, "Version");
        return {ver, pacmanBuildDate(pkg, ver)};
    }
    case PkgMgr::Dpkg: {
        // dpkg has no build date in its metadata; version only.
        QString ver = runCmd("dpkg-query",
                             {"-W", "--showformat=${Version}", pkg});
        return {ver, {}};
    }
    case PkgMgr::Rpm: {
        QString out = runCmd("rpm", {"-q", "--queryformat",
                                     "%{VERSION}-%{RELEASE}\t%{BUILDTIME}",
                                     pkg});
        if (out.isEmpty()) return {};
        QStringList parts = out.split('\t');
        QString ver = parts.value(0).trimmed();
        QString date;
        if (parts.size() >= 2) {
            bool ok = false;
            qint64 ts = parts[1].trimmed().toLongLong(&ok);
            if (ok && ts > 0)
                date = formatDate(QDateTime::fromSecsSinceEpoch(ts));
        }
        return {ver, date};
    }
    default:
        return {};
    }
}

DriverInfo moduleDriverInfo(const QString &moduleName) {
    DriverInfo info;
    if (moduleName.isEmpty() || moduleName == "(kernel)") {
        info.version = kernelRelease();
        return info;
    }

    ModinfoResult mi = queryModinfo(moduleName);

    if (mi.filename == "(builtin)" || mi.filename.isEmpty()) {
        info.version = mi.version.isEmpty() ? kernelRelease() : mi.version;
        info.author = mi.author;
        return info;
    }

    if (mi.filename.contains("/dkms/")) {
        DriverInfo dkms = dkmsInfo(moduleName);
        if (!dkms.version.isEmpty()) {
            info.version = mi.version.isEmpty() ? dkms.version : mi.version;
            info.date = dkms.date;
            info.author = mi.author;
            info.isDkms = true;
            return info;
        }
    }

    QString pkg = pkgOwnerOf(mi.filename);
    if (!pkg.isEmpty()) {
        PkgVersionDate pvd = queryPkgVersionDate(pkg);
        info.version = mi.version.isEmpty()
            ? (pvd.version.isEmpty() ? kernelRelease() : pvd.version)
            : mi.version;
        QString known = knownVersionDate(info.version);
        info.date = known.isEmpty() ? pvd.date : known;
        info.author = mi.author;
        return info;
    }

    info.version = mi.version.isEmpty() ? kernelRelease() : mi.version;
    info.author = mi.author;
    QFileInfo fi(mi.filename);
    if (fi.exists())
        info.date = formatDate(fi.lastModified());
    return info;
}

struct IdsDb {
    QHash<QString, QString> vendors;
    QHash<QString, QString> devices;
};

IdsDb loadIds(const QString &path) {
    IdsDb db;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return db;
    QTextStream in(&f);
    QString currentVendor;
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.startsWith('#') || line.trimmed().isEmpty())
            continue;
        if (line.startsWith("\t\t"))
            continue;
        if (line.startsWith('\t')) {
            QString trimmed = line.mid(1);
            QString id = trimmed.left(4).toLower();
            QString name = trimmed.mid(6).trimmed();
            if (!currentVendor.isEmpty())
                db.devices.insert(currentVendor + ":" + id, name);
        } else {
            if (line.length() < 6)
                continue;
            if (line.startsWith("C "))
                break;
            currentVendor = line.left(4).toLower();
            QString name = line.mid(6).trimmed();
            db.vendors.insert(currentVendor, name);
        }
    }
    return db;
}

const IdsDb &pciIds() {
    static IdsDb db = loadIds(hwdataPath("pci.ids"));
    return db;
}

const IdsDb &usbIds() {
    static IdsDb db = loadIds(hwdataPath("usb.ids"));
    return db;
}

// Loads /usr/share/libdrm/amdgpu.ids: "DEVICE_HEX, REVISION_HEX, Marketing Name"
// Returns a hash keyed by "device:revision" (uppercase, no 0x prefix).
QHash<QString, QString> loadAmdgpuIds() {
    QHash<QString, QString> db;
    QFile f(libdrmPath("amdgpu.ids"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return db;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.startsWith('#') || line.isEmpty())
            continue;
        QStringList parts = line.split(',');
        if (parts.size() < 3)
            continue;
        QString devId  = parts[0].trimmed().toUpper();
        QString revId  = parts[1].trimmed().toUpper();
        QString name   = parts[2].trimmed();
        if (!name.isEmpty())
            db.insert(devId + ":" + revId, name);
    }
    return db;
}

const QHash<QString, QString> &amdgpuIds() {
    static QHash<QString, QString> db = loadAmdgpuIds();
    return db;
}

QString usbVendorName(const QString &vendorHex) {
    QString key = vendorHex.trimmed().toLower();
    return usbIds().vendors.value(key, key);
}

// Bluetooth SIG company identifiers — a separate registry from USB vendor IDs.
// Source: https://www.bluetooth.com/specifications/assigned-numbers/
QString btCompanyName(const QString &hexId) {
    static const QHash<quint16, QString> kCompanies = {
        {0x0000, "Ericsson AB"},
        {0x0001, "Nokia Mobile Phones"},
        {0x0002, "Intel Corp."},
        {0x0003, "IBM Corp."},
        {0x0004, "Toshiba Corp."},
        {0x0006, "Microsoft Corp."},
        {0x000F, "Broadcom Corporation"},
        {0x0046, "LG Electronics"},
        {0x004C, "Apple, Inc."},
        {0x0059, "Nordic Semiconductor ASA"},
        {0x0075, "Samsung Electronics Co. Ltd."},
        {0x009E, "Bose Corporation"},
        {0x00E0, "Google"},
        {0x012D, "Sony Corporation"},
        {0x0131, "Beats Electronics"},
        {0x0217, "Jabra (GN Audio)"},
    };
    bool ok;
    quint16 id = hexId.trimmed().toUShort(&ok, 16);
    if (!ok)
        return {};
    return kCompanies.value(id);
}

QString readHexId(const QString &path) {
    QString s = readSysFile(path);
    if (s.startsWith("0x"))
        s = s.mid(2);
    return s.toLower();
}

QString driverNameFor(const QString &sysfsPath) {
    QFileInfo link(sysfsPath + "/driver");
    if (!link.exists())
        return {};
    return QFileInfo(link.symLinkTarget()).fileName();
}

QString dmiProductName() {
    static const QString name = readSysFile("/sys/class/dmi/id/product_name").trimmed();
    return name;
}

QString friendlyLocation(const QString &sysfsPath,
                         const QString &rawLocation) {
    if (sysfsPath.isEmpty() && rawLocation.isEmpty())
        return "Unknown";

    QString path = sysfsPath.isEmpty() ? rawLocation : sysfsPath;

    if (path.contains("i8042") || path.contains("serio"))
        return "plugged in to PS/2 mouse port";

    if (path.contains("/bluetooth/"))
        return "connected via Bluetooth";

    if (path.contains("/usb")) {
        QRegularExpression usbRe(R"(/usb\d+/(\d+-[\d.]+))");
        auto match = usbRe.match(path);
        if (match.hasMatch())
            return QString("plugged in to USB port %1")
                .arg(match.captured(1));
        return "plugged in to USB port";
    }

    QRegularExpression pciRe(
        R"((\d+):([0-9a-f]+):([0-9a-f]+)\.([0-9a-f]+))");
    auto match = pciRe.match(path);
    if (match.hasMatch()) {
        bool ok1, ok2, ok3;
        int bus = match.captured(2).toInt(&ok1, 16);
        int dev = match.captured(3).toInt(&ok2, 16);
        int fn  = match.captured(4).toInt(&ok3, 16);
        if (!ok1 || !ok2 || !ok3)
            return {};
        return QString("PCI bus %1, device %2, function %3")
            .arg(bus).arg(dev).arg(fn);
    }

    if (path.contains("/platform/") || path.contains("LNXSYSTM")
        || path.contains("ACPI")) {
        QString host = dmiProductName();
        return host.isEmpty() ? "ACPI system device" : "on " + host;
    }

    if (path.contains("/hid/"))
        return "plugged in via HID";

    if (path.contains("/sound/card")) {
        QRegularExpression cardRe(R"(card(\d+))");
        auto m = cardRe.match(rawLocation);
        if (m.hasMatch())
            return QString("Audio card %1").arg(m.captured(1));
        return "Audio device";
    }

    if (!rawLocation.isEmpty() && !rawLocation.startsWith("/sys"))
        return rawLocation;

    return {};
}

QString monitorLocation(const QString &entry, const QString &gpuName = {}) {
    // DRM connector names: "card0-HDMI-A-1", "card1-DP-3", "card0-eDP-1"
    static QRegularExpression re(R"(card(\d+)-(.+)-(\d+))");
    auto m = re.match(entry);
    if (!m.hasMatch())
        return entry;
    QString type = m.captured(2);
    int port = m.captured(3).toInt();
    QString suffix = gpuName.isEmpty() ? QString() : " on " + gpuName;
    if (type == "eDP" || type == "LVDS")
        return "Built-in display" + suffix;
    if (type.startsWith("HDMI"))
        return QString("plugged in to HDMI port %1").arg(port) + suffix;
    if (type == "DP")
        return QString("plugged in to DisplayPort %1").arg(port) + suffix;
    if (type.startsWith("DVI"))
        return QString("plugged in to DVI port %1").arg(port) + suffix;
    if (type == "VGA")
        return QString("plugged in to VGA port %1").arg(port) + suffix;
    return entry;
}

Device makePciDevice(const QString &sysfsPath) {
    Device d;
    QString vendor = readHexId(sysfsPath + "/vendor");
    QString device = readHexId(sysfsPath + "/device");
    QString cls = readHexId(sysfsPath + "/class");
    const auto &db = pciIds();
    d.manufacturer = db.vendors.value(vendor,
                        QStringLiteral("Vendor %1").arg(vendor));
    d.name = db.devices.value(vendor + ":" + device,
                  QStringLiteral("Device %1:%2").arg(vendor, device));
    // For AMD GPUs, amdgpu.ids maps device+revision to a specific marketing name
    // (e.g. "AMD Radeon RX 6700 XT" instead of "Navi 22 [Radeon RX 6700/6700 XT/...]")
    if (vendor == "1002") {
        QString rev = readSysFile(sysfsPath + "/revision").trimmed()
                          .remove("0x").toUpper();
        QString devUpper = device.toUpper();
        QString marketing = amdgpuIds().value(devUpper + ":" + rev);
        if (!marketing.isEmpty())
            d.name = marketing;
    }
    d.driver = driverNameFor(sysfsPath);
    // PCI bridges (06xx), IOMMUs (0806xx), and generic system peripherals (0800xx)
    // are managed entirely by the kernel and never need a userspace driver.
    bool isKernelManaged = cls.startsWith("06")
                           || cls.startsWith("0806")
                           || cls.startsWith("0800");
    d.noDriverNeeded = isKernelManaged && d.driver.isEmpty();
    d.status = (d.driver.isEmpty() && !d.noDriverNeeded) ? "No driver loaded" : "Working properly";
    DriverInfo di = moduleDriverInfo(d.driver);
    d.driverVersion = di.version;
    d.driverDate = di.date;
    d.driverAuthor = di.author;
    d.isDkms = di.isDkms;

    // Non-PCI devices (platform/AMBA on ARM SoCs) have no vendor sysfs file,
    // leaving the name as "Device :". Use the driver's modinfo description as
    // a friendlier fallback, stripping the conventional trailing "driver" word.
    if (vendor.isEmpty() && !d.driver.isEmpty()) {
        QString desc = runCmd("modinfo", {"-F", "description", "--", d.driver});
        if (!desc.isEmpty()) {
            static const QRegularExpression kDriverSuffix(
                R"(\s+[Dd]river[.]*\s*$)");
            desc.remove(kDriverSuffix);
            desc = desc.trimmed();
        }
        if (!desc.isEmpty())
            d.name = desc;
        else
            d.name = d.driver;
        if (d.manufacturer == "Vendor ")
            d.manufacturer = "Unknown";
    }

    d.location = friendlyLocation(sysfsPath, {});
    d.rawLocation = QFileInfo(sysfsPath).fileName();
    d.sysfsPciPath = sysfsPath;
    return d;
}

QVector<Device> scanPciByClass(const QString &classPrefix) {
    QVector<Device> out;
    QDir base("/sys/bus/pci/devices");
    const QString lowerPrefix = classPrefix.toLower();
    for (const QString &entry : base.entryList(
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString path = base.absoluteFilePath(entry);
        QString cls = readHexId(path + "/class");
        if (cls.startsWith(lowerPrefix)) {
            Device d = makePciDevice(path);
            d.rawLocation = entry;
            out.append(d);
        }
    }
    return out;
}

QVector<Device> scanCpus() {
    QVector<Device> out;
    QFile f("/proc/cpuinfo");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;

    QString modelName;
    QString vendorId;

    QString content = QString::fromUtf8(f.read(524288));
    for (const QString &line : content.split('\n')) {
        if (line.startsWith("model name"))
            modelName = line.section(':', 1).trimmed();
        else if (line.startsWith("vendor_id"))
            vendorId = line.section(':', 1).trimmed();
        else if (line.trimmed().isEmpty() && !modelName.isEmpty()) {
            Device d;
            d.name = modelName;
            d.manufacturer = vendorId;
            d.status = "Working properly";
            d.driver = "(kernel)";
            d.driverVersion = kernelRelease();
            QString host = dmiProductName();
            d.location = host.isEmpty() ? QString() : "on " + host;
            out.append(d);
            modelName.clear();
            vendorId.clear();
        }
    }

    return out;
}

QVector<Device> scanDisks() {
    QVector<Device> out;
    QDir base("/sys/block");
    for (const QString &entry : base.entryList(
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.startsWith("loop") || entry.startsWith("ram")
            || entry.startsWith("dm-") || entry.startsWith("sr")
            || entry.startsWith("zram"))
            continue;

        QString path = base.absoluteFilePath(entry);
        QString model;
        QString vendor;

        if (entry.startsWith("nvme")) {
            model = readSysFile(path + "/device/model");
            // For NVMe, vendor comes from the PCI subsystem.
            QString sysPath = QFileInfo(path + "/device").canonicalFilePath();
            QString vid = readHexId(sysPath + "/../vendor");
            vendor = pciIds().vendors.value(vid, "");
        } else {
            model = readSysFile(path + "/device/model");
            vendor = readSysFile(path + "/device/vendor");
        }

        if (model.isEmpty())
            continue;

        Device d;
        d.name = (vendor.isEmpty() ? "" : vendor + " ") + model;
        d.name = d.name.trimmed();
        d.manufacturer = vendor.isEmpty() ? "(Standard disk drives)" : vendor;
        d.status = "Working properly";
        QString driverLink = QFileInfo(
            path + "/device/driver").symLinkTarget();
        d.driver = QFileInfo(driverLink).fileName();
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = entry;
        d.location = friendlyLocation(
            QFileInfo(path + "/device").canonicalFilePath(), entry);
        out.append(d);
    }
    return out;
}

// Returns true if this USB device entry is a Bluetooth adapter — detected by
// interface class E0/01 (Wireless/Bluetooth) or by the btusb driver binding.
// Such devices are shown under "Bluetooth Radios" and should be excluded from
// the generic USB device and unknown-device lists to avoid duplicates.
bool isBluetoothUsbDevice(const QString &entry) {
    QDir base("/sys/bus/usb/devices");
    const QString ifPrefix = entry + ":";
    for (const QString &iface : base.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        if (!iface.startsWith(ifPrefix))
            continue;
        QString ifPath = QFileInfo(base.absoluteFilePath(iface)).canonicalFilePath();
        if (readSysFile(ifPath + "/bInterfaceClass").trimmed().compare(
                QStringLiteral("e0"), Qt::CaseInsensitive) == 0
            && readSysFile(ifPath + "/bInterfaceSubClass").trimmed() == QLatin1String("01"))
            return true;
        if (QFileInfo(QFileInfo(ifPath + "/driver").symLinkTarget()).fileName()
                == QLatin1String("btusb"))
            return true;
    }
    return false;
}

QVector<Device> scanUsbDevices(const QSet<QString> &excluded = {}) {
    QVector<Device> out;
    QDir base("/sys/bus/usb/devices");
    if (!base.exists())
        return out;

    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        // Skip interfaces (contain colon) and root hubs.
        if (entry.contains(':') || entry.startsWith("usb"))
            continue;
        if (excluded.contains(entry))
            continue;
        if (isBluetoothUsbDevice(entry))
            continue;

        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();

        QString product = readSysFile(path + "/product");
        QString mfr = readSysFile(path + "/manufacturer");
        QString devClass = readSysFile(path + "/bDeviceClass");

        // Skip hubs (class 09).
        if (devClass == "09")
            continue;

        if (product.isEmpty())
            continue;

        Device d;
        d.name = product;
        d.manufacturer = mfr.isEmpty() ? "(Standard USB device)" : mfr;
        d.status = "Working properly";
        d.driver = QFileInfo(
            QFileInfo(path + "/driver").symLinkTarget()).fileName();
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = entry;
        d.location = friendlyLocation(path, entry);
        out.append(d);
    }
    return out;
}

struct iOSBatteryInfo {
    int  capacity = -1;
    bool charging = false;
};

iOSBatteryInfo queryiOSBattery(const QString &udid) {
    QProcess proc;
    proc.start("ideviceinfo", {"-u", udid, "-q", "com.apple.mobile.battery"});
    iOSBatteryInfo info;
    if (!proc.waitForFinished(4000) || proc.exitCode() != 0)
        return info;
    for (const QString &line :
         QString::fromUtf8(proc.readAllStandardOutput()).split('\n')) {
        if (line.startsWith("BatteryCurrentCapacity:")) {
            bool ok;
            int v = line.section(':', 1).trimmed().toInt(&ok);
            if (ok) info.capacity = v;
        } else if (line.startsWith("BatteryIsCharging:")) {
            info.charging = line.section(':', 1).trimmed().toLower() == "true";
        }
    }
    return info;
}

// Returns a map keyed by UDID (with and without dashes) → battery info.
// The sysfs /serial file contains the UDID without dashes; idevice_id -l may
// return it with dashes (new-style UDIDs). Both forms are stored so the lookup
// in scanPortableDevices() works regardless of which format sysfs exposes.
QHash<QString, iOSBatteryInfo> scaniOSBatteries() {
    QHash<QString, iOSBatteryInfo> result;
    QProcess idProc;
    idProc.start("idevice_id", {"-l"});
    if (!idProc.waitForFinished(3000) || idProc.exitCode() != 0)
        return result;
    const QString udidList = QString::fromUtf8(
        idProc.readAllStandardOutput()).trimmed();
    if (udidList.isEmpty())
        return result;
    for (const QString &rawUdid : udidList.split('\n', Qt::SkipEmptyParts)) {
        const QString udid = rawUdid.trimmed();
        if (udid.isEmpty()) continue;
        iOSBatteryInfo batt = queryiOSBattery(udid);
        result.insert(udid, batt);
        const QString udidNoDash = QString(udid).remove('-');
        if (udidNoDash != udid)
            result.insert(udidNoDash, batt);
    }
    return result;
}

bool isPortableUsbDevice(const QString &entry, const QString &canonPath) {
    // Devices with a Still Image (MTP/PTP) interface — class 06
    QDir base("/sys/bus/usb/devices");
    QString prefix = entry + ":";
    for (const QString &e : base.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        if (!e.startsWith(prefix))
            continue;
        QString ifPath = QFileInfo(base.absoluteFilePath(e)).canonicalFilePath();
        if (readSysFile(ifPath + "/bInterfaceClass").trimmed() == "06")
            return true;
        // ipheth: iPhone/iPad ethernet-over-USB interface
        QString drv = QFileInfo(
            QFileInfo(ifPath + "/driver").symLinkTarget()).fileName();
        if (drv == "ipheth")
            return true;
    }
    // Apple iOS devices connected before the user trusts the host show only
    // vendor-specific interfaces — catch them by vendor + product name.
    if (readSysFile(canonPath + "/idVendor").trimmed().toLower() == "05ac") {
        QString product = readSysFile(canonPath + "/product").toLower();
        if (product.contains("iphone") || product.contains("ipad")
                || product.contains("ipod") || product.contains("apple tv"))
            return true;
    }
    return false;
}

QVector<Device> scanPortableDevices(QSet<QString> *usedEntries) {
    QVector<Device> out;
    QDir base("/sys/bus/usb/devices");
    if (!base.exists())
        return out;

    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        if (entry.contains(':') || entry.startsWith("usb"))
            continue;

        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();

        if (!isPortableUsbDevice(entry, path))
            continue;

        QString product = readSysFile(path + "/product");
        QString mfr = readSysFile(path + "/manufacturer");
        if (product.isEmpty())
            continue;

        Device d;
        d.name = product;
        d.manufacturer = mfr.isEmpty() ? "(Standard portable device)" : mfr;
        d.status = "Working properly";
        d.driver = QFileInfo(
            QFileInfo(path + "/driver").symLinkTarget()).fileName();
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = entry;
        d.location = friendlyLocation(path, entry);
        QString productLower = product.toLower();
        if (productLower.contains("ipad"))
            d.iconName = "computer-apple-ipad";
        else if (productLower.contains("iphone"))
            d.iconName = "phone-apple-iphone";
        else if (productLower.contains("ipod"))
            d.iconName = "multimedia-player-apple-ipod-touch";

        if (usedEntries)
            usedEntries->insert(entry);
        out.append(d);
    }
    return out;
}

QVector<Device> scanNetwork() {
    QVector<Device> out;
    QDir base("/sys/class/net");
    for (const QString &entry : base.entryList(
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry == "lo")
            continue;
        QString path = base.absoluteFilePath(entry);
        QString devLink = QFileInfo(path + "/device").symLinkTarget();
        if (devLink.isEmpty())
            continue;
        Device d = makePciDevice(
            QFileInfo(path + "/device").canonicalFilePath());
        if (d.name.isEmpty())
            continue;
        d.rawLocation = entry;
        out.append(d);
    }
    return out;
}

QString decodePnpId(quint8 b1, quint8 b2) {
    int v = (b1 << 8) | b2;
    auto field = [](int bits) -> QChar {
        int n = bits & 0x1f;
        if (n < 1 || n > 26) return QChar('?');
        return QChar('A' + n - 1);
    };
    QString s;
    s.append(field(v >> 10));
    s.append(field(v >> 5));
    s.append(field(v));
    return s;
}

const QHash<QString, QString> &pnpIds() {
    static const QHash<QString, QString> db = [] {
        QHash<QString, QString> h;
        QFile f(hwdataPath("pnp.ids"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            while (!in.atEnd()) {
                QString line = in.readLine();
                if (line.startsWith('#') || line.trimmed().isEmpty())
                    continue;
                if (line.length() < 4)
                    continue;
                h.insert(line.left(3).toUpper(), line.mid(4).trimmed());
            }
        }
        return h;
    }();
    return db;
}

QString pnpVendorName(const QString &pnpId) {
    return pnpIds().value(pnpId.toUpper(), pnpId);
}

QString parseEdid(const QByteArray &edid, QString *vendorOut) {
    if (edid.size() < 128)
        return {};
    QString pnp = decodePnpId(quint8(edid[8]), quint8(edid[9]));
    if (vendorOut)
        *vendorOut = pnpVendorName(pnp);
    for (int i = 0; i < 4; ++i) {
        int off = 54 + i * 18;
        if (off + 18 > edid.size())
            break;
        if (edid[off] == 0 && edid[off + 1] == 0
            && edid[off + 2] == 0 && edid[off + 3] == char(0xFC)) {
            QString name = QString::fromLatin1(
                edid.mid(off + 5, 13)).trimmed();
            while (name.endsWith(QChar(0x0a)))
                name.chop(1);
            if (!name.isEmpty())
                return name;
        }
    }
    return {};
}

QVector<Device> scanMonitors() {
    QVector<Device> out;
    QDir base("/sys/class/drm");
    for (const QString &entry : base.entryList(
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString edidPath = base.absoluteFilePath(entry) + "/edid";
        QFile f(edidPath);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QByteArray edid = f.read(32768);
        if (edid.isEmpty())
            continue;
        QString vendor;
        QString name = parseEdid(edid, &vendor);
        if (name.isEmpty())
            name = "Generic monitor";
        Device d;
        d.name = name;
        d.manufacturer = vendor.isEmpty() ? "(Standard monitor types)" : vendor;
        d.status = "Working properly";
        d.driver = "drm";
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.rawLocation = entry;
        QString gpuName;
        {
            static QRegularExpression cardRe(R"(card(\d+)-)");
            auto cm = cardRe.match(entry);
            if (cm.hasMatch()) {
                QString cardPath = QFileInfo(
                    base.absoluteFilePath("card" + cm.captured(1))
                ).canonicalFilePath();
                QString vid = readHexId(cardPath + "/device/vendor");
                QString did = readHexId(cardPath + "/device/device");
                if (!vid.isEmpty()) {
                    if (vid == "1002") {
                        QString rev = readSysFile(cardPath + "/device/revision")
                                          .trimmed().remove("0x").toUpper();
                        gpuName = amdgpuIds().value(did.toUpper() + ":" + rev);
                    }
                    if (gpuName.isEmpty())
                        gpuName = pciIds().devices.value(vid + ":" + did, {});
                }
            }
        }
        d.location = monitorLocation(entry, gpuName);
        out.append(d);
    }
    return out;
}

QVector<Device> scanMice() {
    QVector<Device> out;
    QDir base("/sys/class/input");
    QSet<QString> seenPhys;
    for (const QString &entry : base.entryList(
             {"input*"}, QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        QString name = readSysFile(path + "/name");
        if (name.isEmpty())
            continue;

        // Kernel reports relative-axis capability as a hex bitmap; bits 0–1 are REL_X/REL_Y.
        QString rel = readSysFile(path + "/capabilities/rel");
        if (rel.isEmpty())
            continue;
        bool ok;
        quint64 relBits = rel.trimmed().split(' ').last().toULongLong(&ok, 16);
        if (!ok || (relBits & 0x3) != 0x3)
            continue;

        QString phys = readSysFile(path + "/phys");
        if (!phys.isEmpty() && seenPhys.contains(phys))
            continue;
        if (!phys.isEmpty())
            seenPhys.insert(phys);

        Device d;
        d.name = name;
        d.manufacturer = usbVendorName(readSysFile(path + "/id/vendor"));
        d.status = "Working properly";
        d.driver = "input";
        d.driverVersion = kernelRelease();
        d.location = friendlyLocation(path, phys);
        out.append(d);
    }
    return out;
}

int popcountll(quint64 v) {
#ifdef __GNUC__
    return __builtin_popcountll(v);
#else
    int n = 0;
    while (v) { ++n; v &= v - 1; }
    return n;
#endif
}

QVector<Device> scanKeyboards() {
    QVector<Device> out;
    QDir base("/sys/class/input");
    QSet<QString> seenPhys;
    for (const QString &entry : base.entryList(
             {"input*"}, QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        QString name = readSysFile(path + "/name");
        if (name.isEmpty())
            continue;

        // Skip devices with REL_X+REL_Y capability — those are mice.
        QString rel = readSysFile(path + "/capabilities/rel");
        if (!rel.isEmpty()) {
            bool ok;
            quint64 relBits = rel.trimmed().split(' ').last()
                                  .toULongLong(&ok, 16);
            if (ok && (relBits & 0x3) == 0x3)
                continue;
        }

        // Real keyboards have many key bits set; use total popcount as the
        // heuristic (power buttons, knobs, etc. have far fewer than 20).
        QString key = readSysFile(path + "/capabilities/key");
        if (key.isEmpty())
            continue;
        int totalBits = 0;
        for (const QString &word : key.trimmed().split(' ')) {
            bool ok;
            quint64 v = word.toULongLong(&ok, 16);
            if (ok)
                totalBits += popcountll(v);
        }
        if (totalBits < 20)
            continue;

        // Reject media-key-only devices (e.g. Bluetooth AVRCP remotes): real
        // keyboards have letter keys (KEY_Q=16 … KEY_P=25) in the low word.
        {
            const QStringList keyWords = key.trimmed().split(' ');
            if (!keyWords.isEmpty()) {
                bool ok;
                quint64 lastWord = keyWords.last().toULongLong(&ok, 16);
                // Mask covers bits 16-25 (KEY_Q through KEY_P).
                if (!ok || (lastWord & 0x3FF0000ULL) == 0)
                    continue;
            }
        }

        QString phys = readSysFile(path + "/phys");
        if (!phys.isEmpty() && seenPhys.contains(phys))
            continue;
        if (!phys.isEmpty())
            seenPhys.insert(phys);

        Device d;
        d.name = name;
        d.manufacturer = usbVendorName(readSysFile(path + "/id/vendor"));
        d.status = "Working properly";
        d.driver = "input";
        d.driverVersion = kernelRelease();
        d.location = friendlyLocation(path, phys);
        out.append(d);
    }
    return out;
}

// known HID drivers for game controllers
const QSet<QString> kControllerDrivers = {
    "playstation", "sony", "nintendo", "xpad", "xpadneo",
};

QVector<Device> scanHidGeneric() {
    QVector<Device> out;
    QDir base("/sys/bus/hid/devices");
    if (!base.exists())
        return out;
    QSet<QString> seenNames;
    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        QString uevent = readSysFile(path + "/uevent");
        QString hidName;
        QString hidId;
        for (const QString &line : uevent.split('\n')) {
            if (line.startsWith("HID_NAME="))
                hidName = line.mid(9).trimmed();
            else if (line.startsWith("HID_ID="))
                hidId = line.mid(7).trimmed();
        }
        // USB HID devices (bus 0003) are already shown under Universal Serial
        // Bus devices — suppress the duplicate here, mirroring Win7's PnP tree.
        if (hidId.left(4).toUpper() == "0003")
            continue;
        if (hidName.isEmpty())
            hidName = entry;
        if (seenNames.contains(hidName))
            continue;
        seenNames.insert(hidName);
        Device d;
        d.name = hidName;
        d.manufacturer = "(Standard HID device)";
        d.status = "Working properly";
        d.driver = QFileInfo(
            QFileInfo(path + "/driver").symLinkTarget()).fileName();
        // game controllers get their own category
        if (kControllerDrivers.contains(d.driver))
            continue;
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = entry;
        d.location = friendlyLocation(path, entry);
        out.append(d);
    }
    return out;
}

QVector<Device> scanBluetooth() {
    QVector<Device> out;
    QDir base("/sys/class/bluetooth");
    if (!base.exists())
        return out;

    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        // Connection handles (e.g. "hci0:12") share the hci prefix but are
        // per-device ACL links, not local adapters — skip them.
        if (!entry.startsWith("hci") || entry.contains(':'))
            continue;

        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();

        QString name;
        QString mfr;
        QString devicePath; // path used for friendlyLocation()
        QString actualDriver;
        QDir deviceDir(path);
        int depth = 0;

        // USB root hubs (usb1, usb2, …) carry "xHCI Host Controller" etc. as
        // their product string.  Walking past the real BT USB device into the
        // root hub would misname the adapter, so we skip those directories.
        static const QRegularExpression kUsbRootHub(QStringLiteral(R"(^usb\d+$)"));

        while (deviceDir.cdUp() && ++depth <= 20) {
            const QString dirPath = deviceDir.absolutePath();

            // Capture the driver at the first sysfs level that exposes one
            // (typically the USB interface, e.g. 1-14:1.0).
            if (actualDriver.isEmpty())
                actualDriver = driverNameFor(dirPath);

            if (kUsbRootHub.match(QFileInfo(dirPath).fileName()).hasMatch())
                continue;

            // USB device with a product string — use it directly.
            QString product = readSysFile(dirPath + "/product");
            if (!product.isEmpty()) {
                name = product;
                mfr  = readSysFile(dirPath + "/manufacturer");
                devicePath = dirPath;
                break;
            }

            // USB device without a product string — look up via USB ID database.
            const QString idVendor = readSysFile(dirPath + "/idVendor").trimmed().toLower();
            if (!idVendor.isEmpty()) {
                const QString idProduct = readSysFile(dirPath + "/idProduct").trimmed().toLower();
                const IdsDb &db = usbIds();
                const QString devName    = db.devices.value(idVendor + ":" + idProduct);
                const QString vendorName = db.vendors.value(idVendor);
                name = devName.isEmpty()
                    ? (vendorName.isEmpty() ? QStringLiteral("Bluetooth Adapter")
                                            : vendorName + QStringLiteral(" Bluetooth Adapter"))
                    : devName;
                mfr = vendorName;
                devicePath = dirPath;
                break;
            }

            // PCIe-native Bluetooth — look up via PCI ID database.
            if (QFile::exists(dirPath + "/vendor") && QFile::exists(dirPath + "/class")) {
                const QString pciVendor  = readHexId(dirPath + "/vendor");
                const QString pciDevice  = readHexId(dirPath + "/device");
                const IdsDb &db = pciIds();
                const QString devName    = db.devices.value(pciVendor + ":" + pciDevice);
                const QString vendorName = db.vendors.value(pciVendor);
                name = devName.isEmpty()
                    ? (vendorName.isEmpty() ? QStringLiteral("Bluetooth Adapter")
                                            : vendorName + QStringLiteral(" Bluetooth Adapter"))
                    : devName;
                mfr = vendorName;
                devicePath = dirPath;
                break;
            }

            if (dirPath == "/sys/devices")
                break;
        }

        // Driver modinfo description as fallback when the ID database yielded
        // nothing specific.  Strips " driver" and any trailing version token so
        // "Generic Bluetooth USB driver ver 0.8" becomes "Generic Bluetooth USB".
        if ((name.isEmpty() || name == QStringLiteral("Bluetooth Adapter"))
                && !actualDriver.isEmpty()) {
            QString desc = runCmd("modinfo", {"-F", "description", "--", actualDriver});
            if (!desc.isEmpty()) {
                static const QRegularExpression kDriverSuffix(
                    QStringLiteral(R"(\s+[Dd]river\b.*)"));
                desc.remove(kDriverSuffix);
                desc = desc.trimmed();
            }
            if (!desc.isEmpty())
                name = desc;
        }

        if (name.isEmpty())
            name = entry;

        Device d;
        d.name = name;
        d.status = "Working properly";
        d.driver = actualDriver.isEmpty() ? QStringLiteral("btusb") : actualDriver;
        d.manufacturer = mfr.isEmpty() ? "(Standard Bluetooth device)" : mfr;
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = entry;
        bool ok;
        int hciNum = entry.mid(3).toInt(&ok);
        if (!devicePath.isEmpty())
            d.location = friendlyLocation(devicePath, {});
        else
            d.location = ok ? QString("Bluetooth adapter %1").arg(hciNum) : entry;
        out.append(d);
    }
    return out;
}

QVector<Device> scanTpm() {
    QVector<Device> out;
    QDir base("/sys/class/tpm");
    if (!base.exists())
        return out;
    for (const QString &entry : base.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = base.absoluteFilePath(entry);
        QString major = readSysFile(path + "/tpm_version_major");
        QString name = major == "2"
            ? "Trusted Platform Module 2.0"
            : "Trusted Platform Module";
        Device d;
        d.name = name;
        d.manufacturer = "(Standard)";
        d.status = "Working properly";
        d.driver = "tpm";
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.rawLocation = entry;
        d.location = friendlyLocation(
            QFileInfo(path).canonicalFilePath(), entry);
        out.append(d);
    }
    return out;
}

QVector<Device> scanOpticalDrives() {
    QVector<Device> out;
    QDir base("/sys/block");
    for (const QString &entry : base.entryList(
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!entry.startsWith("sr"))
            continue;
        QString path = base.absoluteFilePath(entry);
        QString model = readSysFile(path + "/device/model");
        QString vendor = readSysFile(path + "/device/vendor");
        Device d;
        d.name = ((vendor.isEmpty() ? "" : vendor + " ") + model).trimmed();
        if (d.name.isEmpty())
            d.name = entry;
        d.manufacturer = vendor.isEmpty() ? "(Standard CD-ROM drives)" : vendor;
        d.status = "Working properly";
        QString drv = QFileInfo(
            path + "/device/driver").symLinkTarget();
        d.driver = QFileInfo(drv).fileName();
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = entry;
        d.location = friendlyLocation(
            QFileInfo(path + "/device").canonicalFilePath(), entry);
        out.append(d);
    }
    return out;
}


// checks whether a power_supply battery at the given canonical path is
// parented by a game controller HID driver
bool isControllerBattery(const QString &canonicalPath) {
    QDir dir(canonicalPath);
    int depth = 0;
    while (dir.cdUp() && ++depth <= 20) {
        QString uevent = readSysFile(dir.absolutePath() + "/uevent");
        for (const QString &line : uevent.split('\n')) {
            if (line.startsWith("DRIVER=")) {
                if (kControllerDrivers.contains(line.mid(7).trimmed()))
                    return true;
            }
        }
        if (dir.absolutePath() == "/sys/devices") break;
    }
    return false;
}

QVector<Device> scanHidBatteries() {
    QVector<Device> out;
    QDir base("/sys/class/power_supply");
    if (!base.exists())
        return out;

    for (const QString &entry : base.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        QString type = readSysFile(path + "/type");

        if (type != "Battery")
            continue;
        if (!path.contains("hid", Qt::CaseInsensitive))
            continue;
        if (isControllerBattery(path))
            continue;

        QString name;
        QString mfr;
        QDir dir(path);
        int depth = 0;
        while (dir.cdUp() && ++depth <= 20) {
            QString uevent = readSysFile(dir.absolutePath() + "/uevent");
            for (const QString &line : uevent.split('\n')) {
                if (line.startsWith("HID_NAME=")) {
                    name = line.mid(9).trimmed();
                    break;
                }
            }
            if (!name.isEmpty()) break;
            if (dir.absolutePath() == "/sys/devices") break;
        }

        if (name.isEmpty())
            name = entry;

        QString capacity = readSysFile(path + "/capacity");
        QString capacityLevel = readSysFile(path + "/capacity_level");
        QString deviceStatus = "Working properly";
        if (!capacity.isEmpty())
            deviceStatus = QString("Battery level: %1%").arg(capacity);
        else if (!capacityLevel.isEmpty() && capacityLevel != "Unknown")
            deviceStatus = QString("Battery level: %1").arg(capacityLevel);

        Device d;
        d.name = "Battery on " + name;
        d.manufacturer = mfr.isEmpty() ? "(Standard)" : mfr;
        d.status = deviceStatus;
        d.driver = "usbhid";
        d.driverVersion = kernelRelease();
        d.rawLocation = entry;
        d.location = name.isEmpty() ? friendlyLocation(path, entry)
                                    : "on " + name;
        out.append(d);
    }
    return out;
}

QVector<Device> scanSoundCards() {
    QVector<Device> out;
    QDir base("/sys/class/sound");
    QSet<QString> seen;

    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        if (!entry.startsWith("card"))
            continue;
        bool isNum = false;
        entry.mid(4).toInt(&isNum);
        if (!isNum)
            continue;

        QString cardPath = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        QString devicePath = QFileInfo(
            cardPath + "/device").canonicalFilePath();
        if (devicePath.isEmpty() || devicePath == cardPath)
            continue;

        QString name;
        QString mfr;

        QDir deviceDir(devicePath);
        if (deviceDir.cdUp()) {
            QString product = readSysFile(
                deviceDir.absolutePath() + "/product");
            QString manufacturer = readSysFile(
                deviceDir.absolutePath() + "/manufacturer");
            if (!product.isEmpty()) {
                name = product;
                mfr = manufacturer;
            }
        }

        if (name.isEmpty()) {
            QString vendor = readHexId(devicePath + "/vendor");
            QString device = readHexId(devicePath + "/device");
            if (!vendor.isEmpty()) {
                mfr = pciIds().vendors.value(vendor, "(Standard)");
                name = pciIds().devices.value(
                    vendor + ":" + device,
                    QStringLiteral("Audio Device %1:%2")
                        .arg(vendor, device));
            }
        }

        if (name.isEmpty())
            name = readSysFile(cardPath + "/id");
        if (name.isEmpty())
            name = entry;

        if (seen.contains(name))
            continue;
        seen.insert(name);

        QString driver = driverNameFor(devicePath);

        Device d;
        d.name = name;
        d.manufacturer = mfr.isEmpty() ? "(Standard)" : mfr;
        d.status = "Working properly";
        d.driver = driver.isEmpty() ? "snd" : driver;
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = entry;
        d.location = friendlyLocation(devicePath, entry);
        if (devicePath.contains("/pci"))
            d.sysfsPciPath = devicePath;
        out.append(d);
    }
    return out;
}

QVector<Device> scanBatteries() {
    QVector<Device> out;
    QDir base("/sys/class/power_supply");
    if (!base.exists())
        return out;
    const QHash<QString, iOSBatteryInfo> iosBatteries = scaniOSBatteries();
    QString hostModel = dmiProductName();
    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = base.absoluteFilePath(entry);
        QString type = readSysFile(path + "/type");
        if (type != "Battery")
            continue;
        if (entry.contains("hidpp", Qt::CaseInsensitive))
            continue;

        QString canonicalPath = QFileInfo(path).canonicalFilePath();
        QString model = readSysFile(path + "/model_name");
        QString mfr = readSysFile(path + "/manufacturer");
        Device d;

        if (entry.startsWith("apple_mfi_fastcharge_")) {
            // Walk up two levels: power_supply/<entry> → power_supply/ → USB device dir
            QDir dir(canonicalPath);
            dir.cdUp();
            dir.cdUp();
            QString usbProduct = readSysFile(dir.absolutePath() + "/product").trimmed();
            QString usbMfr = readSysFile(dir.absolutePath() + "/manufacturer").trimmed();
            if (usbProduct.isEmpty())
                usbProduct = "Apple Device";
            d.name = "Battery on " + usbProduct;
            d.location = "on " + usbProduct;
            d.manufacturer = usbMfr.isEmpty() ? "Apple Inc." : usbMfr;
            // Read battery level via libimobiledevice if the device is trusted.
            d.status = "No battery level";
            if (!iosBatteries.isEmpty()) {
                const QString usbSerial = readSysFile(
                    dir.absolutePath() + "/serial").trimmed();
                if (!usbSerial.isEmpty() && iosBatteries.contains(usbSerial)) {
                    const iOSBatteryInfo &batt = iosBatteries.value(usbSerial);
                    if (batt.capacity >= 0) {
                        d.status = QString("Battery level: %1%").arg(batt.capacity);
                        if (batt.charging)
                            d.status += " (charging)";
                    }
                }
            }
            d.driver = "apple-mfi-fastcharge";
            DriverInfo di = moduleDriverInfo(d.driver);
            d.driverVersion = di.version;
            d.driverAuthor = di.author;
            d.driverDate = di.date;
            d.rawLocation = entry;
            out.append(d);
            continue;
        }

        if (isControllerBattery(canonicalPath)) {
            QString hidName;
            QDir dir(canonicalPath);
            int depth = 0;
            while (dir.cdUp() && ++depth <= 20) {
                QString uevent = readSysFile(dir.absolutePath() + "/uevent");
                for (const QString &line : uevent.split('\n')) {
                    if (line.startsWith("HID_NAME=")) {
                        hidName = line.mid(9).trimmed();
                        break;
                    }
                }
                if (!hidName.isEmpty()) break;
                if (dir.absolutePath() == "/sys/devices") break;
            }
            if (hidName.isEmpty())
                hidName = model.isEmpty() ? entry : model;
            d.name = "Battery on " + hidName;
            d.location = "on " + hidName;
            QString capacity = readSysFile(path + "/capacity");
            QString capacityLevel = readSysFile(path + "/capacity_level");
            if (!capacity.isEmpty())
                d.status = QString("Battery level: %1%").arg(capacity);
            else if (!capacityLevel.isEmpty() && capacityLevel != "Unknown")
                d.status = QString("Battery level: %1").arg(capacityLevel);
            else
                d.status = "Working properly";
        } else {
            d.name = model.isEmpty() ? entry : model;
            d.location = hostModel.isEmpty()
                ? friendlyLocation(canonicalPath, entry)
                : "on " + hostModel;
            d.status = "Working properly";
        }

        d.manufacturer = mfr.isEmpty() ? "(Standard)" : mfr;
        d.driver = "battery";
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.rawLocation = entry;
        out.append(d);
    }
    return out;
}

QVector<Device> scanGameControllers() {
    QVector<Device> out;
    QDir base("/sys/class/power_supply");
    if (!base.exists())
        return out;

    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        QString type = readSysFile(path + "/type");
        if (type != "Battery")
            continue;
        if (!isControllerBattery(path))
            continue;

        // walk up sysfs to find the HID device name, driver, and bus type
        QString name;
        QString mfr;
        QString driverName;
        QString hidId;   // "BUS:VENDOR:PRODUCT" — BUS 0005=BT, 0003=USB
        QString hidUniq; // unique address (BT MAC for Bluetooth devices)
        QDir dir(path);
        int depth = 0;
        while (dir.cdUp() && ++depth <= 20) {
            QString dirPath = dir.absolutePath();
            QString uevent = readSysFile(dirPath + "/uevent");
            for (const QString &line : uevent.split('\n')) {
                if (line.startsWith("HID_NAME=") && name.isEmpty())
                    name = line.mid(9).trimmed();
                else if (line.startsWith("DRIVER=") && driverName.isEmpty())
                    driverName = line.mid(7).trimmed();
                else if (line.startsWith("HID_ID=") && hidId.isEmpty())
                    hidId = line.mid(7).trimmed();
                else if (line.startsWith("HID_UNIQ=") && hidUniq.isEmpty())
                    hidUniq = line.mid(9).trimmed();
            }
            if (mfr.isEmpty()) {
                QString m = readSysFile(dirPath + "/manufacturer");
                if (!m.isEmpty())
                    mfr = m;
            }
            if (!name.isEmpty()) break;
            if (dirPath == "/sys/devices") break;
        }

        if (name.isEmpty())
            name = readSysFile(path + "/model_name");
        if (name.isEmpty())
            name = entry;

        // HID_UNIQ holds the BT MAC for Bluetooth devices; fall back to
        // extracting a MAC from the entry name for older kernel layouts.
        QString btAddr = hidUniq;
        if (btAddr.isEmpty()) {
            static const QRegularExpression btAddrRe(
                R"(([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})$)");
            auto addrMatch = btAddrRe.match(entry);
            if (addrMatch.hasMatch())
                btAddr = addrMatch.captured(1);
        }

        QString deviceStatus = "Working properly";

        // Derive manufacturer from HID_ID vendor field (USB vendor ID registry).
        // HID_ID format: "BUS:VENDOR:PRODUCT" — vendor field is 8 hex digits,
        // last 4 match the USB/BT vendor IDs used in usb.ids.
        QString manufacturer = mfr;
        if (manufacturer.isEmpty() && !hidId.isEmpty()) {
            QStringList hidParts = hidId.split(':');
            if (hidParts.size() >= 2) {
                QString vendorHex = hidParts[1].right(4).toLower();
                QString looked = usbIds().vendors.value(vendorHex);
                if (!looked.isEmpty())
                    manufacturer = looked;
            }
        }

        Device d;
        d.name = name;
        d.manufacturer = manufacturer.isEmpty()
            ? "(Standard game controller)" : manufacturer;
        d.status = deviceStatus;
        d.driver = driverName;
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = entry;
        d.btAddress = btAddr;
        // HID_ID first field is the kernel bus type: 0005=BT, 0003=USB.
        // Fall back to sysfs path heuristics if HID_ID is unavailable.
        if (!hidId.isEmpty()) {
            QString busHex = hidId.left(4).toUpper();
            if (busHex == "0005")
                d.location = "connected via Bluetooth";
            else if (busHex == "0003")
                d.location = "connected via USB";
            else
                d.location = "Unknown";
        } else if (!btAddr.isEmpty() || path.contains("/bluetooth/")) {
            d.location = "connected via Bluetooth";
        } else if (path.contains("/usb")) {
            d.location = "connected via USB";
        } else {
            d.location = "Unknown";
        }
        out.append(d);
    }
    return out;
}

QVector<Device> scanRtc() {
    QVector<Device> out;
    QDir base("/sys/class/rtc");
    if (!base.exists())
        return out;
    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        QString rtcName = readSysFile(path + "/name");
        QString drvToken = rtcName.section(' ', 0, 0);
        Device d;
        d.name = "System CMOS/real time clock";
        d.manufacturer = "(Standard system devices)";
        d.status = "Working properly";
        d.driver = drvToken.isEmpty() ? "rtc_cmos" : drvToken;
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.rawLocation = entry;
        d.location = []{
            QString host = dmiProductName();
            return host.isEmpty() ? QStringLiteral("ACPI system device") : "on " + host;
        }();
        out.append(d);
    }
    return out;
}

QVector<Device> scanPlatformDevices() {
    static const QHash<QString, QString> kKnownPlatform = {
        {"pcspkr",    "System speaker"},
        {"eeepc-wmi", "ASUS WMI device"},
    };
    QVector<Device> out;
    QDir base("/sys/bus/platform/devices");
    if (!base.exists())
        return out;
    QSet<QString> seenDrivers;
    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        QString drvLink = QFileInfo(path + "/driver").symLinkTarget();
        QString drv = QFileInfo(drvLink).fileName();
        if (drv.isEmpty() || seenDrivers.contains(drv))
            continue;
        auto it = kKnownPlatform.constFind(drv);
        if (it == kKnownPlatform.constEnd())
            continue;
        seenDrivers.insert(drv);
        Device d;
        d.name = *it;
        d.manufacturer = "(Standard system devices)";
        d.status = "Working properly";
        d.driver = drv;
        DriverInfo di = moduleDriverInfo(drv);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.rawLocation = entry;
        d.location = []{
            QString host = dmiProductName();
            return host.isEmpty() ? QStringLiteral("ACPI system device") : "on " + host;
        }();
        out.append(d);
    }
    return out;
}

QVector<Device> scanStorageControllers() {
    return scanPciByClass("01");
}

QVector<Device> scanIdeControllers() {
    return scanPciByClass("0101");
}

QVector<Device> scanUsbControllers() {
    return scanPciByClass("0c03");
}

QVector<Device> scanSystemDevices() {
    QVector<Device> out;
    QDir base("/sys/bus/pci/devices");
    for (const QString &entry : base.entryList(
             QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString path = base.absoluteFilePath(entry);
        QString cls = readHexId(path + "/class");
        QString prefix2 = cls.left(2);
        if (prefix2 == "01" || prefix2 == "02" || prefix2 == "03"
            || prefix2 == "04" || cls.startsWith("0c03"))
            continue;

        Device d = makePciDevice(path);

        if (d.name.contains("Dummy", Qt::CaseInsensitive))
            continue;
        if (d.name.contains("Placeholder", Qt::CaseInsensitive))
            continue;

        d.rawLocation = entry;
        out.append(d);
    }
    return out;
}

QVector<Device> scanBlacklistedMissing(
    const QVector<DeviceCategory> &found) {

    QSet<QString> foundDrivers;
    for (const auto &cat : found)
        for (const auto &dev : cat.devices)
            foundDrivers.insert(dev.driver);

    QFile f(kModprobeConf);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QVector<Device> out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (!line.startsWith("blacklist "))
            continue;
        QString mod = line.mid(10).trimmed();
        if (mod.isEmpty() || foundDrivers.contains(mod))
            continue;
        static const QRegularExpression kModRe("^[a-zA-Z0-9_-]+$");
        if (!kModRe.match(mod).hasMatch())
            continue;

        Device d;
        d.driver = mod;
        d.disabled = true;

        QString desc = runCmd("modinfo", {"-F", "description", "--", mod});
        d.name = desc.isEmpty() ? mod : desc;

        DriverInfo di = moduleDriverInfo(mod);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.status = "Working properly";
        d.manufacturer = "(Standard)";
        d.iconName = "preferences-system";
        out.append(d);
    }
    return out;
}

QVector<Device> scanBluetoothAudio() {
    QVector<Device> out;

    QString devList = runCmd("bluetoothctl", {"devices"}, 3000);
    if (devList.isEmpty())
        return out;

    static const QRegularExpression devRe(
        R"(Device\s+([0-9A-Fa-f:]{17})\s+(.+))");
    static const QSet<QString> kAudioUuids = {
        "0000110a", // Audio Source
        "0000110b", // Audio Sink (A2DP)
        "00001108", // Headset
        "0000111e", // Handsfree (HFP)
        "00001112", // Headset AG
        "0000111f", // Handsfree AG
    };

    for (const QString &line : devList.split('\n')) {
        auto m = devRe.match(line);
        if (!m.hasMatch())
            continue;

        QString addr = m.captured(1);
        QString name = m.captured(2).trimmed();

        QString info = runCmd("bluetoothctl", {"info", addr}, 3000);
        if (info.isEmpty())
            continue;

        bool connected = false;
        bool isAudio = false;
        QString mfr;

        static const QRegularExpression modRe(
            R"(bluetooth:v([0-9A-Fa-f]{4})p)");

        for (const QString &il : info.split('\n')) {
            QString t = il.trimmed();
            if (t.startsWith("Connected:") && t.contains("yes"))
                connected = true;
            if (t.startsWith("Icon:") && t.contains("audio"))
                isAudio = true;
            for (const QString &uuid : kAudioUuids) {
                if (t.contains(uuid, Qt::CaseInsensitive)) {
                    isAudio = true;
                    break;
                }
            }
            auto mm = modRe.match(t);
            if (mm.hasMatch() && mfr.isEmpty()) {
                QString vid = mm.captured(1).toLower();
                mfr = btCompanyName(vid);
                if (mfr.isEmpty())
                    mfr = usbVendorName(vid);
            }
        }

        if (!connected || !isAudio)
            continue;

        Device d;
        d.name = name;
        d.manufacturer = mfr.isEmpty() ? "(Bluetooth audio device)" : mfr;
        d.status = "Working properly";
        d.driver = "btusb";
        DriverInfo di = moduleDriverInfo(d.driver);
        d.driverVersion = di.version;
        d.driverAuthor = di.author;
        d.driverDate = di.date;
        d.isDkms = di.isDkms;
        d.rawLocation = addr;
        d.btAddress = addr;
        d.location = "connected via Bluetooth";
        out.append(d);
    }
    return out;
}

QVector<Device> scanUnknownUsbDevices() {
    QVector<Device> out;
    QDir base("/sys/bus/usb/devices");
    if (!base.exists())
        return out;
    for (const QString &entry : base.entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot)) {
        if (entry.contains(':') || entry.startsWith("usb"))
            continue;
        if (isBluetoothUsbDevice(entry))
            continue;
        QString path = QFileInfo(
            base.absoluteFilePath(entry)).canonicalFilePath();
        if (readSysFile(path + "/bDeviceClass") == "09")
            continue;
        if (!readSysFile(path + "/product").isEmpty())
            continue;
        QString vendorHex = readSysFile(path + "/idVendor").trimmed().toLower();
        QString productHex = readSysFile(path + "/idProduct").trimmed().toLower();
        if (vendorHex.isEmpty())
            continue;
        Device d;
        QString vendorName = usbIds().vendors.value(vendorHex);
        d.name = vendorName.isEmpty()
            ? QString("Unknown device %1:%2").arg(vendorHex, productHex)
            : QString("Unknown %1 device").arg(vendorName);
        d.manufacturer = vendorName.isEmpty() ? "Unknown" : vendorName;
        d.status = "Unknown";
        d.rawLocation = entry;
        d.location = friendlyLocation(path, entry);
        out.append(d);
    }
    return out;
}

} // namespace

QVector<DeviceCategory> scanDevices() {
    QVector<DeviceCategory> cats;

    auto addIfAny = [&](const QString &name, const QString &icon,
                        QVector<Device> devs) {
        if (!devs.isEmpty())
            cats.append({name, icon, devs});
    };

    auto batteries = scanBatteries();
    batteries += scanHidBatteries();
    addIfAny("Batteries", "kded5", batteries);
    addIfAny("Bluetooth Radios", "network-bluetooth", scanBluetooth());
    addIfAny("Game controllers", "input-gaming",
             scanGameControllers());
    addIfAny("Disk drives", "drive-harddisk", scanDisks());
    addIfAny("Display adapters", "video-display", scanPciByClass("03"));
    addIfAny("DVD/CD-ROM drives", "media-optical", scanOpticalDrives());
    addIfAny("Human Interface Devices", "input_devices_settings", scanHidGeneric());
    addIfAny("IDE ATA/ATAPI controllers", "drive-harddisk",
             scanIdeControllers());
    addIfAny("Keyboards", "input-keyboard", scanKeyboards());
    addIfAny("Mice and other pointing devices", "input-mouse", scanMice());
    addIfAny("Monitors", "video-display", scanMonitors());
    addIfAny("Network adapters", "network-card", scanNetwork());
    QSet<QString> portableEntries;
    addIfAny("Portable Devices", "smartphone",
             scanPortableDevices(&portableEntries));
    addIfAny("Processors", "cpu", scanCpus());
    addIfAny("Security Devices", "drive-harddisk-encrypted", scanTpm());
    auto soundDevs = scanSoundCards();
    soundDevs += scanBluetoothAudio();
    addIfAny("Sound, video and game controllers", "kmix", soundDevs);
    addIfAny("Storage controllers", "drive-harddisk",
             scanStorageControllers());
    auto sysDevs = scanSystemDevices();
    sysDevs += scanRtc();
    sysDevs += scanPlatformDevices();
    addIfAny("System devices", "computer", sysDevs);
    addIfAny("Universal Serial Bus controllers",
             "drive-removable-media-usb", scanUsbControllers());
    addIfAny("Universal Serial Bus devices",
             "drive-removable-media-usb", scanUsbDevices(portableEntries));
    addIfAny("Unknown devices", "dialog-question", scanUnknownUsbDevices());

    QVector<Device> missing = scanBlacklistedMissing(cats);
    if (!missing.isEmpty())
        cats.append({"__disabled__", "", missing});

    return cats;
}