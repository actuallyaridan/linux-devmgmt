#include "MainWindow.h"
#include "DeviceModel.h"
#include "DeviceData.h"
#include "DevicePropertiesDialog.h"
#include "UpdateDriverDialog.h"
#include "DeviceScanner.h"
#include "DeviceUtils.h"
#include "DeviceOps.h"

#include <QTreeView>
#include <QToolBar>
#include <QMenuBar>
#include <QStatusBar>
#include <QAction>
#include <QMessageBox>
#include <QHeaderView>
#include <QMenu>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QFont>

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

    bool canDisable = isDevice
        && !driver.isEmpty()
        && driver != "(kernel)"
        && isSafeToDisable(driver);

    m_actProperties->setEnabled(isDevice);
    m_actUpdateDriver->setEnabled(isDevice);
    m_actDisable->setEnabled(canDisable || disabled);
    m_actDisable->setText(disabled ? "Enable Device" : "Disable Device");
    m_actUninstall->setEnabled(isDkms);
}

void MainWindow::disableCurrentDevice() {
    auto idx = m_tree->currentIndex();
    if (!idx.isValid()) return;
    QString name = m_model->deviceField(idx, "name").toString();
    QString driver = m_model->deviceField(idx, "driver").toString();
    if (name.isEmpty() || driver.isEmpty()) return;

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
        auto *versionLabel = new QLabel("Version: 1.0");

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
            "Replicated in Linux using Qt and KDE. Best enjoyed with AeroThemePlasma. Any Microsoft branding is used solely for referential use only, and does not aim to usurp copyrights from Microsoft.");
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

    tb->addAction(QIcon::fromTheme("go-previous"), "Back");
    tb->addAction(QIcon::fromTheme("go-next"), "Forward");
    tb->addSeparator();
    tb->addAction(QIcon::fromTheme("view-list-tree"),
                  "Show/hide console tree");
    m_actProperties->setIcon(QIcon::fromTheme("document-properties"));
    tb->addAction(m_actProperties);
    tb->addAction(QIcon::fromTheme("help-contents"), "Help");
    tb->addAction(QIcon::fromTheme("view-refresh"),
                  "Scan for hardware changes", this, &MainWindow::refresh);
    tb->addSeparator();
    m_actUpdateDriver->setIcon(
        QIcon::fromTheme("system-software-update"));
    tb->addAction(m_actUpdateDriver);
    m_actUninstall->setIcon(QIcon::fromTheme("edit-delete"));
    tb->addAction(m_actUninstall);
    tb->addAction(QIcon::fromTheme("list-add"), "Add legacy hardware");
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
        m_model->deviceField(idx, "deviceType").toString()
    };
    DevicePropertiesDialog dlg(info, this);
    connect(&dlg, &DevicePropertiesDialog::disableToggled,
            this, &MainWindow::refresh);
    dlg.exec();
}

void MainWindow::refresh() {
    statusBar()->showMessage("Scanning for hardware...");
    startScan();
}