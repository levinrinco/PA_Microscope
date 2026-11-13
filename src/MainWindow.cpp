#include "MainWindow.h"
#include "SerialDebugDialog.h"
#include "ui_MainWindow.h"
#include "AlineWidget.h"
#include "BscanWidget.h"
#include "CscanWidget.h"
#include "DepthRulerWidget.h"
#include "SpectrumFifo.h"
#include "FpgaSerial.h"
#include "ScanController.h"
#include "DataPipeline.h"
#include "FileWriter.h"
#include "ConfigManager.h"
#include "TrajectoryLoader.h"
#include "ColorBar.h"
#include "DepthColorBar.h"
#include "ColorMap.h"
#include "Logger.h"
#include "third_party/Spectrum/include/regs.h"
#include <QDebug>

#include <QMenuBar>
#include <QStatusBar>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QEvent>
#include <QLineEdit>
#include <QProgressBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QShowEvent>
#include <QDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QListWidget>
#include <QStackedWidget>
#include <QSerialPortInfo>
#include <QSignalBlocker>
#include <algorithm>
#include <limits>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    qInfo() << "MainWindow ctor begin";
    ui->setupUi(this);

    // Create core objects
    m_config = new ConfigManager(this);
    m_fifo   = new SpectrumFifo();
    m_fpga   = new FpgaSerial(this);
    m_scanCtrl = new ScanController(this);
    m_writer   = new FileWriter(this);
    qInfo() << "Core objects created";

    // Wire ScanController to hardware
    m_scanCtrl->setSpectrumFifo(m_fifo);
    m_scanCtrl->setFpgaSerial(m_fpga);
    m_scanCtrl->setFileWriter(m_writer);
    m_workerThread = new QThread(this);
    connect(m_workerThread, &QThread::finished, this, [this] {
        updateButtonStates(m_scanCtrl->state());
    });

    m_alineTimer = new QTimer(this);
    m_serialRefreshTimer = new QTimer(this);

    setupStatusBar();
    setupConnections();

    // Block signals while populating: onScanModeChanged (→ loadParamsForMode)
    // must only run after spinbox ranges are configured, otherwise setValue()
    // would be clamped to the default 0..99 range.
    ui->comboScanMode->blockSignals(true);
    ui->comboScanMode->addItem(tr("光栅扫描"), static_cast<int>(ScanMode::Raster));
    ui->comboScanMode->addItem(tr("轨迹扫描"), static_cast<int>(ScanMode::Trajectory));
    ui->comboScanMode->blockSignals(false);
    ui->spinNFast->setRange(1, 20000);
    ui->spinNFast->setSingleStep(10);
    ui->spinNFast->setKeyboardTracking(false);
    ui->spinNSlow->setRange(1, 5000);
    ui->spinNSlow->setKeyboardTracking(false);

    // Configure spinboxes — keyboardTracking=false: apply on Enter/focus-loss only
    ui->spinDepth->setRange(1, 65535);
    ui->spinDepth->setSingleStep(100);
    ui->spinDepth->setKeyboardTracking(false);
    ui->spinWinOffset->setRange(0, 65535);
    ui->spinWinOffset->setToolTip(tr("信号窗口起始偏移 (samples)，可在扫描中实时调整"));
    ui->spinWinOffset->setKeyboardTracking(false);
    ui->spinImgW->setRange(100, 2000);
    ui->spinImgW->setValue(500);
    ui->spinImgW->setPrefix("W:");
    ui->spinImgW->setKeyboardTracking(false);
    ui->spinImgH->setRange(100, 2000);
    ui->spinImgH->setValue(500);
    ui->spinImgH->setPrefix("H:");
    ui->spinImgH->setKeyboardTracking(false);

    // Style scan buttons
    ui->btnScanStart->setStyleSheet("color: green; font-weight: bold;");
    ui->btnScanStop->setStyleSheet("color: red; font-weight: bold;");

    // All modes share the same A-line / B-scan / C-scan layout.
    ui->splitter->setStretchFactor(0, 1);
    ui->splitter->setStretchFactor(1, 3);
    ui->splitter->setStretchFactor(2, 3);
    ui->splitter->setSizes({240, 540, 540});

    setMinimumSize(1120, 720);

    // Color bars float immediately below their corresponding OpenGL views.
    m_bscanBar = new ColorBar(ui->centralWidget);
    m_cscanBar = new ColorBar(ui->centralWidget);
    m_depthBar = new DepthColorBar(ui->centralWidget);
    m_depthBar->hide();

    // Load settings
    loadSettings();
    qInfo() << "Settings loaded, depth=" << m_scanParams.depth << "segSize=" << m_scanParams.segmentSize;
    m_depthEncode = m_config->loadDepthEncode();

    // Try to open card
    qInfo() << "Opening card...";
    initializeCard();

    // Serial port refresh + auto-open saved port
    refreshSerialPorts();
    if (ui->comboSerialPort->currentIndex() >= 0)
        onSerialPortChanged(ui->comboSerialPort->currentIndex());
    connect(m_serialRefreshTimer, &QTimer::timeout,
            this, &MainWindow::refreshSerialPorts);
    m_serialRefreshTimer->start(2000);

    // Apply loaded params to spinboxes
    ui->spinDepth->setValue(m_scanParams.depth);
    ui->spinWinOffset->setValue(m_scanParams.windowOffset);
    ui->spinNFast->setValue(m_scanParams.nFast);
    ui->spinNSlow->setValue(m_scanParams.nSlow);
    ui->spinImgW->setValue(m_scanParams.imageWidth);
    ui->spinImgH->setValue(m_scanParams.imageHeight);
    ui->editCsvPath->setText(m_scanParams.trajectoryCsvPath);

    const int modeIndex = ui->comboScanMode->findData(static_cast<int>(m_scanParams.scanMode));
    ui->comboScanMode->setCurrentIndex(modeIndex >= 0 ? modeIndex : 1);
    ui->bscanWidget->setDimensions(m_scanParams.depth, m_scanParams.nFast);
    ui->cscanWidget->setDimensions(m_scanParams.imageWidth, m_scanParams.imageHeight);
    updateScanModeUi();

    // Depth ruler between the A-line and B-scan widgets (depth 0 at top,
    // always visible — independent of the B-scan widget width).
    m_bscanDepthRuler = new DepthRulerWidget(ui->centralWidget);
    ui->splitter->insertWidget(1, m_bscanDepthRuler);
    m_bscanDepthRuler->setDepth(m_scanParams.depth);

    m_scanCtrl->setScanParams(m_scanParams);

    updateButtonStates(ScanState::Idle);
    applyDepthSettings();
}

MainWindow::~MainWindow()
{
    if (m_scanCtrl->isRunning()) m_scanCtrl->stopScan();
    if (m_pipeline) m_pipeline->requestStop();
    if (m_fpga) m_fpga->sendStop();
    if (m_fifo->isOpen()) m_fifo->stopAcquisition();
    m_workerThread->quit();
    // DataPipeline drains its save queue before run() returns. Never destroy it
    // while its acquisition/save threads are still accessing the hardware/data.
    m_workerThread->wait();
    m_fifo->closeCard();
    m_fpga->closePort();
    delete m_fifo;
    delete m_pipeline;
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    if (m_scanCtrl->isRunning()) {
        m_scanCtrl->stopScan();
    }
    m_workerThread->quit();
    event->accept();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    positionColorBars();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, [this]() { positionColorBars(); });
}

void MainWindow::positionColorBars()
{
    auto position = [this](QWidget *bar, QWidget *view, int offset) {
        if (!bar || !view || !view->isVisible()) {
            if (bar) bar->hide();
            return;
        }
        const QPoint bottomLeft = view->mapTo(
            ui->centralWidget, QPoint(0, view->height()));
        bar->setGeometry(bottomLeft.x(), bottomLeft.y() + 3 + offset,
                         view->width(), bar->height());
        bar->show();
    };
    position(m_bscanBar, ui->bscanWidget, 0);
    // The depth bar sits directly under the C-scan, with the intensity bar
    // pushed below it while depth encoding is on.
    const bool depthBarVisible = m_depthEncode;
    if (depthBarVisible)
        position(m_depthBar, ui->cscanWidget, 0);
    else if (m_depthBar)
        m_depthBar->hide();
    position(m_cscanBar, ui->cscanWidget, depthBarVisible ? m_depthBar->height() : 0);
}

void MainWindow::setupStatusBar()
{
    auto *sb = ui->statusBar;
    QList<QLabel*> oldLabels = sb->findChildren<QLabel*>();
    for (auto *l : oldLabels) delete l;

    m_statusScanProgress = new QLabel(tr("扫描: 0 / 0"));
    m_statusScanProgress->setMinimumWidth(120);
    m_statusAlineCount   = new QLabel(tr("A-line: 0 / 0"));
    m_statusAlineCount->setMinimumWidth(120);
    m_statusCardConn     = new QLabel(tr("卡: 未连接"));
    m_statusCardConn->setMinimumWidth(100);

    sb->addPermanentWidget(m_statusScanProgress);
    sb->addPermanentWidget(m_statusAlineCount);
    sb->addPermanentWidget(m_statusCardConn);
}

// ===== Signal/Slot Connections =====

