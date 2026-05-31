#include "MainWindow.h"
#include "DeviceModel.h"
#include "DeviceData.h"
#include "DevicePropertiesDialog.h"
#include "UpdateDriverDialog.h"
#include "DeviceScanner.h"
#include "DeviceUtils.h"
#include "DeviceOps.h"
#include "UdevMonitor.h"
#include "IconHelper.h"

#include <QTreeView>
#include <QToolBar>
#include <QMenuBar>
#include <QStatusBar>
#include <QAction>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QHeaderView>
#include <QMenu>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QFont>
#include <QTimer>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Device Manager");
    resize(640, 480);

    m_model = new DeviceModel(this);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_model);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(16);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    setCentralWidget(m_tree);

    connect(m_tree, &QTreeView::doubleClicked,
            this, &MainWindow::showProperties);
    connect(m_tree, &QTreeView::customContextMenuRequested,
            this, &MainWindow::showContextMenu);
    connect(m_tree->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &, const QModelIndex &) {
        onSelectionChanged();
    });

    buildMenus();
    buildToolbar();
    statusBar()->showMessage("Scanning for hardware...");
    startScan();

    m_rescanTimer = new QTimer(this);
    m_rescanTimer->setSingleShot(true);
    m_rescanTimer->setInterval(500);
    connect(m_rescanTimer, &QTimer::timeout, this, &MainWindow::refresh);

    m_udevMonitor = new UdevMonitor(this);
    connect(m_udevMonitor, &UdevMonitor::deviceChanged,
            m_rescanTimer, qOverload<>(&QTimer::start));
}

MainWindow::~MainWindow() {
    if (m_scanner && m_scanner->isRunning()) {
        // Disconnect first so the finished signal doesn't trigger deleteLater
        // or null out m_scanner while we're waiting — parent-child cleanup
        // handles deletion after wait() returns.
        m_scanner->disconnect();
        m_scanner->wait();
    }
}

void MainWindow::startScan() {
    if (m_scanner && m_scanner->isRunning())
        return;
    m_scanner = new DeviceScanner(this);
    connect(m_scanner, &DeviceScanner::scanComplete,
            this, &MainWindow::onScanComplete);
    connect(m_scanner, &QThread::finished, m_scanner, &QObject::deleteLater);
    connect(m_scanner, &QThread::finished, this, [this] { m_scanner = nullptr; });
    m_scanner->start();
}

QSet<QString> MainWindow::saveExpandedState() const {
    QSet<QString> expanded;
    std::function<void(const QModelIndex &, int)> walk =
        [&](const QModelIndex &parent, int depth) {
        for (int i = 0; i < m_model->rowCount(parent); ++i) {
            QModelIndex idx = m_model->index(i, 0, parent);
            if (m_tree->isExpanded(idx)) {
                expanded.insert(
                    QString::number(depth) + ':'
                    + m_model->data(idx, Qt::DisplayRole).toString());
                walk(idx, depth + 1);
            }
        }
    };
    walk(m_tree->rootIndex(), 0);
    return expanded;
}

void MainWindow::restoreExpandedState(const QSet<QString> &expanded) {
    std::function<void(const QModelIndex &, int)> walk =
        [&](const QModelIndex &parent, int depth) {
        for (int i = 0; i < m_model->rowCount(parent); ++i) {
            QModelIndex idx = m_model->index(i, 0, parent);
            if (expanded.contains(
                    QString::number(depth) + ':'
                    + m_model->data(idx, Qt::DisplayRole).toString())) {
                m_tree->expand(idx);
                walk(idx, depth + 1);
            }
        }
    };
    walk(m_tree->rootIndex(), 0);
}

void MainWindow::onScanComplete(const QString &hostName,
                                const QVector<DeviceCategory> &categories) {
    QSet<QString> expanded = saveExpandedState();
    m_model->setCategories(hostName, categories);

    if (expanded.isEmpty()) {
        m_tree->expandToDepth(0);
    } else {
        restoreExpandedState(expanded);
    }

    statusBar()->showMessage("Ready");
    updateActionStates();

    if (!m_pendingReopenName.isEmpty()) {
        QModelIndex idx = findDevice(m_pendingReopenName, m_pendingReopenDriver);
        m_pendingReopenName.clear();
        m_pendingReopenDriver.clear();
        if (idx.isValid()) {
            m_tree->setCurrentIndex(idx);
            showProperties();
        }
    }
}

