#include "DeviceModel.h"
#include "DeviceUtils.h"
#include "DeviceOps.h"
#include "../ui/IconHelper.h"

#include <QFile>
#include <QTextStream>
#include <QIcon>
#include <QDate>

DeviceModel::DeviceModel(QObject *parent)
    : QAbstractItemModel(parent),
      m_root(new Node{RootNode, {}, {}, nullptr, {}, nullptr}) {
    loadDisabledModules();
}

DeviceModel::~DeviceModel() {
    delete m_root;
}

void DeviceModel::loadDisabledModules() {
    m_disabledModules.clear();
    QFile f(kModprobeConf);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.startsWith("blacklist "))
            m_disabledModules.insert(line.mid(10).trimmed());
    }
}

void DeviceModel::setCategories(const QString &hostName,
                                const QVector<DeviceCategory> &categories) {
    beginResetModel();
    delete m_root;
    m_root = new Node{RootNode, {}, {}, nullptr, {}, nullptr};
    m_storage = categories;
    loadDisabledModules();

    auto *host = new Node{ComputerNode, hostName, "preferences-other",
                          m_root, {}, nullptr};
    m_root->children.append(host);

    QString productName = readSysFile("/sys/class/dmi/id/product_name");
    QString biosVendor  = readSysFile("/sys/class/dmi/id/bios_vendor");
    QString biosVersion = readSysFile("/sys/class/dmi/id/bios_version");
    QString biosDate    = readSysFile("/sys/class/dmi/id/bios_date");

    if (productName.isEmpty()) {
#if defined(__aarch64__)
        productName = QStringLiteral("ACPI arm64-based PC");
#else
        productName = QStringLiteral("ACPI x64-based PC");
#endif
    }
    if (biosVendor.isEmpty())
        biosVendor = QStringLiteral("Unknown");

    if (!biosDate.isEmpty()) {
        QDate d = QDate::fromString(biosDate, "MM/dd/yyyy");
        if (d.isValid())
            biosDate = formatDate(d);
    }

    m_machine.name         = productName;
    m_machine.status       = QStringLiteral("Working properly");
    m_machine.manufacturer = biosVendor;
    m_machine.driver       = QStringLiteral("acpi");
    m_machine.driverVersion = biosVersion;
    m_machine.driverDate   = biosDate;
    m_machine.iconName     = QStringLiteral("computer");

    QVector<const Device *> disabledDevices;

    for (int i = 0; i < m_storage.size(); ++i) {
        const auto &cat = m_storage[i];

        if (cat.name == "__disabled__") {
            for (int j = 0; j < m_storage[i].devices.size(); ++j) {
                m_storage[i].devices[j].disabled = true;
                disabledDevices.append(&m_storage[i].devices[j]);
            }
            continue;
        }

        auto *catNode = new Node{CategoryNode, cat.name, cat.iconName,
                                 host, {}, nullptr};
        host->children.append(catNode);

        for (int j = 0; j < cat.devices.size(); ++j) {
            QString icon = cat.devices[j].iconName.isEmpty()
                ? cat.iconName : cat.devices[j].iconName;
            m_storage[i].devices[j].iconName = icon;

            bool disabled = m_disabledModules.contains(
                m_storage[i].devices[j].driver);
            m_storage[i].devices[j].disabled = disabled;

            if (disabled) {
                disabledDevices.append(&m_storage[i].devices[j]);
                continue;
            }

            auto *devNode = new Node{DeviceNode,
                                     cat.devices[j].name,
                                     icon,
                                     catNode, {},
                                     &m_storage[i].devices[j]};
            catNode->children.append(devNode);
        }

        if (catNode->children.isEmpty()) {
            host->children.removeLast();
            delete catNode;
        }
    }

    auto *computerCat = new Node{CategoryNode, "Computer", "computer",
                                 host, {}, nullptr};
    int insertPos = 0;
    while (insertPos < host->children.size() &&
           host->children[insertPos]->label.compare("Computer", Qt::CaseInsensitive) < 0) {
        ++insertPos;
    }
    host->children.insert(insertPos, computerCat);
    auto *machineDev = new Node{DeviceNode, m_machine.name,
                                "computer", computerCat,
                                {}, &m_machine};
    computerCat->children.append(machineDev);

    if (!disabledDevices.isEmpty()) {
        auto *disabledCat = new Node{DisabledCategoryNode,
                                     "Disabled devices",
                                     "dialog-cancel",
                                     host, {}, nullptr};
        host->children.append(disabledCat);
        for (const Device *dev : disabledDevices) {
            auto *devNode = new Node{DeviceNode, dev->name,
                                     "computer",
                                     disabledCat, {}, dev};
            disabledCat->children.append(devNode);
        }
    }

    endResetModel();
}

DeviceModel::Node *DeviceModel::nodeFor(const QModelIndex &index) const {
    if (!index.isValid())
        return m_root;
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex DeviceModel::index(int row, int column,
                               const QModelIndex &parent) const {
    if (!hasIndex(row, column, parent))
        return {};
    Node *p = nodeFor(parent);
    if (row < 0 || row >= p->children.size())
        return {};
    return createIndex(row, column, p->children[row]);
}

QModelIndex DeviceModel::parent(const QModelIndex &child) const {
    if (!child.isValid())
        return {};
    Node *c = nodeFor(child);
    Node *p = c->parent;
    if (!p || p == m_root)
        return {};
    Node *gp = p->parent;
    int row = gp ? gp->children.indexOf(p) : 0;
    return createIndex(row, 0, p);
}

int DeviceModel::rowCount(const QModelIndex &parent) const {
    return nodeFor(parent)->children.size();
}

int DeviceModel::columnCount(const QModelIndex &) const {
    return 1;
}

QVariant DeviceModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};
    Node *n = nodeFor(index);
    if (role == Qt::DisplayRole)
        return n->label;
    if (role == Qt::DecorationRole)
        return resolveIcon(n->iconName);
    return {};
}

QVariant DeviceModel::headerData(int, Qt::Orientation, int) const {
    return {};
}

QVariant DeviceModel::deviceField(const QModelIndex &index,
                                  const QString &field) const {
    if (!index.isValid())
        return {};
    Node *n = nodeFor(index);
    if (n->type != DeviceNode || !n->device)
        return {};
    const auto &d = *n->device;
    if (field == "name") return d.name;
    if (field == "status") return d.status;
    if (field == "manufacturer") return d.manufacturer;
    if (field == "driver") return d.driver;
    if (field == "driverVersion") return d.driverVersion;
    if (field == "driverDate") return d.driverDate;
    if (field == "driverAuthor") return d.driverAuthor;
    if (field == "location") return d.location;
    if (field == "iconName") return d.disabled ? QStringLiteral("computer") : d.iconName;
    if (field == "disabled") return d.disabled;
    if (field == "isDkms") return d.isDkms;
    if (field == "rawLocation") return d.rawLocation;
    if (field == "sysfsPciPath") return d.sysfsPciPath;
    if (field == "btAddress") return d.btAddress;
    if (field == "noDriverNeeded") return d.noDriverNeeded;
    if (field == "deviceType") {
        if (n->parent && (n->parent->type == CategoryNode
                          || n->parent->type == DisabledCategoryNode))
            return n->parent->label;
        return QStringLiteral("Hardware device");
    }
    return {};
}