void MainWindow::setupConnections()
{
    // ---- FPGA buttons ----
    connect(ui->btnFpgaStart, &QPushButton::clicked, this, &MainWindow::onFpgaStart);
    connect(ui->btnFpgaStop,  &QPushButton::clicked, this, &MainWindow::onFpgaStop);

    // ---- Serial port ----
    connect(ui->comboSerialPort, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSerialPortChanged);

    // ---- FPGA serial status ----
    connect(m_fpga, &FpgaSerial::portOpened, this, [this](bool ok, const QString &err) {
        if (ok) {
            ui->labelSerialStatus->setText(tr("已打开"));
            ui->labelSerialStatus->setStyleSheet("color: green;");
            ui->labelSerialStatus->setToolTip(tr("串口已打开"));
        } else {
            ui->labelSerialStatus->setText(tr("失败"));
            ui->labelSerialStatus->setStyleSheet("color: red;");
            ui->labelSerialStatus->setToolTip(err.isEmpty() ? tr("串口打开失败") : err);
            qWarning() << "Serial open failed:" << err;
        }
        if (m_serialDebug)
            m_serialDebug->setAcquisitionPort(ok ? m_fpga->portName() : QString());
    });
    connect(m_fpga, &FpgaSerial::portClosed, this, [this]() {
        ui->labelSerialStatus->setText(tr("未打开"));
        ui->labelSerialStatus->setStyleSheet("");
        if (m_serialDebug)
            m_serialDebug->setAcquisitionPort(QString());
    });

    // ---- Card buttons ----
    connect(ui->btnCardStart, &QPushButton::clicked, this, &MainWindow::onCardStart);
    connect(ui->btnCardStop,  &QPushButton::clicked, this, &MainWindow::onCardStop);
    connect(ui->btnForceTrig, &QPushButton::clicked, this, &MainWindow::onForceTrigger);

    // ---- Scan buttons ----
    connect(ui->btnScanStart, &QPushButton::clicked, this, &MainWindow::onScanStart);
    connect(ui->btnScanStop,  &QPushButton::clicked, this, &MainWindow::onScanStop);
    connect(ui->comboScanMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onScanModeChanged);

    // ---- ScanController signals ----
    connect(m_scanCtrl, &ScanController::stateChanged, this, &MainWindow::onScanStateChanged);
    connect(m_scanCtrl, &ScanController::scanStarted, this, &MainWindow::onScanStarted);
    connect(m_scanCtrl, &ScanController::scanFinished, this, &MainWindow::onScanFinished);
    connect(m_scanCtrl, &ScanController::errorOccurred, this, &MainWindow::onScanError);

    // ScanController -> hardware
    connect(m_scanCtrl, &ScanController::fpgaStartRequested, m_fpga, &FpgaSerial::sendStart);
    connect(m_scanCtrl, &ScanController::fpgaStopRequested,  m_fpga, &FpgaSerial::sendStop);
    connect(m_scanCtrl, &ScanController::startWorkerThread, this, [this]() {
        if (!m_workerThread->isRunning())
            m_workerThread->start();
    });
    connect(m_writer, &FileWriter::errorOccurred, this, [this](const QString &msg) {
        showError(tr("文件错误"), msg);
    });

    // ---- Menu bar ----
    ui->menuDAQ->menuAction()->setVisible(false);
    ui->menuHelp->menuAction()->setVisible(false);
    auto *actSettings = ui->menuBar->addAction(tr("采集卡设置"));
    connect(actSettings, &QAction::triggered, this, &MainWindow::onSettingsCard);
    auto *actDisplay = ui->menuBar->addAction(tr("显示设置"));
    connect(actDisplay, &QAction::triggered, this, &MainWindow::onDisplaySettings);
    auto *actSerialDebug = ui->menuBar->addAction(tr("串口调试助手"));
    connect(actSerialDebug, &QAction::triggered, this, &MainWindow::onSerialDebugAssistant);
    auto *actAboutBar = ui->menuBar->addAction(tr("关于"));
    connect(actAboutBar, &QAction::triggered, this, &MainWindow::onAbout);

    // ---- Browse buttons ----
    connect(ui->btnBrowse, &QPushButton::clicked, this, &MainWindow::onBrowseSavePath);
    connect(ui->btnBrowseCsv, &QPushButton::clicked, this, &MainWindow::onBrowseCsvPath);

    // ---- SpinBox changes -> params ----
    connect(ui->spinDepth, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.depth = v;
        ui->bscanWidget->setDimensions(v, m_scanParams.isRaster() ? m_scanParams.nFast : 1);
        if (m_bscanDepthRuler) m_bscanDepthRuler->setDepth(v);
        if (m_pipeline) m_pipeline->setLiveDepth(v);
        persistScanParams();
    });
    connect(ui->spinWinOffset, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.windowOffset = v;
        if (m_pipeline) m_pipeline->setLiveWindowOffset(v);
        persistScanParams();
    });
    connect(ui->spinNFast, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.nFast = v;
        if (m_scanParams.isRaster()) ui->bscanWidget->setDimensions(m_scanParams.depth, v);
        if (m_scanParams.isRaster()) ui->cscanWidget->setDimensions(m_scanParams.nSlow, v);
        persistScanParams();
    });
    connect(ui->spinNSlow, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.nSlow = v;
        if (m_scanParams.isRaster()) ui->cscanWidget->setDimensions(v, m_scanParams.nFast);
        persistScanParams();
    });
    connect(ui->spinImgW, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.imageWidth = v;
        if (m_scanParams.isTrajectory())
            ui->cscanWidget->setDimensions(v, m_scanParams.imageHeight);
        persistScanParams();
    });
    connect(ui->spinImgH, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.imageHeight = v;
        if (m_scanParams.isTrajectory())
            ui->cscanWidget->setDimensions(m_scanParams.imageWidth, v);
        persistScanParams();
    });
    connect(ui->spinBscanStart, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { updateTrajectoryBscanRange(true); });
    connect(ui->spinBscanEnd, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { updateTrajectoryBscanRange(false); });
    connect(ui->editCsvPath, &QLineEdit::textChanged, this, [this] {
        // A new file has an unknown length until it is loaded at scan start.
        if (m_scanParams.isTrajectory() && !m_workerThread->isRunning()) {
            m_scanParams.totalAlines = 0;
            syncTrajectoryBscanControls();
        }
    });

    // ---- Saved-file counter ----
    // Polled with the display: do not enqueue a counter event for every file.

    // ---- Per-format save toggles (auto-persisted into the active mode) ----
    connect(ui->chkRasterDat, &QCheckBox::toggled, this, [this](bool on) {
        m_scanParams.saveDat = on; persistScanParams();
    });
    connect(ui->chkRasterPng, &QCheckBox::toggled, this, [this](bool on) {
        m_scanParams.savePng = on; persistScanParams();
    });
    connect(ui->chkTrajNpy, &QCheckBox::toggled, this, [this](bool on) {
        m_scanParams.saveNpy = on; persistScanParams();
    });
    connect(ui->chkTrajPcd, &QCheckBox::toggled, this, [this](bool on) {
        m_scanParams.savePcd = on; persistScanParams();
    });
    connect(ui->chkTrajPng, &QCheckBox::toggled, this, [this](bool on) {
        m_scanParams.saveMip = on; persistScanParams();
    });
    connect(ui->chkTrajDat, &QCheckBox::toggled, this, [this](bool on) {
        m_scanParams.saveDatTraj = on; persistScanParams();
    });
    // Depth PNG toggles. Ticking one also switches the display over to depth
    // encoding: saving a depth image the operator never saw on screen would be
    // a good way to file a wrong picture.
    connect(ui->chkRasterPngDepth, &QCheckBox::toggled, this, [this](bool on) {
        m_scanParams.saveDepthPng = on;
        persistScanParams();
        // Only the operator's click should flip the display: this handler also
        // runs when loadParamsForMode() restores the checkboxes, and a saved
        // "depth PNG on" must not silently force depth rendering at startup.
        if (on && m_configReady) setDepthEncodingEnabled(true);
    });
    connect(ui->chkTrajPngDepth, &QCheckBox::toggled, this, [this](bool on) {
        m_scanParams.saveMipDepth = on;
        persistScanParams();
        if (on && m_configReady) setDepthEncodingEnabled(true);
    });
    connect(ui->chkSwapXY, &QCheckBox::toggled, this, [this](bool) {
        persistScanParams();
    });
    connect(ui->editCsvPath, &QLineEdit::editingFinished, this, [this]() {
        m_scanParams.trajectoryCsvPath = ui->editCsvPath->text().trimmed();
        persistScanParams();
    });
    connect(ui->editSavePath, &QLineEdit::editingFinished, this, [this]() {
        m_config->saveSavePath(ui->editSavePath->text().trimmed());
    });

    // ---- Aline poll timer (30ms) ----
    m_alineTimer->setInterval(30);
    connect(m_alineTimer, &QTimer::timeout, this, [this]() {
        refreshScanDisplay();
        if (m_pipeline) {
            auto d = m_pipeline->getLatestAline();
            if (!d.empty()) ui->alineWidget->setAlineData(d);
        }
        const int rangeMv = m_config->loadChannelRange(0);
        if (m_bscanBar && ui->bscanWidget->isVisible()) {
            m_bscanBar->setInputRange(rangeMv);
            m_bscanBar->setMaxValue(ui->bscanWidget->currentMaxValue());
        }
        if (m_cscanBar) {
            m_cscanBar->setInputRange(rangeMv);
            m_cscanBar->setMaxValue(ui->cscanWidget->currentMaxValue());
        }
    });
    m_alineTimer->start();
}

// ===== Card initialization =====

void MainWindow::initializeCard()
{
    if (m_fifo->openCard(0)) {
        CardInfo info = m_fifo->cardInfo();
        qInfo() << "Card opened:" << info.cardType << "serial" << info.serialNumber << "mem" << info.memSizeBytes / (1024*1024) << "MB";
        onCardInfoReady(info);
        applyCardConfig();
        m_statusCardConn->setText(tr("卡: 已连接"));
        m_statusCardConn->setStyleSheet("color: green;");
    } else {
        qWarning() << "Failed to open card 0";
        m_statusCardConn->setText(tr("卡: 未连接"));
        m_statusCardConn->setStyleSheet("color: red;");
        showError(tr("采集卡初始化失败"),
                  tr("无法打开 Spectrum 采集卡 (index=0)。\n"
                     "请确认:\n"
                     "1. 驱动程序已安装\n"
                     "2. 采集卡已插入 PCIe 插槽\n"
                     "3. 卡未被其他程序占用"));
    }
}

// Re-apply the active mode's full DAQ configuration (channels, FIFO, clock,
// trigger, DMA buffer) to the card. Called on startup and on every mode switch
// so switching between Raster / Trajectory never leaves stale hardware settings.
void MainWindow::applyCardConfig()
{
    if (!m_fifo->isOpen()) return;
    QString validationError;
    if (!validateAcquisitionParams(validationError)) {
        qWarning() << "Card configuration skipped:" << validationError;
        return;
    }
    const uint64_t chMask = m_config->loadChannelMask();
    const int nCh = m_fifo->cardInfo().maxChannels;
    for (int ch = 0; ch < nCh; ++ch) {
        m_fifo->setInputPath(ch, m_config->loadChannelPath(ch));
        m_fifo->setInputRange(ch, m_config->loadChannelRange(ch));
        m_fifo->setTermination(ch, m_config->loadChannelTermination(ch));
        m_fifo->setCoupling(ch, m_config->loadChannelCoupling(ch));
        m_fifo->setDiffMode(ch, m_config->loadChannelDiffMode(ch));
        m_fifo->setBandwidthLimit(ch, m_config->loadChannelBandwidth(ch));
        if (m_fifo->hasOffsetDAC())
            m_fifo->setOffset(ch, m_config->loadChannelOffset(ch));
    }
    qInfo() << "applyCardConfig: channel setup done, nCh=" << nCh;

    if (!m_fifo->setupFifoMode(m_scanParams.fifoMode, chMask,
                                m_scanParams.preTrigger,
                                m_scanParams.segmentSize,
                                m_scanParams.loopCount))
        qWarning() << "FIFO setup failed:" << m_fifo->lastErrorText();

    m_fifo->redefineTransfer();  // DMA must be re-defined after mode change

    int clockMode = m_config->loadClockMode();
    if (clockMode == SPC_CM_EXTERNAL)
        m_fifo->setExternalClock(m_config->loadClockDivider());
    else if (clockMode == SPC_CM_EXTREFCLOCK)
        m_fifo->setReferenceClock(m_config->loadRefClockFreq() * 1000000LL,
                                  m_config->loadSampleRate());
    else
        m_fifo->setInternalPllClock(m_config->loadSampleRate());

    m_fifo->setExternalTrigger(m_config->loadTriggerMode(),
                                m_config->loadTriggerExtLine(),
                                m_config->loadTriggerTermination());
    m_fifo->setTriggerLevel(m_config->loadTriggerExtLine(),
                             m_config->loadTriggerLevel0(),
                             m_config->loadTriggerLevel1());
    m_fifo->setTriggerCoupling(m_config->loadTriggerExtLine(),
                                m_config->loadTriggerCoupling());
    m_fifo->setTriggerDelay(m_config->loadTriggerDelay());
    m_fifo->setTimeout(5000);

    int64_t bufSize = static_cast<int64_t>(m_config->loadBufferSizeMB()) * 1024 * 1024;
    // Align to segment boundary so ring buffer wrap never splits an A-line
    int segBytes = m_scanParams.segmentSize * m_fifo->cardInfo().bytesPerSample;
    if (segBytes > 0) bufSize = (bufSize / segBytes) * segBytes;
    int32_t notSize = static_cast<int32_t>(m_config->loadNotifySizeKB()) * 1024;
    if (m_fifo->bufferSize() != bufSize || m_fifo->notifySize() != notSize) {
        m_fifo->freeBuffer();
        if (!m_fifo->allocateBuffer(bufSize, notSize))
            qWarning() << "DMA alloc failed:" << m_fifo->lastErrorText();
        else
            qInfo() << "DMA buffer allocated OK (" << bufSize / (1024*1024) << "MB )";
    }
}

bool MainWindow::validateAcquisitionParams(QString &error) const
{
    const int64_t segment = m_scanParams.segmentSize;
    const int64_t pre = m_scanParams.preTrigger;
    const int64_t offset = m_scanParams.windowOffset;
    const int64_t depth = m_scanParams.depth;
    if (segment <= 0) {
        error = tr("段长必须大于 0。");
        return false;
    }
    if (pre < 0 || pre >= segment) {
        error = tr("预触发点数必须小于段长（当前 Pre=%1，Seg=%2）。")
                    .arg(pre).arg(segment);
        return false;
    }
    if (offset < 0 || depth <= 0 || offset + depth > segment) {
        error = tr("信号窗口必须完整位于段内（偏移 %1 + 深度 %2，段长 %3）。")
                    .arg(offset).arg(depth).arg(segment);
        return false;
    }
    const int64_t bufferBytes =
        static_cast<int64_t>(m_config->loadBufferSizeMB()) * 1024 * 1024;
    const int64_t notifyBytes =
        static_cast<int64_t>(m_config->loadNotifySizeKB()) * 1024;
    const int64_t segmentBytes = segment * std::max(1, m_fifo->cardInfo().bytesPerSample);
    const int64_t alignedBuffer = segmentBytes > 0
        ? (bufferBytes / segmentBytes) * segmentBytes : 0;
    if (alignedBuffer < segmentBytes || notifyBytes <= 0 || notifyBytes > alignedBuffer) {
        error = tr("DMA 通知大小必须大于 0 且不超过按段长对齐后的缓冲区。");
        return false;
    }
    if (m_scanParams.isRaster()) {
        const int64_t alines = static_cast<int64_t>(m_scanParams.nFast) * m_scanParams.nSlow;
        if (alines <= 0 || alines > std::numeric_limits<int>::max()) {
            error = tr("光栅扫描点数超过处理管线上限。");
            return false;
        }
    }
    error.clear();
    return true;
}

void MainWindow::spawnPipeline()
{
    const uint64_t generation = ++m_pipelineGeneration;
    // 1. Stop card to unblock waitForData()
    if (m_fifo && m_fifo->isOpen()) {
        m_fifo->stopAcquisition();
    }

    // 2. Request pipeline stop
    if (m_pipeline) {
        m_pipeline->requestStop();
        disconnect(m_pipeline, nullptr, this, nullptr);
        disconnect(m_pipeline, nullptr, m_scanCtrl, nullptr);
    }

    // 3. Wait for worker thread to exit
    if (m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait();
    }

    // 4. Delete old pipeline
    delete m_pipeline;
    m_pipeline = nullptr;

    // Create fresh pipeline
    m_pipeline = new DataPipeline(m_fifo);
    m_scanCtrl->setPipeline(m_pipeline);

    m_pipeline->setDisplayPollingEnabled(true);
    connect(m_pipeline, &DataPipeline::scanFinished, this, [this, generation] {
        if (generation == m_pipelineGeneration) {
            // Flush before the controller sets the terminal status text.
            refreshScanDisplay();
            m_scanCtrl->onPipelineScanComplete();
        }
    });
    connect(m_pipeline, &DataPipeline::errorOccurred, this,
            [this, generation](const QString &message) {
                if (generation == m_pipelineGeneration) m_scanCtrl->onPipelineError(message);
            });

    connect(m_pipeline, &DataPipeline::firstDataReceived, this, [this, generation] {
        if (generation != m_pipelineGeneration) return;
        if (!m_firstDataSent) {
            m_firstDataSent = true;
            m_scanCtrl->onFirstDataReceived();
        }
    });
    connect(m_pipeline, &DataPipeline::acquisitionComplete, this,
            [this, generation] {
                if (generation == m_pipelineGeneration)
                    m_scanCtrl->onAcquisitionComplete();
            });

    m_pipeline->moveToThread(m_workerThread);
    connect(m_workerThread, &QThread::started, m_pipeline, &DataPipeline::run);
    connect(m_scanCtrl, &ScanController::pipelineStopRequested,
            m_pipeline, &DataPipeline::requestStop, Qt::DirectConnection);
}

// ===== Button handlers =====

void MainWindow::onFpgaStart()
{
    if (!m_fpga->isOpen()) {
        showError(tr("串口未打开"), tr("请先在下拉菜单中选择串口端口。"));
        return;
    }
    m_fpga->sendStart();
    logStatus(tr("FPGA: F1 已发送"));
}