void MainWindow::onSelectionChanged() {
    updateActionStates();
}

void MainWindow::updateActionStates() {
    auto idx = m_tree->currentIndex();
    bool isDevice = idx.isValid() &&
                    !m_model->deviceField(idx, "name").toString().isEmpty();
    bool disabled = isDevice &&
                    m_model->deviceField(idx, "disabled").toBool();
    QString driver = isDevice
        ? m_model->deviceField(idx, "driver").toString()
        : QString{};
    bool isDkms = isDevice && m_model->deviceField(idx, "isDkms").toBool();

    QString location = isDevice
        ? m_model->deviceField(idx, "location").toString()
        : QString{};
    bool isBtAudio = (location == "connected via Bluetooth");

    bool canDisable = !isBtAudio && isDevice
        && !driver.isEmpty()
        && driver != "(kernel)"
        && isSafeToDisable(driver);

    m_actProperties->setEnabled(isDevice);
    m_actUpdateDriver->setEnabled(isDevice);
    m_actDisable->setEnabled(canDisable || disabled || isBtAudio);
    if (isBtAudio)
        m_actDisable->setText("Disconnect");
    else
        m_actDisable->setText(disabled ? "Enable Device" : "Disable Device");
    m_actUninstall->setEnabled(isDkms);
}

void MainWindow::disableCurrentDevice() {
    auto idx = m_tree->currentIndex();
    if (!idx.isValid()) return;
    QString name = m_model->deviceField(idx, "name").toString();
    if (name.isEmpty()) return;

    QString location = m_model->deviceField(idx, "location").toString();
    QString btAddress = m_model->deviceField(idx, "btAddress").toString();
    if (btAddress.isEmpty())
        btAddress = m_model->deviceField(idx, "rawLocation").toString();
    if (location == "connected via Bluetooth") {
        static const QRegularExpression kMacRe(
            "^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$");
        if (!kMacRe.match(btAddress).hasMatch()) {
            QMessageBox::warning(this, "Disconnect failed",
                "Invalid device address.");
            return;
        }
        if (QMessageBox::question(this, "Disconnect device",
                QString("This will disconnect %1.\n\nContinue?")
                    .arg(name.toHtmlEscaped()))
            != QMessageBox::Yes)
            return;

        QProcess proc;
        proc.start("bluetoothctl", {"disconnect", btAddress});
        bool ok = proc.waitForFinished(5000);
        QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        if (!ok || (!out.contains("Successful", Qt::CaseInsensitive)
                    && proc.exitCode() != 0)) {
            QString err = QString::fromUtf8(
                proc.readAllStandardError()).trimmed();
            QMessageBox::warning(this, "Disconnect failed",
                QString("Could not disconnect %1:\n\n%2")
                    .arg(name.toHtmlEscaped(),
                         (err.isEmpty() ? out : err).toHtmlEscaped()));
            return;
        }
        refresh();
        return;
    }

    QString driver = m_model->deviceField(idx, "driver").toString();
    if (driver.isEmpty()) return;

    bool disabled = m_model->deviceField(idx, "disabled").toBool();
    QString action = disabled ? "enable" : "disable";

    if (QMessageBox::question(this,
            QString("%1 device").arg(disabled ? "Enable" : "Disable"),
            QString("This will %1 %2.\n\nContinue?").arg(action, name.toHtmlEscaped()))
        != QMessageBox::Yes)
        return;

    if (!setModuleBlacklisted(driver, !disabled, this))
        return;

    QMessageBox::information(this, "Done",
        disabled ? "Device enabled." : "Device disabled.");
    refresh();
}

void MainWindow::uninstallCurrentDevice() {
    auto idx = m_tree->currentIndex();
    if (!idx.isValid()) return;
    QString name = m_model->deviceField(idx, "name").toString();
    QString driver = m_model->deviceField(idx, "driver").toString();
    if (name.isEmpty()) return;

    if (QMessageBox::question(this, "Uninstall device",
            QString("This will remove the driver for %1.\n\nAre you sure?")
                .arg(name.toHtmlEscaped()))
        != QMessageBox::Yes)
        return;

    if (dkmsRemove(driver, this))
        refresh();
}

