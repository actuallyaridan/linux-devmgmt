#include "DevicePropertiesDialog.h"
#include "DriverDetailsDialog.h"
#include "UpdateDriverDialog.h"
#include "DeviceUtils.h"
#include "DeviceOps.h"

#include <QShowEvent>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QComboBox>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QGroupBox>
#include <QTextEdit>
#include <QMessageBox>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

DevicePropertiesDialog::DevicePropertiesDialog(const DeviceInfo &info,
                                               QWidget *parent)
    : QDialog(parent), m_info(info) {
    setWindowTitle(info.name + " Properties");
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setFixedWidth(420);
    resize(420, 450);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildGeneralTab(), "General");
    tabs->addTab(buildDriverTab(), "Driver");
    tabs->addTab(buildDetailsTab(), "Details");
    tabs->addTab(buildResourcesTab(), "Resources");
    layout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void DevicePropertiesDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    if (!m_sizeAdjusted) {
        m_sizeAdjusted = true;
        adjustSize();
        setFixedSize(size());
    }
}

bool DevicePropertiesDialog::isKernelModule() const {
    return m_info.driver.isEmpty() || m_info.driver == "(kernel)";
}

static QLabel *plainLabel(const QString &text) {
    auto *l = new QLabel;
    l->setTextFormat(Qt::PlainText);
    l->setText(text);
    return l;
}

static QWidget *deviceHeader(const QString &name, const QString &iconName) {
    auto *row = new QWidget;
    auto *hl = new QHBoxLayout(row);
    hl->setContentsMargins(0, 0, 0, 8);
    auto *icon = new QLabel;
    icon->setPixmap(QIcon::fromTheme(
        iconName.isEmpty() ? "preferences-system" : iconName).pixmap(32, 32));
    icon->setFixedSize(40, 40);
    icon->setAlignment(Qt::AlignTop);
    auto *label = plainLabel(name);
    label->setAlignment(Qt::AlignVCenter);
    label->setWordWrap(true);
    hl->addWidget(icon);
    hl->addWidget(label, 1);
    return row;
}

static QFrame *hline() {
    auto *line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

static QLabel *formKey(const QString &text, int minWidth) {
    auto *lbl = new QLabel(text);
    lbl->setMinimumWidth(minWidth);
    return lbl;
}

static int propertyLabelWidth(const QWidget *ref) {
    QFontMetrics fm(ref->font());
    // "Driver Provider:" is the widest label across General and Driver tabs.
    return fm.horizontalAdvance("Driver Provider:") + 4;
}

QWidget *DevicePropertiesDialog::buildGeneralTab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(0);

    v->addWidget(deviceHeader(m_info.name, m_info.iconName));
    v->addSpacing(8);

    const int lw = propertyLabelWidth(w);

    auto *form = new QGridLayout;
    form->setContentsMargins(48, 0, 0, 0);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(6);
    form->setColumnMinimumWidth(0, lw);
    form->setColumnStretch(1, 1);
    int fRow = 0;
    auto makeWrappingKey = [&](const QString &text) {
        auto *k = formKey(text, lw);
        k->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        return k;
    };
    auto makeWrappingValue = [](const QString &text) {
        auto *l = plainLabel(text);
        l->setWordWrap(true);
        return l;
    };
    auto addRow = [&](QLabel *key, QLabel *val) {
        form->addWidget(key, fRow, 0, Qt::AlignTop | Qt::AlignLeft);
        form->addWidget(val, fRow, 1);
        ++fRow;
    };

    addRow(makeWrappingKey("Device type:"), makeWrappingValue(
        m_info.deviceType.isEmpty() ? "Hardware device" : m_info.deviceType));
    addRow(makeWrappingKey("Manufacturer:"), makeWrappingValue(m_info.manufacturer));
    auto *locationLabel = makeWrappingValue(
        m_info.location.isEmpty() ? "Unknown" : m_info.location);
    if (!m_info.rawLocation.isEmpty()
            && m_info.rawLocation != m_info.location)
        locationLabel->setToolTip(m_info.rawLocation);
    addRow(makeWrappingKey("Location:"), locationLabel);
    v->addLayout(form);

    v->addSpacing(18);

    QString statusText;
    if (m_info.disabled)
        statusText = "This device is disabled.";
    else if (m_info.status == "Working properly")
        statusText = "This device is working properly.";
    else if (m_info.status == "No driver loaded")
        statusText = "The drivers for this device are not installed. "
                     "(Code 28)";
    else
        statusText = m_info.status + ".";

    auto *statusGroup = new QGroupBox("Device status");
    auto *gbLayout = new QVBoxLayout(statusGroup);
    gbLayout->setContentsMargins(8, 3, 8, 32);

    auto *textFrame = new QFrame;
    textFrame->setStyleSheet(
        "QFrame { border: 1px solid palette(midlight); background-color: palette(base); }");
    textFrame->setFixedHeight(120);
    auto *frameLayout = new QHBoxLayout(textFrame);
    frameLayout->setContentsMargins(1, 1, 1, 1);
    frameLayout->setSpacing(0);

    auto *statusBox = new QTextEdit;
    statusBox->setReadOnly(true);
    statusBox->setPlainText(statusText);
    statusBox->setFrameShape(QFrame::NoFrame);
    statusBox->setStyleSheet("QTextEdit { border: none; background: transparent; }");
    statusBox->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    statusBox->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    frameLayout->addWidget(statusBox);

    gbLayout->addWidget(textFrame);

    v->addWidget(statusGroup);
    v->addStretch();

    return w;
}