void MainWindow::onFpgaStop()
{
    m_fpga->sendStop();
    logStatus(tr("FPGA: F0 已发送"));
}

void MainWindow::onCardStart()
{
    if (!m_fifo->isOpen() && !m_fifo->openCard(0)) {
        showError(tr("采集卡打开失败"),
                  tr("无法打开 Spectrum 采集卡 0。"));
        return;
    }
    if (!m_fifo->startAcquisition())
        showError(tr("采集启动失败"), m_fifo->lastErrorText());
    else
        logStatus(tr("采集卡: 已启动"));
}

void MainWindow::onCardStop()
{
    if (m_fifo->isOpen()) {
        if (!m_fifo->stopAcquisition())
            showError(tr("采集停止失败"), m_fifo->lastErrorText());
        else
            logStatus(tr("采集卡: 已停止"));
    }
}

void MainWindow::onForceTrigger()
{
    if (m_fifo->isOpen()) {
        if (!m_fifo->forceTrigger())
            showError(tr("强制触发失败"), m_fifo->lastErrorText());
    }
}

void MainWindow::onCardInfoReady(const CardInfo &info)
{
    ui->labelCardType->setText(info.cardType);
    ui->labelSerialNo->setText(QString::number(info.serialNumber));
    double memMB = static_cast<double>(info.memSizeBytes) / (1024.0 * 1024.0);
    ui->labelMemSize->setText(QString("%1 MB").arg(memMB, 0, 'f', 0));
    ui->labelCardStatus->setText(tr("就绪"));
    ui->labelCardError->setText(tr("—"));
    ui->labelCardError->setStyleSheet("");
}

// ===== Scan lifecycle =====

void MainWindow::onScanStart()
{
    // Do not replace mappings or the writer's session until every old worker
    // has finished, including the dedicated save thread.
    if (m_scanCtrl->isRunning() || m_workerThread->isRunning()) return;
    m_scanParams.scanMode = static_cast<ScanMode>(
        ui->comboScanMode->currentData().toInt());
    QString validationError;
    if (!validateAcquisitionParams(validationError)) {
        showError(tr("参数无效"), validationError);
        return;
    }

    double radiusM = 0.0;
    if (m_scanParams.isTrajectory()) {
        const QString csvPath = ui->editCsvPath->text().trimmed();
        if (csvPath.isEmpty()) {
            showError(tr("缺少轨迹文件"), tr("请先选择轨迹 CSV 文件。"));
            return;
        }

        TrajectoryLoader loader;
        if (!loader.load(csvPath, m_scanParams.imageWidth, m_scanParams.imageHeight,
                         ui->chkSwapXY->isChecked(), m_trajectoryPoints,
                         m_trajectoryMapInfo, radiusM)) {
            showError(tr("轨迹加载失败"), loader.lastError());
            return;
        }
        m_scanParams.totalAlines = loader.totalAlines();
        syncTrajectoryBscanControls();
        m_scanParams.scanRadiusM = radiusM;
        m_scanParams.trajectoryCsvPath = csvPath;
    } else {
        m_scanParams.totalAlines = m_scanParams.nFast * m_scanParams.nSlow;
        m_scanParams.scanRadiusM = 0.0;
        m_trajectoryPoints.clear();
        m_trajectoryMapInfo.clear();
    }

    m_config->setActiveMode(m_scanParams.scanMode);
    m_scanParams.channelMask = m_config->loadChannelMask();
    if (m_scanParams.channelMask == 0 ||
        (m_scanParams.channelMask & (m_scanParams.channelMask - 1)) != 0) {
        showError(tr("通道配置不受支持"),
                  tr("当前数据管线一次只解析一个采集通道。请在“采集卡设置”中只启用一个通道。"));
        return;
    }

    if (!m_fpga->isOpen()) {
        const QString port = ui->comboSerialPort->currentData().toString();
        const QString hint = port.isEmpty()
            ? tr("请选择串口端口，或检查串口/FPGA 连接。")
            : tr("串口 %1 未能打开。请检查权限、设备占用和物理连接。")
                  .arg(port);
        showError(tr("串口未打开"), hint);
        return;
    }

    // The per-format toggles are the single storage control. Synchronise them
    // into the active mode's params FIRST, then decide whether this scan has
    // any output at all — asking ScanParams::hasAnyOutput() rather than a flat
    // OR over every field, which would let a stale toggle belonging to the
    // *other* scan mode create a session folder for a scan that saves nothing.
    m_scanParams.saveDat     = ui->chkRasterDat->isChecked();
    m_scanParams.savePng     = ui->chkRasterPng->isChecked();
    m_scanParams.saveDepthPng = ui->chkRasterPngDepth->isChecked();
    m_scanParams.saveNpy     = ui->chkTrajNpy->isChecked();
    m_scanParams.savePcd     = ui->chkTrajPcd->isChecked();
    m_scanParams.saveMip     = ui->chkTrajPng->isChecked();
    m_scanParams.saveMipDepth = ui->chkTrajPngDepth->isChecked();
    m_scanParams.saveDatTraj = ui->chkTrajDat->isChecked();
    m_config->setActiveMode(m_scanParams.scanMode);
    persistScanParams();

    // No format enabled for THIS mode: run the scan without touching the disk
    // (in particular, do not create a session folder).
    const bool anySave = m_scanParams.hasAnyOutput();
    m_writer->setEnabled(anySave);
    m_writer->setSavePath(ui->editSavePath->text().trimmed());
    // Rows per frame are needed to split a continuous session's globally
    // numbered B-scan rows into per-frame folders.
    m_writer->setRowsPerFrame(m_scanParams.nSlow);
    if (anySave && !m_writer->beginSession(ui->chkContinuous->isChecked())) return;

    m_scanCtrl->setScanParams(m_scanParams);
    spawnPipeline();
    // Flush queued completion/save notifications from the previous generation
    // before counters are reset for this scan.
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);

    if (!m_pipeline) return;
    m_pipeline->setScanMode(m_scanParams.scanMode);
    m_pipeline->setRasterSize(m_scanParams.nFast, m_scanParams.nSlow);
    m_pipeline->setMapInfo(m_scanParams.isTrajectory() ? &m_trajectoryMapInfo : nullptr);
    if (m_scanParams.isTrajectory())
        m_pipeline->setGridSize(m_scanParams.imageWidth, m_scanParams.imageHeight);
    m_pipeline->setUpdateInterval(m_scanParams.cscanUpdateInterval);
    m_pipeline->setSegmentSize(m_scanParams.segmentSize);
    m_pipeline->setTrajectoryBscanRange(m_scanParams.trajectoryBscanStart,
                                        m_scanParams.trajectoryBscanEnd);
    m_pipeline->setContinuous(ui->chkContinuous->isChecked());
    m_pipeline->setColorParams(m_config->loadColorMapping(),
                               m_config->loadColorClipping());
    m_pipeline->setColorMap(m_config->loadColorMap());
    m_pipeline->setDepthEncodeEnabled(m_depthEncode);
    m_pipeline->setSaveDepthMapsEnabled(m_scanParams.isRaster()
                                            ? m_scanParams.saveDepthPng
                                            : m_scanParams.saveMipDepth);
    m_pipeline->setDepthConfig(loadDepthConfig());
    m_pipeline->setLiveDepth(m_scanParams.depth);
    m_pipeline->setLiveWindowOffset(m_scanParams.windowOffset);
    if (m_writer->isEnabled()) {
        m_pipeline->setSaveContext(m_writer,
                                   m_scanParams.isTrajectory() ? &m_trajectoryMapInfo : nullptr,
                                   m_scanParams.imageWidth, m_scanParams.imageHeight);
    } else {
        m_pipeline->setSaveContext(nullptr, nullptr, 0, 0);
    }

    // Per-format outputs, stored data type, PCD segmentation and scan radius
    m_pipeline->setSaveFormats(m_scanParams.saveDat, m_scanParams.savePng,
                               m_scanParams.saveDepthPng,
                               m_scanParams.saveNpy, m_scanParams.savePcd,
                               m_scanParams.saveMip, m_scanParams.saveDatTraj,
                               m_scanParams.saveMipDepth);
    m_pipeline->setSaveDataFormat(m_scanParams.dataFormat);
    m_pipeline->setSaveSegmentation(m_scanParams.pcdSegMode,
                                    m_scanParams.pcdSegThreshold);
    m_pipeline->setScanRadiusM(m_scanParams.scanRadiusM);

    m_firstDataSent = false;
    m_savedItemCount = 0;
    ui->labelSavedCount->setText(tr("Saved: 0"));
    ui->progressScan->setRange(0, m_scanParams.totalAlines);
    ui->progressScan->setValue(0);

    ui->bscanWidget->resetBscan();
    ui->labelBscanInfo->setText(tr("等待 B-scan 数据…"));
    if (m_scanParams.isRaster()) {
        ui->bscanWidget->setDimensions(m_scanParams.depth, m_scanParams.nFast);
        if (m_bscanDepthRuler) m_bscanDepthRuler->setDepth(m_scanParams.depth);
        ui->cscanWidget->setDimensions(m_scanParams.nSlow, m_scanParams.nFast);
        ui->cscanWidget->setScanRadius(0.0, 0.0);
    } else {
        ui->cscanWidget->setDimensions(m_scanParams.imageWidth, m_scanParams.imageHeight);
        ui->cscanWidget->setScanRadius(radiusM, radiusM * 2.0);
    }

    m_scanCtrl->startScan();
}

void MainWindow::onScanStop()
{
    m_scanCtrl->stopScan();
}

void MainWindow::onScanStarted()
{
    updateButtonStates(ScanState::Scanning);
}

void MainWindow::onScanFinished()
{
    m_workerThread->quit();
    updateButtonStates(m_scanCtrl->state());
}

void MainWindow::refreshScanDisplay()
{
    const auto saved = m_writer->isEnabled() ? m_writer->savedItemCount() : 0;
    if (saved != m_savedItemCount) {
        m_savedItemCount = saved;
        ui->labelSavedCount->setText(tr("Saved: %1").arg(saved));
    }
    if (!m_pipeline) return;
    auto update = m_pipeline->takeDisplayUpdate();
    if (update.fifo) {
        ui->fifoFillBar->setValue(update.fifo->fillSizePromille);
        ui->labelCardStatus->setText(decodeM2Status(update.fifo->m2Status));
    }
    if (update.progress) onScanProgress(update.progress->first, update.progress->second);
    if (update.bscan) {
        const auto &b = *update.bscan;
        const int points = b.last - b.first + 1;
        if (b.trajectory) {
            ui->bscanWidget->setRawBscanData(b.samples, b.depth, points);
            ui->labelBscanInfo->setText(tr("当前显示：第 %1 帧，点 %2–%3（%4 条 A-line）")
                .arg(b.frame + 1).arg(b.first).arg(b.last).arg(points));
        } else {
            ui->bscanWidget->setBscanData(b.samples, b.depth, points);
            ui->labelBscanInfo->setText(tr("光栅实时切片：第 %1 行，%2 条 A-line")
                .arg(b.row + 1).arg(points));
        }
        if (m_bscanDepthRuler) m_bscanDepthRuler->setDepth(b.depth);
    }
    if (update.cscan) {
        auto &c = *update.cscan;
        if (c.trajectory) {
            m_lastMip = std::move(c.intensity);
            m_lastMipMax = m_lastMip.empty() ? 0.0f
                : *std::max_element(m_lastMip.begin(), m_lastMip.end());
            m_lastDepthMap.clear();
            if (m_depthEncode && !c.depth.empty())
                applyDepthMap(c.frame, c.depth, c.width, c.height);
            else {
                recolorLastMip();
                ui->labelAlineCount->setText(tr("Frame: %1").arg(c.frame + 1));
            }
            m_statusAlineCount->setText(tr("Frame: %1").arg(c.frame + 1));
        } else {
            ui->cscanWidget->updateRasterFrame(c.intensity, c.depth, c.width, c.height);
            const auto label = tr("Frame: %1  B-scan: %2 / %3")
                .arg(c.frame + 1).arg(c.completedRows).arg(c.width);
            ui->labelAlineCount->setText(label);
            m_statusAlineCount->setText(label);
        }
    }
}

// ===== Depth encoding =====

DepthConfig MainWindow::loadDepthConfig() const
{
    DepthConfig config;
    config.mode = m_config->loadDepthDisplayMode() == 0
                      ? DepthDisplayMode::Pure
                      : DepthDisplayMode::Modulated;
    config.colorMap = m_config->loadDepthColorMap();
    config.maxIndex = m_config->loadDepthMaxIndex();
    config.minIntensityFraction = m_config->loadDepthGatePercent() / 100.0;
    config.fallbackDepth = m_scanParams.depth;
    return config;
}

// Push the stored depth settings to everything that renders depth. Called at
// startup and whenever the display settings change; cheap enough to be
// unconditional.
void MainWindow::applyDepthSettings()
{
    const DepthConfig config = loadDepthConfig();

    ui->cscanWidget->setDepthConfig(config);
    ui->cscanWidget->setDepthDisplay(m_depthEncode);
    // The colormap's far end: an explicit setting, or the full signal window.
    const int range = config.maxIndex > 0 ? config.maxIndex
                                          : std::max(1, m_scanParams.depth - 1);
    ui->cscanWidget->setDepthRange(config.maxIndex);
    ui->cscanWidget->setWindowDepth(m_scanParams.depth);
    if (m_depthBar) {
        m_depthBar->setRange(range);
        m_depthBar->setColorMap(config.colorMap);
        m_depthBar->setVisible(m_depthEncode);
    }
    if (m_pipeline) {
        m_pipeline->setDepthEncodeEnabled(m_depthEncode);
        m_pipeline->setDepthConfig(config);
    }
    // Repaint from the cached map when there is one, so a colormap or range
    // change is visible immediately instead of at the next frame.
    if (m_depthEncode && !m_lastDepthMap.empty()) updateLastDepthImage();
    QTimer::singleShot(0, this, [this] { positionColorBars(); });
}

