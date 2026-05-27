#include "DriverDetailsDialog.h"
#include "DeviceUtils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTreeWidget>
#include <QDialogButtonBox>
#include <QProcess>
#include <QHeaderView>

DriverDetailsDialog::DriverDetailsDialog(const QString &moduleName,
                                         QWidget *parent)
    : QDialog(parent) {
    setWindowTitle("Driver File Details");
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    resize(500, 400);

    auto *v = new QVBoxLayout(this);

    v->addWidget(new QLabel(
        "The following driver files are installed for this device:"));

    auto *tree = new QTreeWidget;
    tree->setHeaderLabels({"Field", "Value"});
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);

    auto addRow = [&](const QString &field, const QString &value) {
        new QTreeWidgetItem(tree, {field, value});
    };

    if (!moduleName.isEmpty() && moduleName != "(kernel)") {
        if (!isValidModuleName(moduleName)) {
            addRow("module", "(invalid module name)");
        } else {
            QProcess proc;
            proc.start("modinfo", {"--", moduleName});
            proc.waitForFinished(3000);
            QString output = QString::fromUtf8(proc.readAllStandardOutput());
            for (const QString &line : output.split('\n')) {
                int colon = line.indexOf(':');
                if (colon < 0) continue;
                QString field = line.left(colon).trimmed();
                QString value = line.mid(colon + 1).trimmed();
                if (!field.isEmpty())
                    addRow(field, value);
            }
        }
    } else {
        addRow("module", "(built into kernel)");
        addRow("version", QSysInfo::kernelVersion());
    }

    v->addWidget(tree, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    v->addWidget(buttons);
}