QWidget *DevicePropertiesDialog::buildDriverTab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(0);

    v->addWidget(deviceHeader(m_info.name, m_info.iconName));
    v->addSpacing(8);

    const int lw = propertyLabelWidth(w);

    auto *form = new QGridLayout;
    form->setContentsMargins(48, 0, 0, 0);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(6);
    form->setColumnMinimumWidth(0, lw);
    form->setColumnStretch(1, 1);
    int fRow = 0;
    auto makeWrappingKey = [&](const QString &text) {
        auto *k = formKey(text, lw);
        k->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        return k;
    };
    auto makeWrappingValue = [](const QString &text) {
        auto *l = plainLabel(text);
        l->setWordWrap(true);
        return l;
    };
    auto addFormRow = [&](QLabel *key, QLabel *val) {
        form->addWidget(key, fRow, 0, Qt::AlignTop | Qt::AlignLeft);
        form->addWidget(val, fRow, 1);
        ++fRow;
    };

    auto stripAuthorAnnotations = [](QString s) {
        static const QRegularExpression kAnnotation(R"(\s*[<({\[].*?[>)}\]])", QRegularExpression::DotMatchesEverythingOption);
        s.remove(kAnnotation);
        return s.trimmed();
    };
    QString providerName = !m_info.driverAuthor.isEmpty()
                          ? stripAuthorAnnotations(m_info.driverAuthor)
                          : m_info.manufacturer.startsWith("(Standard")
                            ? QStringLiteral("Linux") : m_info.manufacturer;
    addFormRow(makeWrappingKey("Driver Provider:"), makeWrappingValue(providerName));
    addFormRow(makeWrappingKey("Driver Date:"), makeWrappingValue(
        m_info.driverDate.isEmpty() ? "Unknown" : m_info.driverDate));
    addFormRow(makeWrappingKey("Driver Version:"), makeWrappingValue(
        m_info.driverVersion.isEmpty() ? m_info.driver : m_info.driverVersion));
    addFormRow(makeWrappingKey("Digital Signer:"), makeWrappingValue("Linux Hardware Compatibility Publisher"));
    v->addLayout(form);

    v->addSpacing(16);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(18);
    grid->setContentsMargins(0, 4, 0, 0);

    int row = 0;

    auto addRow = [&](const QString &btnText, const QString &description,
                      bool enabled, const QString &tooltip = {}) {
        auto *btn = new QPushButton(btnText);
        btn->setFixedWidth(110);
        btn->setEnabled(enabled);
        if (!tooltip.isEmpty())
            btn->setToolTip(tooltip);
        auto *desc = new QLabel(description);
        desc->setWordWrap(true);
        grid->addWidget(btn, row, 0);
        grid->addWidget(desc, row, 1);
        ++row;
        return btn;
    };

    auto *detailsBtn = addRow("Driver Details",
        "To view details about the driver files.",
        !isKernelModule());

    auto *updateBtn = addRow("Update Driver...",
        "To update the driver software for this device.", true);

    addRow("Roll Back Driver",
        "If the device fails after updating the driver, roll back "
        "to the previously installed driver.",
        false,
        "No previous driver version is stored for this device.");

    QString disableLabel = m_info.disabled ? "Enable" : "Disable";
    QString disableDesc = m_info.disabled
        ? "Enables the selected device."
        : "Disables the selected device.";
    bool canDisable = !isKernelModule() && isSafeToDisable(m_info.driver);
    QString disableTooltip;
    if (isKernelModule())
        disableTooltip = "Built-in kernel devices cannot be disabled.";
    else if (!isSafeToDisable(m_info.driver))
        disableTooltip = "This device is critical to system operation "
                         "and cannot be disabled.";

    m_disableBtn = addRow(disableLabel, disableDesc,
                          canDisable || m_info.disabled,
                          disableTooltip);

    bool canUninstall = !isKernelModule() && m_info.isDkms;
    auto *uninstallBtn = addRow("Uninstall",
        "To uninstall the driver (Advanced).",
        canUninstall,
        canUninstall ? QString{}
                     : "Only out-of-tree (DKMS) drivers can be uninstalled.");

    grid->setColumnStretch(1, 1);
    v->addLayout(grid);
    v->addStretch();

    connect(detailsBtn, &QPushButton::clicked, this, [this] {
        DriverDetailsDialog dlg(m_info.driver, this);
        dlg.exec();
    });

    connect(updateBtn, &QPushButton::clicked, this, [this] {
        UpdateDriverDialog dlg(m_info.name, this);
        dlg.exec();
    });

    connect(m_disableBtn, &QPushButton::clicked, this, [this] {
        bool disabling = !m_info.disabled;
        const QString safeName = m_info.name.toHtmlEscaped();
        QString msg = disabling
            ? QString("This will disable %1. The device will not function "
                      "until it is re-enabled.\n\nContinue?").arg(safeName)
            : QString("This will enable %1.\n\nContinue?").arg(safeName);

        if (QMessageBox::question(this,
                QString("%1 device").arg(disabling ? "Disable" : "Enable"),
                msg) != QMessageBox::Yes)
            return;

        if (!setModuleBlacklisted(m_info.driver, disabling, this))
            return;

        m_info.disabled = disabling;
        m_disableBtn->setText(disabling ? "Enable" : "Disable");
        emit disableToggled(disabling);
        QMessageBox::information(this, "Done",
            disabling
                ? "Device disabled. Changes will persist after reboot."
                : "Device enabled.");
    });

    connect(uninstallBtn, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Uninstall driver",
                QString("This will remove the driver for %1 using DKMS.\n\n"
                        "Are you sure?").arg(m_info.name.toHtmlEscaped()))
            != QMessageBox::Yes)
            return;

        if (dkmsRemove(m_info.driver, this))
            accept();
    });

    return w;
}

