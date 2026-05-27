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
};

struct DeviceCategory {
    QString name;
    QString iconName;
    QVector<Device> devices;
};

