#pragma once

#include <QString>
#include <QVector>

// A connected Apple headset, as BlueZ already knows it. Listing these costs
// nothing beyond a D-Bus round trip — no radio activity is involved.
struct AirPodsDevice {
    QString name;
    QString address;
    quint16 model = 0;
};

// Battery state decoded from an Apple proximity-pairing advertisement.
//
// A level of -1 means that component is not reporting right now rather than
// that it is empty: each pod reports only while out of the case, and the case
// reports only while its lid is open.
//
// Levels are coarse. The advertisement carries one value per component in
// steps of ten, so a pod Apple's own UI shows as 83% is broadcast as 80%.
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
// AirPods report no battery through any standard channel: they expose no
// org.bluez.Battery1 interface, create no power_supply node, and never appear
// in UPower, so none of the other battery scanners can see them. The levels
// ride in a vendor advertisement they broadcast continuously, which is what
// MagicPods on Windows and AirStatus on Linux read, and what this decodes.
//
// Capturing that advertisement requires an active LE scan, because BlueZ
// discards the advertising device objects the moment discovery stops and the
// data cannot be read back from its cache afterwards. Scanning shares the radio
// with audio, so on many adapters playback stutters or drops out for as long as
// this runs — which is why it is only ever called when the user asks for it,
// and why it stops as soon as it has heard enough. Blocks for up to about three
// seconds, usually far less.
//
// Returns an empty battery if nothing was captured.
AirPodsBattery requestAirPodsBattery(const QString &address);