QWidget *DevicePropertiesDialog::buildDetailsTab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(0);

    v->addWidget(deviceHeader(m_info.name, m_info.iconName));

    v->addWidget(new QLabel("Property:"));
    v->addSpacing(3);
    auto *combo = new QComboBox;
    combo->addItems({
        "Device description",
        "Manufacturer",
        "Driver",
        "Driver version",
        "Driver date",
        "Location",
        "Raw location",
        "Status",
    });
    v->addWidget(combo);
    v->addSpacing(8);

    v->addWidget(new QLabel("Value:"));
    v->addSpacing(3);
    auto *values = new QListWidget;
    v->addWidget(values, 1);

    auto populate = [this, values, combo]() {
        values->clear();
        switch (combo->currentIndex()) {
        case 0: values->addItem(m_info.name); break;
        case 1: values->addItem(m_info.manufacturer); break;
        case 2: values->addItem(m_info.driver); break;
        case 3: values->addItem(m_info.driverVersion); break;
        case 4: values->addItem(m_info.driverDate.isEmpty()
                    ? "Unknown" : m_info.driverDate); break;
        case 5: values->addItem(m_info.location.isEmpty()
                    ? "Unknown" : m_info.location); break;
        case 6: values->addItem(m_info.rawLocation.isEmpty()
                    ? "Unknown" : m_info.rawLocation); break;
        case 7: values->addItem(m_info.disabled
                    ? "Disabled"
                    : m_info.status); break;
        }
    };

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [populate](int) { populate(); });

    populate();
    return w;
}

