#include "AirPodsScanner.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QEventLoop>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

using BluezInterfaces = QMap<QString, QVariantMap>;
using BluezObjects    = QMap<QDBusObjectPath, BluezInterfaces>;
using BluezVendorData = QMap<quint16, QDBusVariant>;

Q_DECLARE_METATYPE(BluezInterfaces)
Q_DECLARE_METATYPE(BluezObjects)
Q_DECLARE_METATYPE(BluezVendorData)

namespace {

constexpr char kBluezService[] = "org.bluez";
constexpr char kAdapterIface[] = "org.bluez.Adapter1";
constexpr char kDeviceIface[]  = "org.bluez.Device1";
constexpr char kPropsIface[]   = "org.freedesktop.DBus.Properties";
constexpr char kObjMgrIface[]  = "org.freedesktop.DBus.ObjectManager";

constexpr quint16 kAppleCompanyId   = 0x004C;
constexpr quint8  kProximityPairing = 0x07;
// One type byte, one length byte, then 25 bytes of payload. Apple also sends
// shorter 0x07 advertisements while a headset is in pairing mode; those use a
// different layout and carry no battery levels.
constexpr int kAdvertLength = 27;

// A2DP Audio Sink. Apple sells Bluetooth peripherals that are not headsets,
// and only headsets broadcast battery levels.
constexpr char kAudioSinkUuid[] = "0000110b-0000-1000-8000-00805f9b34fb";

// Discovery competes with audio for the radio, so the budget is kept tight.
// A headset that is awake is normally heard within a second.
constexpr int kScanBudgetMs = 3000;
constexpr int kSettleMs     = 400;   // keep listening briefly after a match

void registerBluezTypes() {
    static const bool done = [] {
        qDBusRegisterMetaType<BluezInterfaces>();
        qDBusRegisterMetaType<BluezObjects>();
        qDBusRegisterMetaType<BluezVendorData>();
        return true;
    }();
    Q_UNUSED(done);
}

// Levels arrive as a single nibble: 0-10 for 0-100% in steps of ten, and 15
// when that component is not reporting.
int levelFromNibble(quint8 nibble) {
    return nibble > 10 ? -1 : nibble * 10;
}

// Decodes the plaintext head of an Apple proximity-pairing advertisement. Only
// the first ten bytes are readable; everything after them is encrypted.
bool decodeAdvert(const QByteArray &a, quint16 *model, AirPodsBattery *out) {
    if (a.size() != kAdvertLength || quint8(a.at(0)) != kProximityPairing)
        return false;

    *model = quint16(quint8(a.at(4)) << 8) | quint8(a.at(3));

    // Either pod can be the primary of the pair, and the advertisement lists
    // the primary first. This bit says which way round the pair currently is,
    // so it decides which nibble belongs to which ear.
    const bool flipped = ((quint8(a.at(5)) >> 4) & 0x02) == 0;

    const quint8 primary   = quint8(a.at(6)) >> 4;
    const quint8 secondary = quint8(a.at(6)) & 0x0F;
    out->left  = levelFromNibble(flipped ? primary   : secondary);
    out->right = levelFromNibble(flipped ? secondary : primary);

    const quint8 charging = quint8(a.at(7)) >> 4;
    out->caseLevel     = levelFromNibble(quint8(a.at(7)) & 0x0F);
    out->leftCharging  = charging & (flipped ? 0x02 : 0x01);
    out->rightCharging = charging & (flipped ? 0x01 : 0x02);
    out->caseCharging  = charging & 0x04;
    return true;
}

bool managedObjects(const QDBusConnection &bus, BluezObjects *out) {
    QDBusMessage call = QDBusMessage::createMethodCall(
        kBluezService, "/", kObjMgrIface, "GetManagedObjects");
    const QDBusMessage reply = bus.call(call, QDBus::Block, 4000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return false;
    *out = qdbus_cast<BluezObjects>(reply.arguments().constFirst());
    return true;
}

QString poweredAdapter(const BluezObjects &objects) {
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const auto iface = it.value().constFind(QLatin1String(kAdapterIface));
        if (iface == it.value().constEnd())
            continue;
        if (iface.value().value(QStringLiteral("Powered")).toBool())
            return it.key().path();
    }
    return {};
}

// Connected Apple headsets, identified by the vendor and product in their
// Device ID record. The product id also appears in the advertisement, which is
// how a captured advertisement is tied back to a device that is really ours
// rather than to a neighbour's headset.
QVector<AirPodsDevice> appleAudioFrom(const BluezObjects &objects) {
    static const QRegularExpression modaliasRe(
        QStringLiteral("^bluetooth:v004Cp([0-9A-Fa-f]{4})d"),
        QRegularExpression::CaseInsensitiveOption);

    QVector<AirPodsDevice> out;
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const auto iface = it.value().constFind(QLatin1String(kDeviceIface));
        if (iface == it.value().constEnd())
            continue;
        const QVariantMap props = iface.value();
        if (!props.value(QStringLiteral("Connected")).toBool())
            continue;

        const auto m = modaliasRe.match(props.value(QStringLiteral("Modalias")).toString());
        if (!m.hasMatch())
            continue;

        const bool isAudio =
            props.value(QStringLiteral("Icon")).toString().startsWith(QStringLiteral("audio"))
            || props.value(QStringLiteral("UUIDs")).toStringList()
                   .contains(QLatin1String(kAudioSinkUuid), Qt::CaseInsensitive);
        if (!isAudio)
            continue;

        AirPodsDevice dev;
        dev.model   = m.captured(1).toUShort(nullptr, 16);
        dev.name    = props.value(QStringLiteral("Alias")).toString();
        dev.address = props.value(QStringLiteral("Address")).toString();
        if (dev.name.isEmpty())
            dev.name = props.value(QStringLiteral("Name")).toString();
        if (dev.name.isEmpty())
            dev.name = dev.address;
        out.append(dev);
    }
    return out;
}