void MainWindow::buildMenus() {
    auto *fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("E&xit", this, &QWidget::close);

    auto *actionMenu = menuBar()->addMenu("&Action");
    m_actUpdateDriver = actionMenu->addAction(
        "Update Driver Software...", [this] {
            auto idx = m_tree->currentIndex();
            if (!idx.isValid()) return;
            QString name = m_model->deviceField(idx, "name").toString();
            UpdateDriverDialog dlg(name, this);
            dlg.exec();
        });
    m_actDisable = actionMenu->addAction("Disable Device",
        this, &MainWindow::disableCurrentDevice);
    m_actUninstall = actionMenu->addAction("Uninstall Device",
        this, &MainWindow::uninstallCurrentDevice);
    actionMenu->addSeparator();
    m_actScan = actionMenu->addAction("&Scan for hardware changes",
                                      this, &MainWindow::refresh);
    actionMenu->addSeparator();
    m_actProperties = actionMenu->addAction("&Properties",
                                            this, &MainWindow::showProperties);

    auto *viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction("Devices by &type",
                        [this]{ m_tree->expandToDepth(0); });
    viewMenu->addAction("Devices by &connection");
    viewMenu->addSeparator();
    viewMenu->addAction("&Show hidden devices")->setCheckable(true);

    auto *helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&Help Topics");
    helpMenu->addSeparator();
    helpMenu->addAction("&About", [this]{
        QDialog dlg(this);
        dlg.setWindowTitle("About Device Manager");
        dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
        dlg.setFixedWidth(340);

        auto *iconLabel = new QLabel;
        iconLabel->setFixedSize(64, 64);
        iconLabel->setPixmap(windowIcon().pixmap(64, 64));
        iconLabel->setAlignment(Qt::AlignCenter);

        auto *nameLabel = new QLabel("Device Manager");

        auto *companyLabel = new QLabel("@actuallyaridan");
        auto *versionLabel = new QLabel("Version: 2.0");

        auto *infoLayout = new QVBoxLayout;
        infoLayout->addWidget(nameLabel);
        infoLayout->addWidget(companyLabel);
        infoLayout->addWidget(versionLabel);
        infoLayout->addStretch();

        auto *topLayout = new QHBoxLayout;
        topLayout->addWidget(iconLabel);
        topLayout->addSpacing(8);
        topLayout->addLayout(infoLayout);
        topLayout->addStretch();

        auto *sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Sunken);

        auto *descLabel = new QLabel(
            "You can use Device Manager to view a list of hardware devices "
            "installed on your computer and set properties for each device.");
        descLabel->setWordWrap(true);
        descLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        descLabel->setContentsMargins(4, 4, 4, 4);

        auto *creditsLabel = new QLabel(
            "Replicated in Linux with Qt6 and real hardware backends via sysfs/procfs. Best enjoyed with AeroThemePlasma. Any Microsoft branding is used solely for referential use only, and does not aim to usurp copyrights from Microsoft.");
        creditsLabel->setWordWrap(true);
        creditsLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        creditsLabel->setContentsMargins(4, 4, 4, 4);

        auto *okBtn = new QPushButton("OK");
        okBtn->setFixedWidth(80);
        okBtn->setDefault(true);
        QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

        auto *btnLayout = new QHBoxLayout;
        btnLayout->addStretch();
        btnLayout->addWidget(okBtn);

        auto *mainLayout = new QVBoxLayout(&dlg);
        mainLayout->setContentsMargins(12, 12, 12, 12);
        mainLayout->setSpacing(8);
        mainLayout->addLayout(topLayout);
        mainLayout->addWidget(sep);
        mainLayout->addWidget(descLabel);
        mainLayout->addWidget(creditsLabel);
        mainLayout->addSpacing(4);
        mainLayout->addLayout(btnLayout);

        dlg.layout()->setSizeConstraint(QLayout::SetFixedSize);
        dlg.exec();
    });

    m_actProperties->setEnabled(false);
    m_actUpdateDriver->setEnabled(false);
    m_actDisable->setEnabled(false);
    m_actUninstall->setEnabled(false);
}