void MainWindow::setDepthEncodingEnabled(bool on)
{
    if (m_depthEncode == on) return;
    m_depthEncode = on;
    m_config->saveDepthEncode(on);
    if (m_pipeline) m_pipeline->setDepthEncodeEnabled(on);
    // The toggle decides whether the C-scan is painted by depth or intensity,
    // so it applies immediately mid-scan as well.
    ui->cscanWidget->setDepthDisplay(on);
    if (!on) m_lastDepthMap.clear();
    applyDepthSettings();
    logStatus(on ? tr("深度编码: 开") : tr("深度编码: 关"));
}

void MainWindow::updateLastDepthImage()
{
    if (m_lastDepthMap.empty() || m_lastDepthW <= 0 || m_lastDepthH <= 0) return;
    if (static_cast<int>(m_lastDepthMap.size()) < m_lastDepthW * m_lastDepthH) return;

    QImage img;
    encodeDepthImage(m_lastDepthMap.data(),
                     m_lastMip.empty() ? nullptr : m_lastMip.data(),
                     m_lastDepthW, m_lastDepthH, m_scanParams.depth,
                     loadDepthConfig(), img);
    ui->cscanWidget->updateMipImage(img, m_lastMipMax);
}

// A completed trajectory frame: its depth map drives both the live depth image
// and, through the widget, the appearance of the saved depth PNG.
void MainWindow::applyDepthMap(int frameIdx, const std::vector<float> &depthMap,
                               int imageW, int imageH)
{
    if (!m_depthEncode || imageW <= 0 || imageH <= 0) return;
    if (static_cast<int>(depthMap.size()) < imageW * imageH) return;

    m_lastDepthMap.assign(depthMap.begin(), depthMap.begin() + imageW * imageH);
    m_lastDepthW = imageW;
    m_lastDepthH = imageH;

    QImage img;
    encodeDepthImage(m_lastDepthMap.data(),
                     m_lastMip.empty() ? nullptr : m_lastMip.data(),
                     imageW, imageH, m_scanParams.depth, loadDepthConfig(), img);
    ui->cscanWidget->updateMipImage(img, m_lastMipMax);
    ui->labelAlineCount->setText(tr("Frame: %1 (深度编码)").arg(frameIdx + 1));
}

void MainWindow::onScanStateChanged(ScanState state)
{
    updateButtonStates(state);
    switch (state) {
    case ScanState::Idle:
        m_statusScanProgress->setText(tr("空闲"));
        ui->labelScanState->setText(tr("空闲"));
        ui->labelScanState->setStyleSheet("");
        break;
    case ScanState::Armed:
        m_statusScanProgress->setText(tr("就绪..."));
        ui->labelScanState->setText(tr("已武装 — 等待触发"));
        ui->labelScanState->setStyleSheet("color: orange; font-weight: bold;");
        break;
    case ScanState::Scanning:
        m_statusScanProgress->setText(tr("采集中..."));
        ui->labelScanState->setText(tr("采集中"));
        ui->labelScanState->setStyleSheet("color: green; font-weight: bold;");
        break;
    case ScanState::Done:
        m_statusScanProgress->setText(tr("完成"));
        ui->labelScanState->setText(tr("完成"));
        ui->labelScanState->setStyleSheet("");
        break;
    case ScanState::Stopping:
        m_statusScanProgress->setText(tr("正在结束采集并等待存盘…"));
        ui->labelScanState->setText(tr("正在结束采集并等待存盘…"));
        ui->labelScanState->setStyleSheet("color: orange;");
        break;
    case ScanState::Error:
        m_statusScanProgress->setText(tr("错误"));
        ui->labelScanState->setText(tr("错误"));
        ui->labelScanState->setStyleSheet("color: red; font-weight: bold;");
        break;
    }
}

void MainWindow::onScanError(const QString &msg)
{
    m_statusCardConn->setText(tr("扫描错误"));
    m_statusCardConn->setStyleSheet("color: red; font-weight: bold;");
    QMessageBox::critical(this, tr("采集错误"), msg);
    updateButtonStates(m_scanCtrl->state());
}

// ===== Data display =====

void MainWindow::onScanProgress(int current, int total)
{
    ui->progressScan->setMaximum(total);
    ui->progressScan->setValue(current);
    m_statusScanProgress->setText(tr("扫描: %1 / %2").arg(current).arg(total));
    if (m_scanParams.isTrajectory()) {
        ui->labelAlineCount->setText(tr("A-line: %1 / %2").arg(current).arg(total));
        m_statusAlineCount->setText(tr("A-line: %1 / %2").arg(current).arg(total));
    }
}

void MainWindow::onScanModeChanged(int /*index*/)
{
    if (m_scanCtrl->isRunning() || m_workerThread->isRunning()) return;
    m_scanParams.scanMode = static_cast<ScanMode>(
        ui->comboScanMode->currentData().toInt());
    loadParamsForMode(m_scanParams.scanMode);
    updateScanModeUi();
}

// Load and apply the parameter set of the given mode (independent per-mode
// management). All scan / DAQ / channel values are mode-scoped; switching
// modes re-applies the whole hardware configuration to the card.
void MainWindow::loadParamsForMode(ScanMode mode)
{
    m_config->setActiveMode(mode);
    ScanParams fresh = m_config->loadScanParams();
    fresh.scanMode = mode;
    m_scanParams = fresh;

    ui->spinDepth->setValue(fresh.depth);
    ui->spinWinOffset->setValue(fresh.windowOffset);
    ui->spinNFast->setValue(fresh.nFast);
    ui->spinNSlow->setValue(fresh.nSlow);
    ui->spinImgW->setValue(fresh.imageWidth);
    ui->spinImgH->setValue(fresh.imageHeight);
    ui->editCsvPath->setText(fresh.trajectoryCsvPath);
    syncTrajectoryBscanControls();
    ui->chkRasterDat->setChecked(fresh.saveDat);
    ui->chkRasterPng->setChecked(fresh.savePng);
    ui->chkRasterPngDepth->setChecked(fresh.saveDepthPng);
    ui->chkTrajNpy->setChecked(fresh.saveNpy);
    ui->chkTrajPcd->setChecked(fresh.savePcd);
    ui->chkTrajPng->setChecked(fresh.saveMip);
    ui->chkTrajPngDepth->setChecked(fresh.saveMipDepth);
    ui->chkTrajDat->setChecked(fresh.saveDatTraj);

    persistScanParams();
    applyCardConfig();
}

// Instant auto-save of the active mode's parameter set (no save/load buttons).
// Disabled until initial settings are loaded: the constructor's setRange /
// setChecked calls emit valueChanged before the config is ready, and persisting
// those pre-config values would taint the saved parameter set.
void MainWindow::persistScanParams()
{
    if (!m_configReady) return;
    m_config->setActiveMode(m_scanParams.scanMode);
    m_config->saveScanParams(m_scanParams);
}

void MainWindow::updateScanModeUi()
{
    const bool raster = m_scanParams.isRaster();
    ui->lblNFast->setVisible(raster);
    ui->spinNFast->setVisible(raster);
    ui->lblNSlow->setVisible(raster);
    ui->spinNSlow->setVisible(raster);
    ui->bscanWidget->show();
    ui->bscanWidget->resetBscan();
    if (m_bscanDepthRuler) m_bscanDepthRuler->setDepth(m_scanParams.depth);
    ui->lblBscanRange->setEnabled(!raster);
    ui->spinBscanStart->setEnabled(!raster);
    ui->spinBscanEnd->setEnabled(!raster);
    syncTrajectoryBscanControls();
    ui->labelBscanInfo->setText(raster ? tr("光栅实时切片") :
        tr("按采集顺序选取，包含两端；连续采集中修改于下一帧生效"));

    ui->lblImgSize->setVisible(!raster);
    ui->spinImgW->setVisible(!raster);
    ui->spinImgH->setVisible(!raster);
    ui->chkSwapXY->setVisible(!raster);
    ui->lblCsv->setVisible(!raster);
    ui->editCsvPath->setVisible(!raster);
    ui->btnBrowseCsv->setVisible(!raster);

    // Per-format save toggles: mode-specific
    ui->chkRasterDat->setVisible(raster);
    ui->chkRasterPng->setVisible(raster);
    ui->chkRasterPngDepth->setVisible(raster);
    ui->chkTrajNpy->setVisible(!raster);
    ui->chkTrajPcd->setVisible(!raster);
    ui->chkTrajPng->setVisible(!raster);
    ui->chkTrajPngDepth->setVisible(!raster);
    ui->chkTrajDat->setVisible(!raster);

    if (raster) {
        ui->bscanWidget->setDimensions(m_scanParams.depth, m_scanParams.nFast);
        if (m_bscanDepthRuler) m_bscanDepthRuler->setDepth(m_scanParams.depth);
        ui->cscanWidget->setDimensions(m_scanParams.nSlow, m_scanParams.nFast);
        ui->cscanWidget->setScanRadius(0.0, 0.0);
        ui->progressScan->setRange(0, m_scanParams.nFast * m_scanParams.nSlow);
    } else {
        ui->cscanWidget->setDimensions(m_scanParams.imageWidth,
                                       m_scanParams.imageHeight);
    }
    // The depth color bar's range follows the mode's own depth window. Done
    // here (not inside applyDepthSettings) to keep the two directions from
    // calling each other.
    const DepthConfig depthConfig = loadDepthConfig();
    if (m_depthBar) {
        m_depthBar->setRange(depthConfig.maxIndex > 0 ? depthConfig.maxIndex
                                                      : std::max(1, m_scanParams.depth - 1));
        m_depthBar->setVisible(m_depthEncode);
    }
    ui->cscanWidget->setWindowDepth(m_scanParams.depth);
    QTimer::singleShot(0, this, [this] { positionColorBars(); });
}

void MainWindow::syncTrajectoryBscanControls()
{
    const QSignalBlocker startBlock(ui->spinBscanStart);
    const QSignalBlocker endBlock(ui->spinBscanEnd);
    const int total = m_scanParams.isTrajectory() ? m_scanParams.totalAlines : 0;
    const int maximum = total > 0 ? total : std::numeric_limits<int>::max();
    m_scanParams.trajectoryBscanStart = std::clamp(m_scanParams.trajectoryBscanStart, 1, maximum);
    m_scanParams.trajectoryBscanEnd = std::clamp(m_scanParams.trajectoryBscanEnd,
                                               m_scanParams.trajectoryBscanStart, maximum);
    ui->spinBscanStart->setMaximum(maximum);
    ui->spinBscanEnd->setMaximum(maximum);
    ui->spinBscanStart->setValue(m_scanParams.trajectoryBscanStart);
    ui->spinBscanEnd->setValue(m_scanParams.trajectoryBscanEnd);
    const QString hint = tr("按 CSV 有效数据行的采集顺序从 1 编号（非 CSV index 列或 XY 坐标）。"
                            "包含两端。修改后下一帧生效；停止后修改需重新采集。") +
        (total > 0 ? tr(" 当前帧共 %1 点。").arg(total) : tr("加载轨迹后自动限制到实际点数。"));
    ui->spinBscanStart->setToolTip(hint);
    ui->spinBscanEnd->setToolTip(hint);
}

void MainWindow::updateTrajectoryBscanRange(bool startChanged)
{
    if (!m_configReady || !m_scanParams.isTrajectory()) return;
    int first = ui->spinBscanStart->value();
    int last = ui->spinBscanEnd->value();
    if (first > last) {
        if (startChanged) last = first;
        else first = last;
    }
    m_scanParams.trajectoryBscanStart = first;
    m_scanParams.trajectoryBscanEnd = last;
    syncTrajectoryBscanControls();
    if (m_pipeline) m_pipeline->setTrajectoryBscanRange(first, last);
    persistScanParams();
    ui->labelBscanInfo->setText(m_workerThread->isRunning()
        ? tr("区间 %1–%2 将于下一帧生效；图像保留上一段数据").arg(first).arg(last)
        : tr("待采集区间：%1–%2（包含两端）").arg(first).arg(last));
}

void MainWindow::onSerialPortChanged(int /*index*/)
{
    QString port = ui->comboSerialPort->currentData().toString();
    if (port.isEmpty()) return;

    // A tty must have exactly one reader in this program. If the debug
    // assistant already owns this port, opening it for the acquisition card
    // would split FPGA responses between the two readers.
    if (m_serialDebug && m_serialDebug->isPortOpen()) {
        if (SerialDebugDialog::normalizedPortName(m_serialDebug->portName()) ==
            SerialDebugDialog::normalizedPortName(port)) {
            ui->labelSerialStatus->setText(tr("被调试助手占用"));
            ui->labelSerialStatus->setStyleSheet("color: #b26a00;");
            ui->labelSerialStatus->setToolTip(
                tr("串口调试助手正在使用 %1。关闭调试串口后会自动恢复。").arg(port));
            showError(tr("串口被占用"),
                      tr("“串口调试助手”正在使用 %1。\n"
                         "请先在调试助手中关闭该串口，程序随后会自动重新打开采集卡串口。")
                          .arg(port));
            return;
        }
    }

    if (m_fpga->isOpen())
        m_fpga->closePort();

    m_fpga->openPort(port);
    m_config->saveSerialPort(port);
}