int rssiFor(const BluezObjects &objects, const QString &path) {
    const auto obj = objects.constFind(QDBusObjectPath(path));
    if (obj == objects.constEnd())
        return -32768;
    const auto dev = obj.value().constFind(QLatin1String(kDeviceIface));
    if (dev == obj.value().constEnd())
        return -32768;
    return dev.value().value(QStringLiteral("RSSI"), -32768).toInt();
}

} // namespace

// Collects matching advertisements as BlueZ reports them.
//
// The advertisements have to be taken from the signals themselves rather than
// polled off the device objects: BlueZ keeps only the most recent vendor data
// per company id, and Apple headsets interleave several advertisement types, so
// by the time a poll runs the battery advertisement has usually been overwritten
// by an unrelated one.
class AirPodsAdvertCollector : public QObject {
    Q_OBJECT
public:
    quint16 wantedModel = 0;
    QHash<QString, QByteArray> hits;   // object path -> advertisement
    QEventLoop loop;

    AirPodsAdvertCollector() {
        m_settle.setSingleShot(true);
        connect(&m_settle, &QTimer::timeout, &loop, &QEventLoop::quit);
    }

public slots:
    void onInterfacesAdded(const QDBusMessage &msg) {
        const QList<QVariant> args = msg.arguments();
        if (args.size() < 2)
            return;
        const BluezInterfaces ifaces = qdbus_cast<BluezInterfaces>(args.at(1));
        const auto dev = ifaces.constFind(QLatin1String(kDeviceIface));
        if (dev != ifaces.constEnd())
            consider(args.at(0).value<QDBusObjectPath>().path(), dev.value());
    }

    void onPropertiesChanged(const QDBusMessage &msg) {
        const QList<QVariant> args = msg.arguments();
        if (args.size() < 2 || args.at(0).toString() != QLatin1String(kDeviceIface))
            return;
        consider(msg.path(), qdbus_cast<QVariantMap>(args.at(1)));
    }

private:
    void consider(const QString &path, const QVariantMap &props) {
        const auto raw = props.constFind(QStringLiteral("ManufacturerData"));
        if (raw == props.constEnd())
            return;
        const BluezVendorData vendor = qdbus_cast<BluezVendorData>(raw.value());
        const auto apple = vendor.constFind(kAppleCompanyId);
        if (apple == vendor.constEnd())
            return;

        const QByteArray advert = apple.value().variant().toByteArray();
        quint16 model = 0;
        AirPodsBattery probe;
        if (!decodeAdvert(advert, &model, &probe) || model != wantedModel)
            return;

        hits.insert(path, advert);
        // Give any other headset of the same model a moment to be heard too, so
        // the closest one can be picked rather than simply the first heard.
        if (!m_settle.isActive())
            m_settle.start(kSettleMs);
    }