void MainWindow::buildToolbar() {
    auto *tb = addToolBar("Main");
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setIconSize(QSize(16, 16));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);

    tb->addAction(themeIcon({"go-previous", "arrow-left"}), "Back");
    tb->addAction(themeIcon({"go-next", "arrow-right"}), "Forward");
    tb->addSeparator();
    tb->addAction(themeIcon({"view-list-tree", "format-justify-fill",
                             "format-list-unordered"}),
                  "Show/hide console tree");
    m_actProperties->setIcon(
        themeIcon({"document-properties", "configure"}));
    tb->addAction(m_actProperties);
    tb->addAction(themeIcon({"help-contents", "help-browser"}), "Help");
    tb->addAction(themeIcon({"view-refresh", "reload"}),
                  "Scan for hardware changes", this, &MainWindow::refresh);
    tb->addSeparator();
    m_actUpdateDriver->setIcon(
        themeIcon({"system-software-update", "software-update-available"}));
    tb->addAction(m_actUpdateDriver);
    m_actUninstall->setIcon(themeIcon({"edit-delete", "list-remove"}));
    tb->addAction(m_actUninstall);
    tb->addAction(themeIcon({"list-add", "add"}), "Add legacy hardware");
}

void MainWindow::showContextMenu(const QPoint &pos) {
    auto idx = m_tree->indexAt(pos);
    if (!idx.isValid())
        return;

    QString name = m_model->deviceField(idx, "name").toString();
    if (name.isEmpty())
        return;

    m_tree->setCurrentIndex(idx);

    QMenu menu(this);

    menu.addAction("Update driver software...", [this, idx] {
        QString n = m_model->deviceField(idx, "name").toString();
        UpdateDriverDialog dlg(n, this);
        dlg.exec();
    });

    auto *disableAct = menu.addAction(
        m_actDisable->text(),
        this, &MainWindow::disableCurrentDevice);
    disableAct->setEnabled(m_actDisable->isEnabled());

    auto *uninstallAct = menu.addAction("Uninstall device",
        this, &MainWindow::uninstallCurrentDevice);
    uninstallAct->setEnabled(m_actUninstall->isEnabled());

    menu.addSeparator();

    menu.addAction("Scan for hardware changes",
                   this, &MainWindow::refresh);

    menu.addSeparator();

    menu.addAction("Properties", this, &MainWindow::showProperties);

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

QModelIndex MainWindow::findDevice(const QString &name,
                                   const QString &driver) const {
    std::function<QModelIndex(const QModelIndex &)> walk =
        [&](const QModelIndex &parent) -> QModelIndex {
        for (int i = 0; i < m_model->rowCount(parent); ++i) {
            QModelIndex idx = m_model->index(i, 0, parent);
            if (m_model->deviceField(idx, "name").toString() == name
                    && m_model->deviceField(idx, "driver").toString() == driver)
                return idx;
            QModelIndex found = walk(idx);
            if (found.isValid())
                return found;
        }
        return {};
    };
    return walk(m_tree->rootIndex());
}

void MainWindow::showProperties() {
    auto idx = m_tree->currentIndex();
    if (!idx.isValid())
        return;

    QString name = m_model->deviceField(idx, "name").toString();
    if (name.isEmpty()) {
        if (m_tree->isExpanded(idx))
            m_tree->collapse(idx);
        else
            m_tree->expand(idx);
        return;
    }

    DeviceInfo info{
        name,
        m_model->deviceField(idx, "manufacturer").toString(),
        m_model->deviceField(idx, "status").toString(),
        m_model->deviceField(idx, "driver").toString(),
        m_model->deviceField(idx, "driverVersion").toString(),
        m_model->deviceField(idx, "driverDate").toString(),
        m_model->deviceField(idx, "driverAuthor").toString(),
        m_model->deviceField(idx, "location").toString(),
        m_model->deviceField(idx, "iconName").toString(),
        m_model->deviceField(idx, "disabled").toBool(),
        m_model->deviceField(idx, "isDkms").toBool(),
        m_model->deviceField(idx, "rawLocation").toString(),
        m_model->deviceField(idx, "sysfsPciPath").toString(),
        m_model->deviceField(idx, "deviceType").toString(),
        m_model->deviceField(idx, "btAddress").toString(),
        m_model->deviceField(idx, "noDriverNeeded").toBool()
    };
    DevicePropertiesDialog dlg(info, this);
    connect(&dlg, &DevicePropertiesDialog::disableToggled,
            this, &MainWindow::refresh);
    auto scheduleReopen = [this, info] {
        m_pendingReopenName   = info.name;
        m_pendingReopenDriver = info.driver;
        refresh();
    };
    connect(&dlg, &DevicePropertiesDialog::usbmuxdStarted,  this, scheduleReopen);
    connect(&dlg, &DevicePropertiesDialog::refreshRequested, this, scheduleReopen);
    dlg.exec();
}

void MainWindow::refresh() {
    statusBar()->showMessage("Scanning for hardware...");
    startScan();
}