void MainWindow::refreshSerialPorts()
{
    QString previousData = ui->comboSerialPort->currentData().toString();
    ui->comboSerialPort->blockSignals(true);
    ui->comboSerialPort->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    if (ports.isEmpty()) {
        ui->comboSerialPort->addItem(tr("未检测到串口"), QString());
        ui->comboSerialPort->setEnabled(false);
        if (!m_fpga->isOpen()) {
            ui->labelSerialStatus->setText(tr("无可用串口"));
            ui->labelSerialStatus->setStyleSheet("color: orange;");
            ui->labelSerialStatus->setToolTip(
                tr("未检测到串口设备。请检查 USB/串口连接。"));
        }
        ui->comboSerialPort->blockSignals(false);
        return;
    }

    ui->comboSerialPort->setEnabled(!m_scanCtrl->isRunning());
    for (const auto &p : ports) {
        ui->comboSerialPort->addItem(p.portName() + " — " + p.description(),
                                     p.portName());
    }

    // Prefer the user's last selection, else the saved port, else the first found.
    QString target = !previousData.isEmpty() ? previousData : m_config->loadSerialPort();
    int idx = -1;
    if (!target.isEmpty()) idx = ui->comboSerialPort->findData(target);
    if (idx < 0) {
        idx = 0;
        // The previously-selected/opened port vanished: drop the stale handle.
        if (m_fpga->isOpen() && m_fpga->portName() == previousData)
            m_fpga->closePort();
    }
    ui->comboSerialPort->setCurrentIndex(idx);

    ui->comboSerialPort->blockSignals(false);
}

void MainWindow::onBrowseSavePath()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("选择存储路径"),
                                                     ui->editSavePath->text());
    if (!dir.isEmpty()) {
        ui->editSavePath->setText(dir);
        m_config->saveSavePath(dir);
    }
}

void MainWindow::onBrowseCsvPath()
{
    QString path = QFileDialog::getOpenFileName(this, tr("选择螺旋轨迹 CSV 文件"),
                                                 QString(),
                                                 tr("CSV 文件 (*.csv);;所有文件 (*.*)"));
    if (!path.isEmpty()) {
        ui->editCsvPath->setText(path);
        m_scanParams.trajectoryCsvPath = path.trimmed();
        persistScanParams();
    }
}

// ===== Settings Dialog (trigger/clock/channels/mode) — kept from M4_AD_FIFO =====
// (Same as original, with bscanWidget references updated to cscanWidget)

