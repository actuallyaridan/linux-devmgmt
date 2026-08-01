#pragma once

#include <QString>
#include <QVector>

// A connected Apple headset, as BlueZ already knows it. Listing these costs
// nothing beyond a D-Bus round trip, no radio activity is involved.
struct AirPodsDevice {
    QString name;
    QString address;
    quint16 model = 0;
};

// Battery state decoded from an Apple proximity-pairing advertisement.
//
// A level of -1 means that component is not reporting rather than that it is
// empty: a pod reports only out of the case, the case only with its lid open.
//
// Levels are coarse, broadcast in steps of ten, so a pod Apple's own UI shows
// as 83% comes across as 80%.
struct AirPodsBattery {
    int  left      = -1;
    int  right     = -1;
    int  caseLevel = -1;
    bool leftCharging  = false;
    bool rightCharging = false;
    bool caseCharging  = false;

    bool isEmpty() const { return left < 0 && right < 0 && caseLevel < 0; }
};

// Connected Apple headsets. Safe to call during a normal device scan.
QVector<AirPodsDevice> connectedAirPods();

// Reads the battery levels of one connected Apple headset.
//
// AirPods report no battery through any standard channel: no org.bluez.Battery1
// interface, no power_supply node, nothing in UPower. The levels ride in a
// vendor advertisement they broadcast continuously, the same one MagicPods and
// AirStatus read.
//
// Capturing it requires an active LE scan, since BlueZ discards the advertising
// device objects the moment discovery stops and its cache cannot be read back.
// Scanning shares the radio with audio, so on many adapters playback stutters
// for as long as this runs; hence it is only called when the user asks and
// stops as soon as it has heard enough. Blocks for up to about three seconds.
//
// Returns an empty battery if nothing was captured.
AirPodsBattery requestAirPodsBattery(const QString &address);