    QTimer m_settle;
};

QVector<AirPodsDevice> connectedAirPods() {
    registerBluezTypes();

    QVector<AirPodsDevice> result;
    const QString connName = QStringLiteral("devmgmt-airpods-list");
    // Scoped so the connection is released before it is torn down; the socket
    // stays open for as long as any handle to it survives.
    {
        QDBusConnection bus =
            QDBusConnection::connectToBus(QDBusConnection::SystemBus, connName);

        BluezObjects objects;
        if (bus.isConnected() && managedObjects(bus, &objects))
            result = appleAudioFrom(objects);
    }
    QDBusConnection::disconnectFromBus(connName);
    return result;
}

namespace {

AirPodsBattery captureBattery(QDBusConnection &bus, const QString &address) {
    AirPodsBattery result;

    BluezObjects objects;
    if (!bus.isConnected() || !managedObjects(bus, &objects))
        return result;

    quint16 model = 0;
    for (const AirPodsDevice &dev : appleAudioFrom(objects)) {
        if (dev.address.compare(address, Qt::CaseInsensitive) == 0) {
            model = dev.model;
            break;
        }
    }
    const QString adapterPath = poweredAdapter(objects);
    if (model == 0 || adapterPath.isEmpty())
        return result;

    AirPodsAdvertCollector collector;
    collector.wantedModel = model;

    bus.connect(kBluezService, QString(), kObjMgrIface, "InterfacesAdded",
                &collector, SLOT(onInterfacesAdded(QDBusMessage)));
    bus.connect(kBluezService, QString(), kPropsIface, "PropertiesChanged",
                &collector, SLOT(onPropertiesChanged(QDBusMessage)));

    QDBusInterface adapter(kBluezService, adapterPath, kAdapterIface, bus);
    const QVariantMap filter{
        {QStringLiteral("Transport"), QStringLiteral("le")},
        // Without this BlueZ reports only the first advertisement from each
        // address, and the battery one may well not be it.
        {QStringLiteral("DuplicateData"), true},
    };
    adapter.call(QStringLiteral("SetDiscoveryFilter"), filter);
    adapter.call(QStringLiteral("StartDiscovery"));

    QTimer budget;
    budget.setSingleShot(true);
    QObject::connect(&budget, &QTimer::timeout, &collector.loop, &QEventLoop::quit);
    budget.start(kScanBudgetMs);
    collector.loop.exec();
    budget.stop();

    adapter.call(QStringLiteral("StopDiscovery"));

    // Signal strength is read afterwards; the advertisements themselves do not
    // carry it. Headsets rotate their advertising address, so several entries
    // can be the same physical pair — picking the strongest is what keeps a
    // neighbour's identical model from winning when both are in range.
    BluezObjects seen;
    managedObjects(bus, &seen);

    QByteArray best;
    int bestRssi = 0;
    for (auto hit = collector.hits.constBegin(); hit != collector.hits.constEnd(); ++hit) {
        const int rssi = rssiFor(seen, hit.key());
        if (best.isEmpty() || rssi > bestRssi) {
            best     = hit.value();
            bestRssi = rssi;
        }
    }
    if (!best.isEmpty()) {
        quint16 seenModel = 0;
        decodeAdvert(best, &seenModel, &result);
    }
    return result;
}

} // namespace

AirPodsBattery requestAirPodsBattery(const QString &address) {
    registerBluezTypes();

    AirPodsBattery result;
    const QString connName = QStringLiteral("devmgmt-airpods-scan");
    // Scoped so the connection is released before it is torn down; the socket
    // stays open for as long as any handle to it survives.
    {
        QDBusConnection bus =
            QDBusConnection::connectToBus(QDBusConnection::SystemBus, connName);
        result = captureBattery(bus, address);
    }
    QDBusConnection::disconnectFromBus(connName);
    return result;
}

#include "AirPodsScanner.moc"