void MainWindow::onSettingsCard()
{
    if (m_scanCtrl->isRunning() || m_workerThread->isRunning()) {
        showError(tr("扫描进行中"),
                  tr("请先停止扫描，再修改采集卡、触发、时钟或 DMA 参数。"));
        return;
    }
    qInfo() << "onSettingsCard: begin";

    int32_t availClockModes = m_fifo->isOpen() ? m_fifo->availableClockModes() : (SPC_CM_INTPLL);
    int32_t availTrigSrc    = m_fifo->isOpen() ? m_fifo->availableTriggerSources() : (SPC_TMASK_EXT0 | SPC_TMASK_XIO0 | SPC_TMASK_SOFTWARE);
    int32_t availChannelTrig = m_fifo->isOpen()
        ? m_fifo->availableChannelTriggerSources()
        : (SPC_TMASK0_CH0 | SPC_TMASK0_CH1 | SPC_TMASK0_CH2 | SPC_TMASK0_CH3);
    int nChannels = m_fifo->isOpen() ? m_fifo->cardInfo().maxChannels : 4;
    int nPaths    = m_fifo->isOpen() ? m_fifo->inputPathCount() : 1;
    int32_t feat0 = m_fifo->isOpen() ? m_fifo->inputFeatures(0) : 0;
    bool hasTerm    = (feat0 & 1);
    bool hasDiff    = (feat0 & 4);
    bool hasOffs    = m_fifo->hasOffsetDAC() && (feat0 & (8|16));
    bool hasCoupling= (feat0 & 0x80) && (feat0 & 0x40);
    bool hasBW      = (feat0 & 0x100);

    auto getRanges = [&](int p) -> QVector<SpectrumFifo::InputRange> {
        if (m_fifo->isOpen()) {
            QVector<SpectrumFifo::InputRange> rv;
            for (auto &r : m_fifo->inputRanges(p)) rv.append(r);
            if (rv.isEmpty())
                return {{-200,200},{-500,500},{-1000,1000},{-2000,2000},{-5000,5000},{-10000,10000}};
            return rv;
        }
        return {{-200,200},{-500,500},{-1000,1000},{-2000,2000},{-5000,5000},{-10000,10000}};
    };
    QVector<QVector<SpectrumFifo::InputRange>> pathRanges(nPaths);
    for (int p = 0; p < nPaths; ++p) pathRanges[p] = getRanges(p);

    QDialog dlg(this);
    dlg.setWindowTitle(tr("采集卡设置"));
    dlg.resize(720, 520);
    auto *outerLayout = new QVBoxLayout(&dlg);
    auto *bodyRow = new QHBoxLayout();

    auto *list = new QListWidget(&dlg);
    list->setFixedWidth(150);
    list->addItem(tr("通道配置"));
    list->addItem(tr("触发设置"));
    list->addItem(tr("时钟设置"));
    list->addItem(tr("采集模式"));
    list->setCurrentRow(0);
    bodyRow->addWidget(list);

    auto *stack = new QStackedWidget(&dlg);
    bodyRow->addWidget(stack);

    // ---- Page 0: Channels ----
    {
        auto *page = new QWidget();
        auto *lay  = new QVBoxLayout(page);
        QStringList headers;
        headers << tr("使能");
        if (nPaths > 1) headers << tr("路径");
        headers << tr("量程 (mV)");
        if (hasTerm)     headers << tr("终端");
        if (hasCoupling) headers << tr("耦合");
        if (hasDiff)     headers << tr("模式");
        if (hasBW)       headers << tr("BW");
        if (hasOffs)     headers << tr("偏置 (mV)");

        auto *table = new QTableWidget(nChannels, static_cast<int>(headers.size()));
        table->setHorizontalHeaderLabels(headers);
        table->verticalHeader()->setVisible(false);
        uint64_t savedMask = m_config->loadChannelMask();

        for (int ch = 0; ch < nChannels; ++ch) {
            int savedPath  = m_config->loadChannelPath(ch);
            int savedRange = m_config->loadChannelRange(ch);
            bool savedTerm = m_config->loadChannelTermination(ch);
            bool savedCoup = m_config->loadChannelCoupling(ch);
            bool savedDiff = m_config->loadChannelDiffMode(ch);
            bool savedBW   = m_config->loadChannelBandwidth(ch);
            int savedOffs  = m_config->loadChannelOffset(ch);

            int col = 0;
            auto *enItem = new QTableWidgetItem();
            enItem->setFlags(enItem->flags() | Qt::ItemIsUserCheckable);
            enItem->setCheckState((savedMask & (1ULL << ch)) ? Qt::Checked : Qt::Unchecked);
            table->setItem(ch, col++, enItem);

            QComboBox *pathCombo = nullptr;
            if (nPaths > 1) {
                pathCombo = new QComboBox();
                pathCombo->addItems(m_fifo->isM3i()
                    ? QStringList{tr("Buffered (1M/50Ω)"), tr("HF 50Ω")}
                    : QStringList{tr("Path 0"), tr("Path 1")});
                pathCombo->setCurrentIndex(savedPath);
                table->setCellWidget(ch, col++, pathCombo);
            }

            auto *rangeCombo = new QComboBox();
            int colRange = col++;
            auto fillRanges = [&pathRanges, rangeCombo, savedRange](int pathIdx) {
                rangeCombo->clear();
                if (pathIdx >= 0 && pathIdx < pathRanges.size())
                    for (auto &r : pathRanges[pathIdx])
                        rangeCombo->addItem(QString::fromUtf8("±%1 mV").arg(r.maxMv), r.maxMv);
                int idx = rangeCombo->findData(savedRange);
                if (idx >= 0) rangeCombo->setCurrentIndex(idx);
                else if (rangeCombo->count() > 2) rangeCombo->setCurrentIndex(2);
            };
            fillRanges(savedPath);
            table->setCellWidget(ch, colRange, rangeCombo);

            if (pathCombo) {
                QObject::connect(pathCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                                 &dlg, [fillRanges](int idx) { fillRanges(idx); });
            }

            if (hasTerm) {
                auto *cb = new QComboBox(); cb->addItems({"1 MΩ", "50 Ω"});
                cb->setCurrentIndex(savedTerm ? 1 : 0);
                table->setCellWidget(ch, col++, cb);
            }
            if (hasCoupling) {
                auto *cb = new QComboBox(); cb->addItems({"DC", "AC"});
                cb->setCurrentIndex(savedCoup ? 1 : 0);
                table->setCellWidget(ch, col++, cb);
            }
            if (hasDiff) {
                auto *cb = new QComboBox(); cb->addItems({"SE", "Diff"});
                cb->setCurrentIndex(savedDiff ? 1 : 0);
                table->setCellWidget(ch, col++, cb);
            }
            if (hasBW) {
                auto *cb = new QComboBox(); cb->addItems({"Full", "20 MHz"});
                cb->setCurrentIndex(savedBW ? 1 : 0);
                table->setCellWidget(ch, col++, cb);
            }
            if (hasOffs) {
                auto *sp = new QSpinBox(); sp->setRange(-5000, 5000); sp->setValue(savedOffs);
                sp->setSuffix(" mV");
                table->setCellWidget(ch, col++, sp);
            }
        }
        table->resizeColumnsToContents();
        lay->addWidget(table);
        stack->addWidget(page);
    }

    // ---- Page 1: Trigger ----
    {
        auto *page = new QWidget();
        auto *lay  = new QFormLayout(page);

        struct SrcEntry { int32_t maskBit; int extLine; QString label; };
        QVector<SrcEntry> srcEntries;
        if (availTrigSrc & SPC_TMASK_EXT0)    srcEntries.append({SPC_TMASK_EXT0,    0,  tr("Ext0 模拟触发 (Trg 口)")});
        if (availTrigSrc & SPC_TMASK_XIO0)    srcEntries.append({SPC_TMASK_XIO0,    1,  tr("X0 TTL 触发 (TTL 电平)")});
        if (availTrigSrc & SPC_TMASK_SOFTWARE) srcEntries.append({SPC_TMASK_SOFTWARE, -1, tr("软件触发 (Software)")});
        for (int ch = 0; ch < std::min(4, nChannels); ++ch) {
            if (availChannelTrig & (SPC_TMASK0_CH0 << ch))
                srcEntries.append({static_cast<int32_t>(SPC_TMASK0_CH0 << ch), 10+ch, tr("通道 %1 触发").arg(ch)});
        }

        auto *srcCombo = new QComboBox(); srcCombo->setObjectName("trigSrc");
        int savedExtLine = m_config->loadTriggerExtLine();
        int savedSrcIdx = 0;
        for (int i = 0; i < srcEntries.size(); ++i) {
            srcCombo->addItem(srcEntries[i].label);
            if (srcEntries[i].extLine == savedExtLine) savedSrcIdx = i;
        }
        srcCombo->setCurrentIndex(savedSrcIdx);
        lay->addRow(tr("触发源:"), srcCombo);

        static const int modeVals[8] = {SPC_TM_POS, SPC_TM_NEG, SPC_TM_HIGH, SPC_TM_LOW,
                                         SPC_TM_WINENTER, SPC_TM_WINLEAVE, SPC_TM_INWIN, SPC_TM_OUTSIDEWIN};
        auto *modeCombo = new QComboBox();
        modeCombo->addItems({tr("上升沿 (Pos Edge)"), tr("下降沿 (Neg Edge)"),
                             tr("高电平 (High Level)"), tr("低电平 (Low Level)"),
                             tr("进入窗口 (Enter Window)"), tr("离开窗口 (Leave Window)"),
                             tr("窗内 (In Window)"), tr("窗外 (Out Window)")});
        int savedMode = m_config->loadTriggerMode();
        for (int i = 0; i < 8; ++i)
            if (savedMode == modeVals[i]) { modeCombo->setCurrentIndex(i); break; }
        modeCombo->setObjectName("trigMode");
        lay->addRow(tr("触发模式:"), modeCombo);

        auto *level0Spin = new QSpinBox(); level0Spin->setObjectName("trigLevel0");
        level0Spin->setRange(-5000, 5000); level0Spin->setSuffix(" mV");
        level0Spin->setValue(m_config->loadTriggerLevel0());
        lay->addRow(tr("触发电平 0:"), level0Spin);

        auto *level1Spin = new QSpinBox(); level1Spin->setObjectName("trigLevel1");
        level1Spin->setRange(-5000, 5000); level1Spin->setSuffix(" mV");
        level1Spin->setValue(m_config->loadTriggerLevel1());
        lay->addRow(tr("触发电平 1:"), level1Spin);

        auto *couplingCombo = new QComboBox(); couplingCombo->setObjectName("trigCoupling");
        couplingCombo->addItems({tr("DC 耦合"), tr("AC 耦合")});
        couplingCombo->setCurrentIndex(m_config->loadTriggerCoupling() ? 1 : 0);
        lay->addRow(tr("触发耦合:"), couplingCombo);

        auto *termCheck = new QCheckBox(tr("50 Ω 终端电阻")); termCheck->setObjectName("trigTerm");
        termCheck->setChecked(m_config->loadTriggerTermination());
        lay->addRow(tr("终端:"), termCheck);

        auto *delaySpin = new QSpinBox(); delaySpin->setObjectName("trigDelay");
        delaySpin->setRange(0, 65536); delaySpin->setSingleStep(16); delaySpin->setSuffix(" samples");
        delaySpin->setValue(m_config->loadTriggerDelay());
        lay->addRow(tr("触发延迟:"), delaySpin);

        QVector<int> trigExtLines;
        for (auto &e : srcEntries) trigExtLines.append(e.extLine);
        auto updateTrigFields = [trigExtLines, modeCombo, level0Spin, level1Spin,
                                 couplingCombo, termCheck](int idx) {
            if (idx < 0 || idx >= trigExtLines.size()) return;
            int extLine = trigExtLines[idx];
            const bool isExt0 = (extLine == 0);
            const bool isSoftware = (extLine == -1);
            const bool isChannel = (extLine >= 10);
            modeCombo->setEnabled(!isSoftware);
            level0Spin->setEnabled(isExt0 || isChannel);
            level1Spin->setEnabled(isExt0 || isChannel);
            couplingCombo->setEnabled(isExt0);
            termCheck->setEnabled(isExt0);
        };
        QObject::connect(srcCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, updateTrigFields);
        updateTrigFields(savedSrcIdx);
        stack->addWidget(page);
    }

    // ---- Page 2: Clock ----
    {
        auto *page = new QWidget();
        auto *lay  = new QFormLayout(page);

        struct ClkEntry { int32_t mode; QString label; };
        QVector<ClkEntry> clkEntries;
        if (availClockModes & SPC_CM_INTPLL)      clkEntries.append({SPC_CM_INTPLL,      tr("内部 PLL (Internal PLL)")});
        if (availClockModes & SPC_CM_EXTERNAL)    clkEntries.append({SPC_CM_EXTERNAL,    tr("外部时钟 (External)")});
        if (availClockModes & SPC_CM_EXTREFCLOCK) clkEntries.append({SPC_CM_EXTREFCLOCK, tr("参考时钟 (Reference Clock)")});

        auto *modeCombo = new QComboBox(); modeCombo->setObjectName("clockMode");
        int savedClk = m_config->loadClockMode();
        int clkIdx = 0;
        for (int i = 0; i < clkEntries.size(); ++i) {
            modeCombo->addItem(clkEntries[i].label);
            if (clkEntries[i].mode == savedClk) clkIdx = i;
        }
        modeCombo->setCurrentIndex(clkIdx);
        lay->addRow(tr("时钟模式:"), modeCombo);

        int maxMsps = m_fifo->isOpen() ? static_cast<int>(m_fifo->cardInfo().maxSamplerate / 1000000) : 500;
        auto *rateSpin = new QSpinBox(); rateSpin->setObjectName("clockRate");
        rateSpin->setRange(1, maxMsps); rateSpin->setSuffix(" MS/s");
        rateSpin->setValue(m_config->loadSampleRate() / 1000000);
        rateSpin->setKeyboardTracking(false);
        lay->addRow(tr("采样率:"), rateSpin);

        auto *refSpin = new QSpinBox(); refSpin->setObjectName("clockRef");
        refSpin->setRange(1, 1000); refSpin->setSuffix(" MHz");
        refSpin->setValue(m_config->loadRefClockFreq());
        lay->addRow(tr("参考时钟频率:"), refSpin);
        auto *labelRef = lay->labelForField(refSpin);

        auto *divSpin = new QSpinBox(); divSpin->setObjectName("clockDiv");
        divSpin->setRange(1, 256);
        divSpin->setValue(m_config->loadClockDivider());
        lay->addRow(tr("外时钟分频:"), divSpin);
        auto *labelDiv = lay->labelForField(divSpin);

        auto *clkOutChk = new QCheckBox(tr("时钟输出")); clkOutChk->setObjectName("clockOut");
        clkOutChk->setChecked(m_config->loadClockOutput());
        lay->addRow(tr("输出:"), clkOutChk);

        QVector<int32_t> clkModeVals;
        for (auto &e : clkEntries) clkModeVals.append(e.mode);
        auto updateClkFields = [clkModeVals, rateSpin, refSpin, labelRef,
                                divSpin, labelDiv, clkOutChk](int idx) {
            if (idx < 0 || idx >= clkModeVals.size()) return;
            int32_t mode = clkModeVals[idx];
            bool isPll = (mode == SPC_CM_INTPLL);
            bool isExt = (mode == SPC_CM_EXTERNAL);
            bool isRef = (mode == SPC_CM_EXTREFCLOCK);
            rateSpin->setEnabled(isPll || isRef);
            refSpin->setEnabled(isRef); labelRef->setEnabled(isRef);
            divSpin->setEnabled(isExt); labelDiv->setEnabled(isExt);
            clkOutChk->setEnabled(isPll);
        };
        QObject::connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, updateClkFields);
        updateClkFields(clkIdx);
        stack->addWidget(page);
    }

    // ---- Page 3: Acquisition Mode ----
    {
        auto *page = new QWidget();
        auto *lay  = new QFormLayout(page);

        auto *modeCombo = new QComboBox(); modeCombo->setObjectName("fifoMode");
        modeCombo->addItems({tr("FIFO Singleshot"), tr("FIFO Multiple Recording"),
                             tr("FIFO Gated Sampling"), tr("FIFO ABA")});
        int modeVals[4] = {SPC_REC_FIFO_SINGLE, SPC_REC_FIFO_MULTI, SPC_REC_FIFO_GATE, SPC_REC_FIFO_ABA};
        int modeIdx = 1;
        for (int i = 0; i < 4; ++i)
            if (m_scanParams.fifoMode == modeVals[i]) { modeIdx = i; break; }
        modeCombo->setCurrentIndex(modeIdx);
        lay->addRow(tr("FIFO 模式:"), modeCombo);

        lay->addRow(new QLabel(tr("<b>每触发硬件采集参数</b>")));

        auto *segSpin = new QSpinBox(); segSpin->setObjectName("segSize");
        segSpin->setRange(64, 2097152); segSpin->setSingleStep(256);
        segSpin->setValue(m_scanParams.segmentSize);
        lay->addRow(tr("段大小 Seg (samples):"), segSpin);

        auto *preSpin = new QSpinBox(); preSpin->setObjectName("preTrig");
        preSpin->setRange(8, 65536);
        preSpin->setValue(m_scanParams.preTrigger);
        lay->addRow(tr("预触发 Pre (samples):"), preSpin);

        auto *postLabel = new QLabel();
        auto updatePost = [segSpin, preSpin, postLabel]() {
            postLabel->setText(QString::number(segSpin->value() - preSpin->value()));
        };
        updatePost();
        QObject::connect(segSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dlg, [updatePost](int) { updatePost(); });
        QObject::connect(preSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dlg, [updatePost](int) { updatePost(); });
        lay->addRow(tr("后触发 Post (=Seg-Pre):"), postLabel);

        auto *loopSpin = new QSpinBox(); loopSpin->setObjectName("loopCount");
        loopSpin->setRange(0, 1000000);
        loopSpin->setValue(m_scanParams.loopCount);
        loopSpin->setSpecialValueText(tr("0 (无限)"));
        lay->addRow(tr("循环次数 Loop:"), loopSpin);

        lay->addRow(new QLabel(tr("<b>软件信号窗口提取</b>")));

        auto *winOffSpin = new QSpinBox(); winOffSpin->setObjectName("winOffset");
        winOffSpin->setRange(0, 65535);
        winOffSpin->setValue(m_scanParams.windowOffset);
        lay->addRow(tr("窗口起始偏移 (samples):"), winOffSpin);

        auto *winSizeSpin = new QSpinBox(); winSizeSpin->setObjectName("winSize");
        winSizeSpin->setRange(1, 65535);
        winSizeSpin->setValue(m_scanParams.depth);
        lay->addRow(tr("窗口宽度 depth (samples):"), winSizeSpin);

        lay->addRow(new QLabel(tr("<b>DMA 缓冲设置</b>")));

        auto *bufSpin = new QSpinBox(); bufSpin->setObjectName("bufSizeMB");
        bufSpin->setRange(4, 4096); bufSpin->setSingleStep(64);
        bufSpin->setSuffix(" MB");
        bufSpin->setValue(m_config->loadBufferSizeMB());
        lay->addRow(tr("缓冲区大小:"), bufSpin);

        auto *notifySpin = new QSpinBox(); notifySpin->setObjectName("notifySizeKB");
        notifySpin->setRange(64, 65536); notifySpin->setSingleStep(256);
        notifySpin->setSuffix(" KB");
        notifySpin->setValue(m_config->loadNotifySizeKB());
        lay->addRow(tr("通知粒度:"), notifySpin);

        auto syncAcquisitionRanges = [segSpin, preSpin, winOffSpin,
                                      winSizeSpin, bufSpin, notifySpin]() {
            const int segment = segSpin->value();
            preSpin->setMaximum(std::max(8, std::min(65536, segment - 1)));
            winOffSpin->setMaximum(std::max(0, std::min(65535, segment - 1)));
            winSizeSpin->setMaximum(
                std::max(1, std::min(65535, segment - winOffSpin->value())));
            notifySpin->setMaximum(
                std::max(64, std::min(65536, bufSpin->value() * 1024)));
        };
        syncAcquisitionRanges();
        QObject::connect(segSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                         &dlg, [syncAcquisitionRanges](int) { syncAcquisitionRanges(); });
        QObject::connect(winOffSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                         &dlg, [syncAcquisitionRanges](int) { syncAcquisitionRanges(); });
        QObject::connect(bufSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                         &dlg, [syncAcquisitionRanges](int) { syncAcquisitionRanges(); });

        auto *chunkLabel = new QLabel();
        auto updateDmaPreview = [this, bufSpin, notifySpin, chunkLabel]() {
            int bufMB = bufSpin->value();
            int notKB = notifySpin->value();
            if (notKB <= 0 || bufMB <= 0) { chunkLabel->setText(tr("—")); return; }
            int chunks = (bufMB * 1024) / notKB;
            int rateHz = m_config->loadSampleRate();
            if (rateHz <= 0) rateHz = 250000000;
            double notifyMs = (notKB * 1024.0) / (static_cast<double>(rateHz) * 2.0) * 1000.0;
            chunkLabel->setText(tr("%1 个 chunk, 唤醒间隔 ≈ %2 ms").arg(chunks).arg(notifyMs, 0, 'f', 2));
        };
        updateDmaPreview();
        QObject::connect(bufSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dlg, [updateDmaPreview](int) { updateDmaPreview(); });
        QObject::connect(notifySpin, QOverload<int>::of(&QSpinBox::valueChanged), &dlg, [updateDmaPreview](int) { updateDmaPreview(); });
        lay->addRow(tr(""), chunkLabel);

        lay->addRow(new QLabel(tr("<b>存储数据格式 / PCD 分割</b>")));

        auto *fmtCb = new QComboBox(); fmtCb->setObjectName("dataFormat");
        fmtCb->addItem("float64", static_cast<int>(DataFormat::Float64));
        fmtCb->addItem("float32", static_cast<int>(DataFormat::Float32));
        fmtCb->addItem("float16", static_cast<int>(DataFormat::Float16));
        int curFmt = fmtCb->findData(static_cast<int>(m_scanParams.dataFormat));
        fmtCb->setCurrentIndex(curFmt >= 0 ? curFmt : 0);   // 0 = float64 default
        lay->addRow(tr("数据格式 (.dat/.npy):"), fmtCb);

        auto *pcdModeCb = new QComboBox(); pcdModeCb->setObjectName("pcdSegMode");
        pcdModeCb->addItem(tr("关闭"), static_cast<int>(PcdSegMode::Off));
        pcdModeCb->addItem(tr("Otsu 自动阈值"), static_cast<int>(PcdSegMode::Otsu));
        pcdModeCb->addItem(tr("手动阈值"), static_cast<int>(PcdSegMode::Manual));
        int curSeg = pcdModeCb->findData(static_cast<int>(m_scanParams.pcdSegMode));
        pcdModeCb->setCurrentIndex(curSeg >= 0 ? curSeg : 1);
        lay->addRow(tr("PCD 分割 (轨迹):"), pcdModeCb);

        auto *pcdThreshSp = new QDoubleSpinBox(); pcdThreshSp->setObjectName("pcdSegThresh");
        pcdThreshSp->setRange(0.0, 1.0);
        pcdThreshSp->setSingleStep(0.01);
        pcdThreshSp->setDecimals(2);
        pcdThreshSp->setValue(m_scanParams.pcdSegThreshold);
        lay->addRow(tr("PCD 阈值 (×峰值):"), pcdThreshSp);

        // The threshold value only matters in Manual mode; disable it for
        // Otsu (auto) and Off so the UI reflects the actual behavior.
        const auto updatePcdThreshEnabled = [pcdModeCb, pcdThreshSp]() {
            const auto mode = static_cast<PcdSegMode>(pcdModeCb->currentData().toInt());
            pcdThreshSp->setEnabled(mode == PcdSegMode::Manual);
        };
        QObject::connect(pcdModeCb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                         pcdThreshSp, [updatePcdThreshEnabled](int) { updatePcdThreshEnabled(); });
        updatePcdThreshEnabled();

        stack->addWidget(page);
    }

    outerLayout->addLayout(bodyRow);
    QObject::connect(list, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto *btnApply  = new QPushButton(tr("应用"));
    auto *btnCancel = new QPushButton(tr("取消"));
    btnLayout->addWidget(btnApply);
    btnLayout->addWidget(btnCancel);
    outerLayout->addLayout(btnLayout);

    QObject::connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(btnApply,  &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) return;

    // Apply Page 0 — Channels
    {
        auto *table = stack->widget(0)->findChild<QTableWidget*>();
        if (table) {
            uint64_t mask = 0;
            for (int ch = 0; ch < nChannels && ch < table->rowCount(); ++ch) {
                bool en = table->item(ch, 0)->checkState() == Qt::Checked;
                if (en) mask |= (1ULL << ch);
                int col = 1, path = 0, rangeMv = 1000, offsetMv = 0;
                bool term = false, coup = false, diff = false, bw = false;
                if (nPaths > 1) {
                    auto *pc = qobject_cast<QComboBox*>(table->cellWidget(ch, col));
                    if (pc) path = pc->currentIndex();
                    ++col;
                }
                auto *rc = qobject_cast<QComboBox*>(table->cellWidget(ch, col++));
                if (rc) rangeMv = rc->currentData().toInt();
                if (hasTerm)     { auto *w = qobject_cast<QComboBox*>(table->cellWidget(ch, col++)); term = w && w->currentIndex()==1; }
                if (hasCoupling) { auto *w = qobject_cast<QComboBox*>(table->cellWidget(ch, col++)); coup = w && w->currentIndex()==1; }
                if (hasDiff)     { auto *w = qobject_cast<QComboBox*>(table->cellWidget(ch, col++)); diff = w && w->currentIndex()==1; }
                if (hasBW)       { auto *w = qobject_cast<QComboBox*>(table->cellWidget(ch, col++)); bw   = w && w->currentIndex()==1; }
                if (hasOffs)     { auto *w = qobject_cast<QSpinBox*>(table->cellWidget(ch, col++)); if (w) offsetMv = w->value(); }
                m_config->saveChannelPath(ch, path);
                m_config->saveChannelRange(ch, rangeMv);
                if (hasTerm)     m_config->saveChannelTermination(ch, term);
                if (hasCoupling) m_config->saveChannelCoupling(ch, coup);
                if (hasDiff)     m_config->saveChannelDiffMode(ch, diff);
                if (hasBW)       m_config->saveChannelBandwidth(ch, bw);
                if (hasOffs)     m_config->saveChannelOffset(ch, offsetMv);
                if (m_fifo->isOpen()) {
                    m_fifo->setInputPath(ch, path);
                    m_fifo->setInputRange(ch, rangeMv);
                    if (hasTerm)     m_fifo->setTermination(ch, term);
                    if (hasCoupling) m_fifo->setCoupling(ch, coup);
                    if (hasDiff)     m_fifo->setDiffMode(ch, diff);
                    if (hasBW)       m_fifo->setBandwidthLimit(ch, bw);
                    if (hasOffs)     m_fifo->setOffset(ch, offsetMv);
                }
            }
            m_scanParams.channelMask = mask;
            m_config->saveChannelMask(mask);
        }
    }

    // Apply Page 1 — Trigger
    {
        auto *srcCb = stack->widget(1)->findChild<QComboBox*>("trigSrc");
        auto *modeCb = stack->widget(1)->findChild<QComboBox*>("trigMode");
        auto *coupCb = stack->widget(1)->findChild<QComboBox*>("trigCoupling");
        auto *lvl0Sp = stack->widget(1)->findChild<QSpinBox*>("trigLevel0");
        auto *lvl1Sp = stack->widget(1)->findChild<QSpinBox*>("trigLevel1");
        auto *dlySp  = stack->widget(1)->findChild<QSpinBox*>("trigDelay");
        auto *termCk = stack->widget(1)->findChild<QCheckBox*>("trigTerm");
        if (srcCb && modeCb && lvl0Sp && lvl1Sp && dlySp) {
            struct SrcEntry { int32_t maskBit; int extLine; };
            QVector<SrcEntry> se;
            if (availTrigSrc & SPC_TMASK_EXT0)     se.append({SPC_TMASK_EXT0, 0});
            if (availTrigSrc & SPC_TMASK_XIO0)     se.append({SPC_TMASK_XIO0, 1});
            if (availTrigSrc & SPC_TMASK_SOFTWARE) se.append({SPC_TMASK_SOFTWARE, -1});
            for (int ch = 0; ch < std::min(4, nChannels); ++ch)
                if (availChannelTrig & (SPC_TMASK0_CH0 << ch))
                    se.append({static_cast<int32_t>(SPC_TMASK0_CH0 << ch), 10+ch});
            static const int modeVals[8] = {SPC_TM_POS, SPC_TM_NEG, SPC_TM_HIGH, SPC_TM_LOW, SPC_TM_WINENTER, SPC_TM_WINLEAVE, SPC_TM_INWIN, SPC_TM_OUTSIDEWIN};
            int srcIdx  = srcCb->currentIndex();
            int extLine = srcIdx < se.size() ? se[srcIdx].extLine : 0;
            int mode    = modeVals[modeCb->currentIndex()];
            m_config->saveTriggerMode(mode);
            m_config->saveTriggerExtLine(extLine);
            m_config->saveTriggerLevel0(lvl0Sp->value());
            m_config->saveTriggerLevel1(lvl1Sp->value());
            m_config->saveTriggerCoupling(coupCb && coupCb->currentIndex() == 1);
            m_config->saveTriggerDelay(dlySp->value());
            m_config->saveTriggerTermination(termCk ? termCk->isChecked() : true);
            if (m_fifo->isOpen()) {
                m_fifo->setExternalTrigger(mode, extLine,
                                           termCk ? termCk->isChecked() : true);
                if (extLine != -1) {
                    m_fifo->setTriggerLevel(extLine, lvl0Sp->value(), lvl1Sp->value());
                }
                m_fifo->setTriggerCoupling(extLine,
                                           coupCb && coupCb->currentIndex() == 1);
                m_fifo->setTriggerDelay(dlySp->value());
            }
        }
    }

    // Apply Page 2 — Clock
    {
        auto *modeCb = stack->widget(2)->findChild<QComboBox*>("clockMode");
        auto *rateSp = stack->widget(2)->findChild<QSpinBox*>("clockRate");
        auto *refSp  = stack->widget(2)->findChild<QSpinBox*>("clockRef");
        auto *divSp  = stack->widget(2)->findChild<QSpinBox*>("clockDiv");
        auto *outCk  = stack->widget(2)->findChild<QCheckBox*>("clockOut");
        if (modeCb && rateSp && refSp && divSp) {
            struct ClkEntry { int32_t mode; };
            QVector<ClkEntry> ce;
            if (availClockModes & SPC_CM_INTPLL)      ce.append(ClkEntry{SPC_CM_INTPLL});
            if (availClockModes & SPC_CM_EXTERNAL)    ce.append(ClkEntry{SPC_CM_EXTERNAL});
            if (availClockModes & SPC_CM_EXTREFCLOCK) ce.append(ClkEntry{SPC_CM_EXTREFCLOCK});
            if (!ce.isEmpty()) {
                int32_t selMode = ce[modeCb->currentIndex()].mode;
                m_config->saveClockMode(selMode);
                m_config->saveSampleRate(rateSp->value() * 1000000);
                m_config->saveClockDivider(divSp->value());
                m_config->saveRefClockFreq(refSp->value());
                m_config->saveClockOutput(outCk && outCk->isChecked());
                if (m_fifo->isOpen()) {
                    if (selMode == SPC_CM_EXTERNAL)
                        m_fifo->setExternalClock(divSp->value());
                    else if (selMode == SPC_CM_INTPLL)
                        m_fifo->setInternalPllClock(rateSp->value() * 1000000, outCk && outCk->isChecked());
                    else if (selMode == SPC_CM_EXTREFCLOCK)
                        m_fifo->setReferenceClock(refSp->value() * 1000000LL,
                                                  rateSp->value() * 1000000LL);
                }
            }
        }
    }

    // Apply Page 3 — Mode
    {
        auto *fifoCb  = stack->widget(3)->findChild<QComboBox*>("fifoMode");
        auto *segSp   = stack->widget(3)->findChild<QSpinBox*>("segSize");
        auto *preSp   = stack->widget(3)->findChild<QSpinBox*>("preTrig");
        auto *loopSp  = stack->widget(3)->findChild<QSpinBox*>("loopCount");
        auto *woffSp  = stack->widget(3)->findChild<QSpinBox*>("winOffset");
        auto *wsizeSp = stack->widget(3)->findChild<QSpinBox*>("winSize");
        auto *bufSp   = stack->widget(3)->findChild<QSpinBox*>("bufSizeMB");
        auto *notSp   = stack->widget(3)->findChild<QSpinBox*>("notifySizeKB");
        auto *fmtCb   = stack->widget(3)->findChild<QComboBox*>("dataFormat");
        auto *segCb   = stack->widget(3)->findChild<QComboBox*>("pcdSegMode");
        auto *pcdThSp = stack->widget(3)->findChild<QDoubleSpinBox*>("pcdSegThresh");
        if (fifoCb && segSp && preSp && loopSp && woffSp && wsizeSp && bufSp && notSp) {
            static const int mVals[4] = {SPC_REC_FIFO_SINGLE, SPC_REC_FIFO_MULTI, SPC_REC_FIFO_GATE, SPC_REC_FIFO_ABA};
            ScanParams candidate = m_scanParams;
            candidate.fifoMode     = mVals[fifoCb->currentIndex()];
            candidate.segmentSize  = segSp->value();
            candidate.preTrigger   = preSp->value();
            candidate.loopCount    = loopSp->value();
            candidate.windowOffset = woffSp->value();
            candidate.depth        = wsizeSp->value();
            if (fmtCb)
                candidate.dataFormat = static_cast<DataFormat>(fmtCb->currentData().toInt());
            if (segCb)
                candidate.pcdSegMode = static_cast<PcdSegMode>(segCb->currentData().toInt());
            if (pcdThSp)
                candidate.pcdSegThreshold = static_cast<float>(pcdThSp->value());

            const int newBufMB = bufSp->value();
            const int newNotKB = notSp->value();
            const int bytesPerSample = m_fifo->isOpen()
                ? std::max(1, m_fifo->cardInfo().bytesPerSample)
                : static_cast<int>(sizeof(int16_t));
            const int64_t segmentBytes =
                static_cast<int64_t>(candidate.segmentSize) * bytesPerSample;
            const int64_t requestedBuffer =
                static_cast<int64_t>(newBufMB) * 1024 * 1024;
            const int64_t alignedBuffer =
                (requestedBuffer / segmentBytes) * segmentBytes;
            const int64_t notifyBytes = static_cast<int64_t>(newNotKB) * 1024;
            if (candidate.preTrigger < 0 ||
                candidate.preTrigger >= candidate.segmentSize ||
                candidate.windowOffset < 0 || candidate.depth <= 0 ||
                static_cast<int64_t>(candidate.windowOffset) + candidate.depth >
                    candidate.segmentSize ||
                alignedBuffer < segmentBytes || notifyBytes <= 0 ||
                notifyBytes > alignedBuffer) {
                showError(tr("参数无效"),
                          tr("请检查段长、预触发、信号窗口以及 DMA 通知/缓冲区大小。"));
                return;
            }

            const int oldBufMB = m_config->loadBufferSizeMB();
            const int oldNotKB = m_config->loadNotifySizeKB();
            m_scanParams = candidate;
            m_config->saveScanParams(m_scanParams);
            m_config->saveBufferSizeMB(newBufMB);
            m_config->saveNotifySizeKB(newNotKB);
            ui->spinDepth->setValue(m_scanParams.depth);
            ui->spinWinOffset->setValue(m_scanParams.windowOffset);
            ui->bscanWidget->setDimensions(m_scanParams.depth, m_scanParams.nFast);
            if (m_bscanDepthRuler) m_bscanDepthRuler->setDepth(m_scanParams.depth);
            if (m_scanParams.isRaster())
                ui->cscanWidget->setDimensions(m_scanParams.nSlow, m_scanParams.nFast);
            else
                ui->cscanWidget->setDimensions(m_scanParams.imageWidth, m_scanParams.imageHeight);
            if (m_pipeline) {
                m_pipeline->setLiveDepth(m_scanParams.depth);
                m_pipeline->setLiveWindowOffset(m_scanParams.windowOffset);
            }
            if (m_fifo->isOpen()) {
                m_fifo->setupFifoMode(m_scanParams.fifoMode, m_config->loadChannelMask(),
                                      m_scanParams.preTrigger, m_scanParams.segmentSize,
                                      m_scanParams.loopCount);
                m_fifo->redefineTransfer();  // DMA must be re-defined after segment change
            }
            if (m_fifo->isOpen() && (newBufMB != oldBufMB || newNotKB != oldNotKB)) {
                m_fifo->freeBuffer();
                if (!m_fifo->allocateBuffer(
                        alignedBuffer, static_cast<int32_t>(notifyBytes))) {
                    showError(tr("DMA 缓冲区配置失败"), m_fifo->lastErrorText());
                }
            }
        }
    }
}

