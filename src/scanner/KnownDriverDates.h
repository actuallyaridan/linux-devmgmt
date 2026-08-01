#pragma once
#include <QHash>
#include <QString>

// Release dates for legacy drivers whose upstream date cannot be determined
// from the local system. Maps the exact version string (as `modinfo -F version`
// reports it) to the ISO 8601 date the vendor published it.
//
// Sources:
//   NVIDIA legacy:  https://www.nvidia.com/en-us/drivers/unix/legacy-gpu/
//   Broadcom STA:   https://docs.broadcom.com/doc/802-11-linux-sta-wireless-driver-release-notes
inline const QHash<QString, QString> &knownDriverDates() {
    static const QHash<QString, QString> table{
        // Broadcom STA
        {"6.30.223.271", "2015-09-18"},

        // NVIDIA legacy branches (final release per branch)
        {"470.256.02",   "2024-06-04"},  // 470.xx, last branch still maintained
        {"390.157",      "2022-11-22"},  // 390.xx
        {"340.108",      "2019-12-23"},  // 340.xx
        {"304.137",      "2017-09-19"},  // 304.xx
        {"173.14.39",    "2017-05-22"},  // 173.xx
        {"96.43.23",     "2016-05-31"},  // 96.xx
        {"71.86.15",     "2014-01-07"},  // 71.xx
    };
    return table;
}
