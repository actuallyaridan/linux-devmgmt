#pragma once

#include <QString>
#include <QVector>

struct Device {
    QString name;
    QString status;
    QString manufacturer;
    QString driver;
    QString driverVersion;
    QString driverDate;
    QString driverAuthor;
    QString location;
    QString iconName;
    bool disabled = false;
    bool isDkms = false;
    QString rawLocation;
    QString sysfsPciPath;
    QString btAddress;
    bool noDriverNeeded = false;
    // Battery readable only by an LE scan the user has to ask for; see
    // AirPodsScanner.h for why it cannot simply be read during a device scan.
    bool airpodsBattery = false;
};

struct DeviceCategory {
    QString name;
    QString iconName;
    QVector<Device> devices;
};

