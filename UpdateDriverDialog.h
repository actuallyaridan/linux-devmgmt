#pragma once

#include <QDialog>

class UpdateDriverDialog : public QDialog {
    Q_OBJECT
public:
    explicit UpdateDriverDialog(const QString &deviceName,
                                QWidget *parent = nullptr);
};