QWidget *DevicePropertiesDialog::buildResourcesTab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(0);

    v->addWidget(deviceHeader(m_info.name, m_info.iconName));

    v->addWidget(new QLabel("Resource settings:"));
    v->addSpacing(3);

    auto *tree = new QTreeWidget;
    tree->setColumnCount(2);
    tree->setHeaderLabels({"Resource type", "Setting"});
    tree->setRootIsDecorated(false);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setAlternatingRowColors(false);
    tree->header()->setDefaultAlignment(Qt::AlignLeft);
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree->setFixedHeight(130);
    v->addWidget(tree);

    auto makeIcon = [](const QColor &color) -> QIcon {
        QPixmap pm(14, 14);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setPen(QColor(60, 60, 60));
        p.setBrush(color);
        p.drawRect(1, 1, 11, 11);
        return QIcon(pm);
    };

    auto fmtAddr = [](quint64 val) -> QString {
        if (val > 0xFFFFFFFFULL)
            return QString("%1").arg(val, 16, 16, QChar('0')).toUpper();
        if (val > 0xFFFFULL)
            return QString("%1").arg(val, 8, 16, QChar('0')).toUpper();
        return QString("%1").arg(val, 4, 16, QChar('0')).toUpper();
    };

    const QIcon memIcon = makeIcon(QColor(100, 80, 180));
    const QIcon ioIcon  = makeIcon(QColor(40, 140, 40));
    const QIcon irqIcon = makeIcon(QColor(210, 160, 20));

    auto resourceType = [](quint64 flags) -> QString {
        if (flags == 0) return {};
        if (flags & 0x200) return "I/O Range";
        return "Memory Range";
    };

    if (!m_info.sysfsPciPath.isEmpty()) {
        QFile irqFile(m_info.sysfsPciPath + "/irq");
        if (irqFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString irq = QString::fromUtf8(irqFile.read(1024)).trimmed();
            if (!irq.isEmpty() && irq != "0") {
                auto *item = new QTreeWidgetItem(tree);
                item->setIcon(0, irqIcon);
                item->setText(0, "IRQ");
                item->setText(1, irq);
            }
        }

        QFile f(m_info.sysfsPciPath + "/resource");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            int bar = 0;
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                QStringList parts = line.split(' ', Qt::SkipEmptyParts);
                if (parts.size() >= 3) {
                    bool ok1, ok2, ok3;
                    quint64 start = parts[0].toULongLong(&ok1, 16);
                    quint64 end   = parts[1].toULongLong(&ok2, 16);
                    quint64 flags = parts[2].toULongLong(&ok3, 16);
                    if (ok1 && ok2 && ok3 && start != 0 && end != 0) {
                        QString type = resourceType(flags);
                        if (!type.isEmpty()) {
                            auto *item = new QTreeWidgetItem(tree);
                            bool isIO = (flags & 0x200);
                            item->setIcon(0, isIO ? ioIcon : memIcon);
                            item->setText(0, type);
                            item->setText(1, fmtAddr(start) + " - " + fmtAddr(end));
                        }
                    }
                }
                ++bar;
                if (bar > 12) break;
            }
        }
    }

    if (tree->topLevelItemCount() == 0) {
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, "(not available)");
    }

    v->addSpacing(4);
    auto *settingRow = new QHBoxLayout;
    settingRow->setContentsMargins(0, 0, 0, 0);
    settingRow->setSpacing(8);
    settingRow->addWidget(new QLabel("Setting based on:"));
    auto *settingCombo = new QComboBox;
    settingCombo->setEnabled(false);
    settingRow->addWidget(settingCombo, 1);
    v->addLayout(settingRow);
    v->addSpacing(4);

    auto *cbRow = new QHBoxLayout;
    cbRow->setContentsMargins(0, 0, 0, 0);
    auto *autoCheck = new QCheckBox("Use automatic settings");
    autoCheck->setChecked(true);
    autoCheck->setEnabled(false);
    cbRow->addWidget(autoCheck, 1);
    auto *changeBtn = new QPushButton("Change Setting...");
    changeBtn->setEnabled(false);
    cbRow->addWidget(changeBtn);
    v->addLayout(cbRow);
    v->addSpacing(8);

    v->addWidget(new QLabel("Conflicting device list:"));
    v->addSpacing(3);

    auto *conflictFrame = new QFrame;
    conflictFrame->setStyleSheet(
        "QFrame { border: 1px solid palette(midlight); background-color: palette(base); }");
    conflictFrame->setFixedHeight(84);
    auto *conflictFrameLayout = new QHBoxLayout(conflictFrame);
    conflictFrameLayout->setContentsMargins(1, 1, 1, 1);
    conflictFrameLayout->setSpacing(0);

    auto *conflictList = new QListWidget;
    conflictList->setFrameShape(QFrame::NoFrame);
    conflictList->setStyleSheet("QListWidget { border: none; background: transparent; }");
    conflictList->setFocusPolicy(Qt::NoFocus);
    conflictList->setSelectionMode(QAbstractItemView::NoSelection);
    conflictList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    conflictList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conflictList->addItem("No conflicts.");
    conflictFrameLayout->addWidget(conflictList);

    v->addWidget(conflictFrame);
    v->addStretch();

    return w;
}