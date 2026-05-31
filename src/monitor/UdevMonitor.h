#pragma once

#include <QObject>

class QSocketNotifier;

class UdevMonitor : public QObject {
    Q_OBJECT
public:
    explicit UdevMonitor(QObject *parent = nullptr);
    ~UdevMonitor() override;

    bool isActive() const { return m_sock >= 0; }

signals:
    void deviceChanged();

private slots:
    void onSocketReady();

private:
    int m_sock = -1;
    QSocketNotifier *m_notifier = nullptr;
};