void MainWindow::onSerialDebugAssistant()
{
    if (!m_serialDebug) {
        m_serialDebug = new SerialDebugDialog(this);
        connect(m_serialDebug, &SerialDebugDialog::portStateChanged, this, [this] {
            if (!m_serialDebug || m_scanCtrl->isRunning())
                return;
            if (m_serialDebug->isPortOpen()) {
                if (!m_fpga->isOpen()) {
                    ui->labelSerialStatus->setText(
                        tr("调试助手占用 %1").arg(m_serialDebug->portName()));
                    ui->labelSerialStatus->setStyleSheet("color: #b26a00;");
                    ui->labelSerialStatus->setToolTip(
                        tr("串口调试助手正在使用该端口；关闭调试串口后会自动恢复采集卡串口。"));
                }
                return;
            }
            // The debug port was released. Selecting the same entry in the
            // acquisition combo does not emit a signal, so restore it here.
            if (!m_fpga->isOpen() && ui->comboSerialPort->currentIndex() >= 0) {
                ui->labelSerialStatus->setText(tr("未打开"));
                ui->labelSerialStatus->setStyleSheet("color: orange;");
                onSerialPortChanged(ui->comboSerialPort->currentIndex());
            }
        });
        connect(m_serialDebug, &SerialDebugDialog::acquisitionPortReleaseRequested,
                this, [this] {
            if (!m_serialDebug)
                return;
            // Never release the serial line while a scan or the worker is
            // running: FPGA start/stop commands would have nowhere to go.
            if (m_scanCtrl->isRunning() || m_workerThread->isRunning()) {
                QMessageBox::warning(this, tr("扫描进行中"),
                                     tr("请先停止扫描，再释放采集卡串口。"));
                return;
            }
            if (m_fpga->isOpen()) {
                const QString port = m_fpga->portName();
                // Stop a manually free-running FPGA before the tty changes
                // owner; otherwise it would keep acquiring with nobody able to
                // send F0 until the port is restored.
                m_fpga->sendStop();
                m_fpga->closePort();
                qInfo() << "Serial debug assistant released acquisition port" << port;
            }
        });
    }
    m_serialDebug->setAcquisitionPort(m_fpga->isOpen() ? m_fpga->portName() : QString());
    m_serialDebug->show();
    m_serialDebug->raise();
    m_serialDebug->activateWindow();
}

