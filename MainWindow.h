#pragma once

#include <QMainWindow>
#include <QSet>
#include "DeviceData.h"

class QTreeView;
class DeviceModel;
class DeviceScanner;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void showProperties();
    void refresh();
    void showContextMenu(const QPoint &pos);
    void onScanComplete(const QString &hostName,
                        const QVector<DeviceCategory> &categories);
    void onSelectionChanged();
    void disableCurrentDevice();
    void uninstallCurrentDevice();

private:
    QTreeView *m_tree;
    DeviceModel *m_model;
    DeviceScanner *m_scanner = nullptr;

    QAction *m_actProperties;
    QAction *m_actUpdateDriver;
    QAction *m_actDisable;
    QAction *m_actUninstall;
    QAction *m_actScan;

    void buildMenus();
    void buildToolbar();
    void startScan();
    void updateActionStates();
    QSet<QString> saveExpandedState() const;
    void restoreExpandedState(const QSet<QString> &expanded);
};