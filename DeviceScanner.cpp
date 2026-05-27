#include "DeviceScanner.h"
#include "SysfsScanner.h"
#include "DeviceData.h"
#include <QSysInfo>

DeviceScanner::DeviceScanner(QObject *parent)
    : QThread(parent) {}

void DeviceScanner::run() {
    QString host = QSysInfo::machineHostName().toUpper();
    if (host.isEmpty())
        host = QStringLiteral("LOCALHOST");
    QVector<DeviceCategory> cats = scanDevices();
    emit scanComplete(host, cats);
}