void MainWindow::onAbout()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("关于 光声显微镜统一采集软件"));
    auto *layout = new QVBoxLayout(&dlg);
    auto *label = new QLabel(tr("光声显微镜数据采集控制软件\n"
                                "PA Microscope\n\n"
                                "支持光栅扫描与自定义轨迹扫描\n"
                                "支持 Spectrum M3i / M4i 系列采集卡\n"
                                "FIFO Multiple Recording 模式\n"
                                "Qt 6 / C++17\n\n"
                                "Developed by PA Team SCNU"));
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    auto *btn = new QPushButton(tr("确定"));
    connect(btn, &QPushButton::clicked, &dlg, &QDialog::accept);
    layout->addWidget(btn, 0, Qt::AlignCenter);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    dlg.exec();
}

void MainWindow::onDisplaySettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("显示设置"));
    auto *layout = new QFormLayout(&dlg);

    auto *mappingCombo = new QComboBox(&dlg);
    mappingCombo->addItem(tr("线性 (Linear)"), 0);
    mappingCombo->addItem(tr("对数 (Log, 40 dB)"), 1);
    mappingCombo->setCurrentIndex(m_config->loadColorMapping());
    layout->addRow(tr("颜色映射:"), mappingCombo);

    auto *colormapCombo = new QComboBox(&dlg);
    colormapCombo->addItem(tr("热力 (Hot)"), 0);
    colormapCombo->addItem(tr("灰度 (Grayscale)"), 1);
    colormapCombo->setCurrentIndex(m_config->loadColorMap());
    layout->addRow(tr("配色:"), colormapCombo);

    auto *clipCombo = new QComboBox(&dlg);
    clipCombo->addItem(tr("削顶 (1.1x headroom)"), true);
    clipCombo->addItem(tr("不削顶 (max → white)"), false);
    clipCombo->setCurrentIndex(m_config->loadColorClipping() ? 0 : 1);
    layout->addRow(tr("映射削顶:"), clipCombo);

    // ---- Depth encoding ----
    auto *depthSep = new QLabel(tr("深度编码"));
    depthSep->setStyleSheet("font-weight: bold; margin-top: 6px;");
    layout->addRow(depthSep);

    auto *depthCheck = new QCheckBox(tr("启用（C-scan 按深度上色）"), &dlg);
    depthCheck->setChecked(m_depthEncode);
    depthCheck->setToolTip(tr("颜色表示每条 A-line 包络峰值所在的深度；\n"
                              "关掉后 C-scan 恢复为强度图，也不再计算深度。"));
    layout->addRow(depthCheck);

    auto *depthModeCombo = new QComboBox(&dlg);
    depthModeCombo->addItem(tr("深度色调 + 强度亮度"), 1);
    depthModeCombo->addItem(tr("纯深度色"), 0);
    depthModeCombo->setCurrentIndex(m_config->loadDepthDisplayMode() == 0 ? 1 : 0);
    layout->addRow(tr("显示方式:"), depthModeCombo);

    auto *depthMapCombo = new QComboBox(&dlg);
    depthMapCombo->addItem(tr("Turbo（彩虹，对比最强）"), 0);
    depthMapCombo->addItem(tr("Viridis（感知均匀·色盲友好·推荐）"), 1);
    depthMapCombo->addItem(tr("Plasma（紫→橙黄，深层对比好）"), 2);
    depthMapCombo->addItem(tr("Magma（黑→紫→白）"), 3);
    depthMapCombo->addItem(tr("Inferno（黑→橙→黄白）"), 4);
    depthMapCombo->addItem(tr("Jet（蓝→青→黄→红）"), 5);
    depthMapCombo->addItem(tr("红→青（高对比）"), 6);
    depthMapCombo->setCurrentIndex(std::clamp(m_config->loadDepthColorMap(), 0, 6));
    depthMapCombo->setToolTip(tr("Viridis / Plasma / Magma / Inferno 是感知均匀色标："
                                 "相同的深度差在量程各处看起来一样明显。\n"
                                 "Turbo / Jet 对比更强，但会夸大中间段的差异。\n"
                                 "色值取自各家公开的 256 级色表（CC0）。"));
    layout->addRow(tr("深度配色:"), depthMapCombo);

    auto *depthRangeSpin = new QSpinBox(&dlg);
    depthRangeSpin->setRange(0, 65535);
    depthRangeSpin->setSpecialValueText(tr("整个信号窗口"));
    depthRangeSpin->setValue(m_config->loadDepthMaxIndex());
    depthRangeSpin->setToolTip(tr("色标末端对应的采样点序号（相对信号窗口起点）。\n"
                                  "限制量程可以放大某个深度段的颜色差异。"));
    layout->addRow(tr("深度量程:"), depthRangeSpin);

    auto *depthGateSpin = new QSpinBox(&dlg);
    depthGateSpin->setRange(0, 100);
    depthGateSpin->setSuffix(" %");
    depthGateSpin->setValue(m_config->loadDepthGatePercent());
    depthGateSpin->setToolTip(tr("低于该强度比例的像素视为背景，画成黑色，\n"
                                 "避免噪声被上成随机的深度色。0 = 不过滤。"));
    layout->addRow(tr("背景阈值:"), depthGateSpin);

    auto *depthNote = new QLabel(tr("深度=包络峰值位置，0 在信号窗口起点；\n"
                                    "勾选「PNG 深度」才会保存 depth_*.png。"));
    depthNote->setStyleSheet("color: gray;");
    layout->addRow(depthNote);

    // Grey out the depth controls while depth encoding is off.
    auto syncDepthRows = [depthCheck, depthModeCombo, depthMapCombo,
                          depthRangeSpin, depthGateSpin](bool on) {
        depthModeCombo->setEnabled(on);
        depthMapCombo->setEnabled(on);
        depthRangeSpin->setEnabled(on);
        depthGateSpin->setEnabled(on);
    };
    syncDepthRows(depthCheck->isChecked());
    QObject::connect(depthCheck, &QCheckBox::toggled, &dlg, syncDepthRows);

    auto *noteLabel = new QLabel(tr("修改即时生效。"));
    noteLabel->setStyleSheet("color: gray;");
    layout->addRow(noteLabel);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto *btnOk     = new QPushButton(tr("确定"));
    auto *btnCancel = new QPushButton(tr("取消"));
    btnLayout->addWidget(btnOk);
    btnLayout->addWidget(btnCancel);
    layout->addRow(btnLayout);

    QObject::connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(btnOk,     &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) return;

    int  newMapping  = mappingCombo->currentData().toInt();
    bool newClipping = clipCombo->currentData().toBool();
    int  newColorMap = colormapCombo->currentData().toInt();
    m_config->saveColorMapping(newMapping);
    m_config->saveColorClipping(newClipping);
    m_config->saveColorMap(newColorMap);
    m_config->saveDepthDisplayMode(depthModeCombo->currentData().toInt());
    m_config->saveDepthColorMap(depthMapCombo->currentData().toInt());
    m_config->saveDepthMaxIndex(depthRangeSpin->value());
    m_config->saveDepthGatePercent(depthGateSpin->value());
    m_depthEncode = depthCheck->isChecked();
    m_config->saveDepthEncode(m_depthEncode);
    applyDepthSettings();
    ui->cscanWidget->setColorParams(newMapping, newClipping);
    ui->cscanWidget->setColorMap(newColorMap);
    ui->bscanWidget->setColorParams(newMapping, newClipping);
    ui->bscanWidget->setColorMap(newColorMap);
    if (m_pipeline) {
        m_pipeline->setColorParams(newMapping, newClipping);
        m_pipeline->setColorMap(newColorMap);
    }
    if (m_bscanBar) m_bscanBar->setColorMap(newColorMap);
    if (m_cscanBar) m_cscanBar->setColorMap(newColorMap);

    // Trajectory frames are pre-colored in MainWindow; raster widgets retain
    // their own raw buffers and were recolored by setColor* above.
    if (m_scanParams.isTrajectory()) recolorLastMip();
}

// ===== Helpers =====

void MainWindow::recolorLastMip()
{
    if (m_lastMip.empty()) return;
    int w = m_scanParams.imageWidth;
    int h = m_scanParams.imageHeight;
    int gridSize = w * h;
    if (static_cast<int>(m_lastMip.size()) < gridSize) return;

    int cm = m_config->loadColorMapping();
    bool cc = m_config->loadColorClipping();
    const uint32_t *lut = colorLut(m_config->loadColorMap());

    QImage img(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        uchar *sl = img.scanLine(y);
        int rb = y * w;
        for (int x = 0; x < w; ++x) {
            int ci = mapToIndex(m_lastMip[rb + x], m_lastMipMax, cm, cc);
            uint32_t c = lut[ci];
            sl[x * 3 + 0] = c & 0xFF;
            sl[x * 3 + 1] = (c >> 8) & 0xFF;
            sl[x * 3 + 2] = (c >> 16) & 0xFF;
        }
    }
    ui->cscanWidget->updateMipImage(img, m_lastMipMax);
}

void MainWindow::updateButtonStates(ScanState state)
{
    bool isRunning = (state == ScanState::Armed || state == ScanState::Scanning);
    bool isIdle = (state == ScanState::Idle || state == ScanState::Done || state == ScanState::Error)
                  && !m_workerThread->isRunning();

    ui->btnScanStart->setEnabled(isIdle);
    ui->btnScanStop->setEnabled(isRunning);
    ui->comboScanMode->setEnabled(isIdle);
    ui->spinNFast->setEnabled(isIdle);
    ui->spinNSlow->setEnabled(isIdle);
    ui->spinDepth->setEnabled(isIdle);
    ui->spinImgW->setEnabled(isIdle);
    ui->spinImgH->setEnabled(isIdle);
    ui->editCsvPath->setEnabled(isIdle);
    ui->btnBrowseCsv->setEnabled(isIdle);
    ui->chkSwapXY->setEnabled(isIdle);
    ui->chkContinuous->setEnabled(isIdle);
    ui->editSavePath->setEnabled(isIdle);
    ui->btnBrowse->setEnabled(isIdle);
    ui->comboSerialPort->setEnabled(isIdle && ui->comboSerialPort->count() > 0);

    ui->btnCardStart->setEnabled(isIdle);
    ui->btnCardStop->setEnabled(isRunning);
    ui->btnForceTrig->setEnabled(true);
    ui->btnFpgaStart->setEnabled(true);
    ui->btnFpgaStop->setEnabled(true);
    ui->chkRasterPngDepth->setEnabled(isIdle);
    ui->chkTrajPngDepth->setEnabled(isIdle);
}

void MainWindow::showError(const QString &title, const QString &detail)
{
    m_statusCardConn->setText(tr("错误: %1").arg(title));
    m_statusCardConn->setStyleSheet("color: red; font-weight: bold;");
    ui->labelCardError->setText(detail);
    ui->labelCardError->setStyleSheet("color: red;");
    QMessageBox::warning(this, title, detail);
}

void MainWindow::logStatus(const QString &msg)
{
    m_statusCardConn->setText(msg);
    m_statusCardConn->setStyleSheet("color: green;");
}

QString MainWindow::decodeM2Status(int32_t status)
{
    if (status == 0) return tr("空闲");
    QStringList parts;
    if (status & 0x00000001) parts << "PRETRG";
    if (status & 0x00000002) parts << "TRG";
    if (status & 0x00000004) parts << "READY";
    if (status & 0x00000100) parts << "BLOCK";
    if (status & 0x00000400) parts << "OVERRUN";
    if (status & 0x00000800) parts << "ERROR";
    if (parts.isEmpty())
        return QString("0x%1").arg(status, 8, 16, QChar('0'));
    return parts.join(", ");
}

void MainWindow::saveSettings()
{
    m_config->setActiveMode(m_scanParams.scanMode);
    m_config->saveScanParams(m_scanParams);
    m_config->saveSavePath(ui->editSavePath->text());
    m_config->saveTrajectoryCsvPath(ui->editCsvPath->text().trimmed());
}

void MainWindow::loadSettings()
{
    // The mode is global; every other parameter is scoped to the mode.
    m_scanParams.scanMode = m_config->loadScanMode();
    m_config->setActiveMode(m_scanParams.scanMode);
    m_scanParams = m_config->loadScanParams();
    ui->editSavePath->setText(m_config->loadSavePath());
    ui->editCsvPath->setText(m_config->loadTrajectoryCsvPath());
    ui->chkRasterDat->setChecked(m_scanParams.saveDat);
    ui->chkRasterPng->setChecked(m_scanParams.savePng);
    ui->chkRasterPngDepth->setChecked(m_scanParams.saveDepthPng);
    ui->chkTrajNpy->setChecked(m_scanParams.saveNpy);
    ui->chkTrajPcd->setChecked(m_scanParams.savePcd);
    ui->chkTrajPng->setChecked(m_scanParams.saveMip);
    ui->chkTrajPngDepth->setChecked(m_scanParams.saveMipDepth);
    ui->chkTrajDat->setChecked(m_scanParams.saveDatTraj);
    ui->cscanWidget->setColorParams(m_config->loadColorMapping(), m_config->loadColorClipping());
    ui->bscanWidget->setColorParams(m_config->loadColorMapping(), m_config->loadColorClipping());
    int cm = m_config->loadColorMap();
    ui->cscanWidget->setColorMap(cm);
    ui->bscanWidget->setColorMap(cm);
    if (m_bscanBar) m_bscanBar->setColorMap(cm);
    if (m_cscanBar) m_cscanBar->setColorMap(cm);

    m_configReady = true;   // from here on, control changes persist immediately
}
