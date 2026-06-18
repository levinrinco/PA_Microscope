#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "AlineWidget.h"
#include "CscanWidget.h"
#include "SpectrumFifo.h"
#include "FpgaSerial.h"
#include "ScanController.h"
#include "DataPipeline.h"
#include "FileWriter.h"
#include "ConfigManager.h"
#include "TrajectoryLoader.h"
#include "ColorBar.h"
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
#include <QCheckBox>
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
#include <QThreadPool>

// ColorMap symbols (defined in ColorMap.cpp)
extern uint32_t hotLUT[256];
extern int mapToIndex(double value, double maxVal, int mapping, bool clipping);

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

    m_alineTimer = new QTimer(this);
    m_serialRefreshTimer = new QTimer(this);

    setupStatusBar();
    setupConnections();

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

    // Splitter: Aline sidebar (narrow) | C-scan MIP (fills remaining space)
    ui->splitter->setStretchFactor(0, 1);   // Aline — don't stretch
    ui->splitter->setStretchFactor(1, 6);   // C-scan — take all extra space
    ui->splitter->setSizes({200, 1000});

    // Create color bar below C-scan
    m_cscanBar = new ColorBar(ui->centralWidget);

    // Load settings
    loadSettings();
    qInfo() << "Settings loaded, depth=" << m_scanParams.depth << "segSize=" << m_scanParams.segmentSize;

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
    ui->spinImgW->setValue(m_scanParams.imageWidth);
    ui->spinImgH->setValue(m_scanParams.imageHeight);
    ui->editCsvPath->setText(m_scanParams.trajectoryCsvPath);

    // Set C-scan dimensions
    ui->cscanWidget->setDimensions(m_scanParams.imageWidth, m_scanParams.imageHeight);

    m_scanCtrl->setScanParams(m_scanParams);

    updateButtonStates(ScanState::Idle);
}

MainWindow::~MainWindow()
{
    if (m_scanCtrl->isRunning()) {
        m_scanCtrl->stopScan();
    }
    m_workerThread->quit();
    m_workerThread->wait(3000);
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
        m_workerThread->quit();
        m_workerThread->wait(3000);
    }
    m_fifo->closeCard();
    m_fpga->closePort();
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
    if (!m_cscanBar || !ui->cscanWidget) return;
    QPoint bl = ui->cscanWidget->mapTo(ui->centralWidget, QPoint(0, ui->cscanWidget->height()));
    m_cscanBar->setGeometry(bl.x(), bl.y() + 3, ui->cscanWidget->width(), m_cscanBar->height());
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
    connect(m_fpga, &FpgaSerial::portOpened, this, [this](bool ok) {
        ui->labelSerialStatus->setText(ok ? tr("已打开") : tr("失败"));
        ui->labelSerialStatus->setStyleSheet(ok ? "color: green;" : "color: red;");
    });
    connect(m_fpga, &FpgaSerial::portClosed, this, [this]() {
        ui->labelSerialStatus->setText(tr("未打开"));
        ui->labelSerialStatus->setStyleSheet("");
    });

    // ---- Card buttons ----
    connect(ui->btnCardStart, &QPushButton::clicked, this, &MainWindow::onCardStart);
    connect(ui->btnCardStop,  &QPushButton::clicked, this, &MainWindow::onCardStop);
    connect(ui->btnForceTrig, &QPushButton::clicked, this, &MainWindow::onForceTrigger);

    // ---- Scan buttons ----
    connect(ui->btnScanStart, &QPushButton::clicked, this, &MainWindow::onScanStart);
    connect(ui->btnScanStop,  &QPushButton::clicked, this, &MainWindow::onScanStop);

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
    auto *actAboutBar = ui->menuBar->addAction(tr("关于"));
    connect(actAboutBar, &QAction::triggered, this, &MainWindow::onAbout);

    // ---- Browse buttons ----
    connect(ui->btnBrowse, &QPushButton::clicked, this, &MainWindow::onBrowseSavePath);
    connect(ui->btnBrowseCsv, &QPushButton::clicked, this, &MainWindow::onBrowseCsvPath);

    // ---- SpinBox changes -> params ----
    connect(ui->spinDepth, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.depth = v;
        if (m_pipeline) m_pipeline->setLiveDepth(v);
    });
    connect(ui->spinWinOffset, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.windowOffset = v;
        if (m_pipeline) m_pipeline->setLiveWindowOffset(v);
    });
    connect(ui->spinImgW, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.imageWidth = v;
        ui->cscanWidget->setDimensions(m_scanParams.imageWidth, v);
    });
    connect(ui->spinImgH, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_scanParams.imageHeight = v;
        ui->cscanWidget->setDimensions(v, m_scanParams.imageHeight);
    });

    // ---- Save enable -> FileWriter ----
    connect(ui->chkSaveEnable, &QCheckBox::toggled, m_writer, &FileWriter::setEnabled);

    // ---- Aline poll timer (30ms) ----
    m_alineTimer->setInterval(30);
    connect(m_alineTimer, &QTimer::timeout, this, [this]() {
        if (m_pipeline) {
            auto d = m_pipeline->getLatestAline();
            if (!d.empty()) ui->alineWidget->setAlineData(d);
        }
        if (m_cscanBar) {
            int rangeMv = m_config->loadChannelRange(0);
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

        uint64_t chMask = m_config->loadChannelMask();
        int nCh = m_fifo->cardInfo().maxChannels;
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
        qInfo() << "Channel setup done, nCh=" << nCh;

        qInfo() << "Setting FIFO mode";
        if (!m_fifo->setupFifoMode(m_scanParams.fifoMode, chMask,
                                    m_scanParams.preTrigger,
                                    m_scanParams.segmentSize,
                                    m_scanParams.loopCount)) {
            qWarning() << "FIFO setup failed:" << m_fifo->lastErrorText();
        }

        int clockMode = m_config->loadClockMode();
        if (clockMode == SPC_CM_EXTERNAL)
            m_fifo->setExternalClock(m_config->loadClockDivider());
        else if (clockMode == SPC_CM_EXTREFCLOCK)
            m_fifo->setReferenceClock(m_config->loadRefClockFreq() * 1000000);
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
        if (!m_fifo->allocateBuffer(bufSize, notSize))
            qWarning() << "DMA alloc failed:" << m_fifo->lastErrorText();
        else
            qInfo() << "DMA buffer allocated OK";

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

void MainWindow::spawnPipeline()
{
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
        if (!m_workerThread->wait(5000)) {
            qWarning() << "spawnPipeline: worker thread stuck, forcing terminate";
            m_workerThread->terminate();
            m_workerThread->wait(3000);
        }
    }

    // 4. Delete old pipeline
    delete m_pipeline;
    m_pipeline = nullptr;

    // Create fresh pipeline
    m_pipeline = new DataPipeline(m_fifo);
    m_scanCtrl->setPipeline(m_pipeline);

    // Async MIP finalization on main thread (non-blocking pipeline)
    connect(m_pipeline, &DataPipeline::mipFrameCompleted, this,
        [this](int slotIdx, int frameIdx) {
            const MipAccum &slot = m_pipeline->mipSlot(slotIdx);
            int w = m_scanParams.imageWidth;
            int h = m_scanParams.imageHeight;
            int gridSize = w * h;
            int cm = m_config->loadColorMapping();
            bool cc = m_config->loadColorClipping();

            // Normalize in-place (slot will be zeroed by releaseMipSlot)
            auto &sum = const_cast<MipAccum&>(slot).sum;
            float maxVal = 0.0f;
            for (int i = 0; i < gridSize; ++i) {
                float wt = slot.weight[i];
                if (wt > 0.0f) {
                    sum[i] /= wt;
                    if (sum[i] > maxVal) maxVal = sum[i];
                } else {
                    sum[i] = 0.0f;
                }
            }

            // Build QImage
            QImage img(w, h, QImage::Format_RGB888);
            for (int y = 0; y < h; ++y) {
                uchar *sl = img.scanLine(y);
                int rb = y * w;
                for (int x = 0; x < w; ++x) {
                    int ci = mapToIndex(sum[rb + x], maxVal, cm, cc);
                    uint32_t c = hotLUT[ci];
                    sl[x * 3 + 0] = c & 0xFF;
                    sl[x * 3 + 1] = (c >> 8) & 0xFF;
                    sl[x * 3 + 2] = (c >> 16) & 0xFF;
                }
            }

            ui->cscanWidget->updateMipImage(img, maxVal);
            m_statusAlineCount->setText(QString("Frame: %1").arg(frameIdx + 1));

            // Return slot to pool (zeroes + releases)
            m_pipeline->releaseMipSlot(slotIdx);
        });

    // Raw data → file save only (no MIP computation on main thread!)
    connect(m_pipeline, &DataPipeline::frameDataReady, this,
        [this](std::shared_ptr<FrameBuf> frameData, int alineCount, int depth, int frameIdx) {
            if (m_writer->isEnabled()) {
                auto writer = m_writer;
                auto *mapInfo = &m_trajectoryMapInfo;   // pointer, no copy
                int iw = m_scanParams.imageWidth;
                int ih = m_scanParams.imageHeight;
                QThreadPool::globalInstance()->start(
                    [writer, frameData, alineCount, depth, frameIdx, mapInfo, iw, ih]() {
                        writer->saveGriddedVolume(frameData->rawAlines.get(), alineCount, depth,
                                                  *mapInfo, iw, ih, frameIdx);
                    });
            }
        });

    connect(m_pipeline, &DataPipeline::scanProgress, this, &MainWindow::onScanProgress);
    connect(m_pipeline, &DataPipeline::fifoStatusChanged, this, [this](const FifoStatus &s) {
        ui->fifoFillBar->setValue(s.fillSizePromille);
        ui->labelCardStatus->setText(decodeM2Status(s.m2Status));
    });
    connect(m_pipeline, &DataPipeline::scanFinished, m_scanCtrl, &ScanController::onPipelineScanComplete);
    connect(m_pipeline, &DataPipeline::errorOccurred, m_scanCtrl, &ScanController::onPipelineError);

    // First-data detection
    connect(m_pipeline, &DataPipeline::mipFrameCompleted, this, [this](int, int) {
        if (!m_firstDataSent) { m_firstDataSent = true; m_scanCtrl->onFirstDataReceived(); }
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
    // Check CSV path
    QString csvPath = ui->editCsvPath->text().trimmed();
    if (csvPath.isEmpty()) {
        showError(tr("缺少轨迹文件"), tr("请先选择螺旋轨迹 CSV 文件。"));
        return;
    }

    // Load trajectory into member variables (owned by MainWindow — outlives scan)
    TrajectoryLoader loader;
    double radiusM = 0.0;
    if (!loader.load(csvPath, m_scanParams.imageWidth, m_scanParams.imageHeight,
                     ui->chkSwapXY->isChecked(),
                     m_trajectoryPoints, m_trajectoryMapInfo, radiusM)) {
        showError(tr("轨迹加载失败"), loader.lastError());
        return;
    }

    m_scanParams.totalAlines = loader.totalAlines();
    m_scanParams.scanRadiusM = radiusM;
    m_scanParams.trajectoryCsvPath = csvPath;

    // Sync channelMask
    m_scanParams.channelMask = m_config->loadChannelMask();
    m_scanCtrl->setScanParams(m_scanParams);

    // Create pipeline
    spawnPipeline();

    // Pass trajectory data to pipeline (m_trajectoryMapInfo lives in MainWindow)
    if (m_pipeline) {
        m_pipeline->setMapInfo(&m_trajectoryMapInfo);
        m_pipeline->setGridSize(m_scanParams.imageWidth, m_scanParams.imageHeight);
        m_pipeline->setUpdateInterval(m_scanParams.cscanUpdateInterval);
        m_pipeline->setSegmentSize(m_scanParams.segmentSize);
        m_pipeline->setContinuous(ui->chkContinuous->isChecked());
        m_pipeline->setColorParams(m_config->loadColorMapping(), m_config->loadColorClipping());
        m_pipeline->setLiveDepth(m_scanParams.depth);
        m_pipeline->setLiveWindowOffset(m_scanParams.windowOffset);
    }

    if (!m_fpga->isOpen()) {
        showError(tr("串口未打开"), tr("请先在下拉菜单中选择串口端口。"));
        return;
    }

    m_writer->setEnabled(ui->chkSaveEnable->isChecked());
    m_writer->setSavePath(ui->editSavePath->text());
    if (m_writer->isEnabled()) {
        m_writer->beginSession();
        m_pipeline->setSaveContext(m_writer, &m_trajectoryMapInfo,
                                   m_scanParams.imageWidth, m_scanParams.imageHeight);
    } else {
        m_pipeline->setSaveContext(nullptr, nullptr, 0, 0);
    }

    m_firstDataSent = false;
    ui->progressScan->setRange(0, m_scanParams.totalAlines);
    ui->progressScan->setValue(0);

    ui->cscanWidget->setDimensions(m_scanParams.imageWidth, m_scanParams.imageHeight);
    ui->cscanWidget->setScanRadius(radiusM, radiusM * 2.0);

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
    updateButtonStates(ScanState::Done);
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
    updateButtonStates(ScanState::Error);
}

// ===== Data display =====

void MainWindow::onScanProgress(int current, int total)
{
    ui->progressScan->setMaximum(total);
    ui->progressScan->setValue(current);
    m_statusScanProgress->setText(tr("扫描: %1 / %2").arg(current).arg(total));
}

void MainWindow::onSerialPortChanged(int /*index*/)
{
    QString port = ui->comboSerialPort->currentData().toString();
    if (port.isEmpty()) return;

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
    for (const auto &p : ports) {
        ui->comboSerialPort->addItem(p.portName() + " — " + p.description(),
                                     p.portName());
    }
    QString target = !previousData.isEmpty() ? previousData : m_config->loadSerialPort();
    if (!target.isEmpty()) {
        int idx = ui->comboSerialPort->findData(target);
        if (idx >= 0) {
            ui->comboSerialPort->setCurrentIndex(idx);
        } else if (!previousData.isEmpty()) {
            if (m_fpga->isOpen() && m_fpga->portName() == previousData)
                m_fpga->closePort();
        }
    }
    ui->comboSerialPort->blockSignals(false);
}

void MainWindow::onBrowseSavePath()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("选择存储路径"),
                                                     ui->editSavePath->text());
    if (!dir.isEmpty()) {
        ui->editSavePath->setText(dir);
    }
}

void MainWindow::onBrowseCsvPath()
{
    QString path = QFileDialog::getOpenFileName(this, tr("选择螺旋轨迹 CSV 文件"),
                                                 QString(),
                                                 tr("CSV 文件 (*.csv);;所有文件 (*.*)"));
    if (!path.isEmpty()) {
        ui->editCsvPath->setText(path);
    }
}

// ===== Settings Dialog (trigger/clock/channels/mode) — kept from M4_AD_FIFO =====
// (Same as original, with bscanWidget references updated to cscanWidget)

void MainWindow::onSettingsCard()
{
    qInfo() << "onSettingsCard: begin";

    int32_t availClockModes = m_fifo->isOpen() ? m_fifo->availableClockModes() : (SPC_CM_INTPLL);
    int32_t availTrigSrc    = m_fifo->isOpen() ? m_fifo->availableTriggerSources() : (SPC_TMASK_EXT0 | SPC_TMASK_EXT1 | SPC_TMASK_SOFTWARE);
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

        auto *table = new QTableWidget(nChannels, headers.size());
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
        if (availTrigSrc & SPC_TMASK_EXT1)    srcEntries.append({SPC_TMASK_EXT1,    1,  tr("X0 TTL 触发 (TTL 电平)")});
        if (availTrigSrc & SPC_TMASK_SOFTWARE) srcEntries.append({SPC_TMASK_SOFTWARE, -1, tr("软件触发 (Software)")});
        for (int ch = 0; ch < 4; ++ch) {
            if (availTrigSrc & (SPC_TMASK0_CH0 << ch))
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
            bool isAnalog = (extLine == 0);
            bool isExt = (extLine <= 1);
            modeCombo->setEnabled(isExt);
            level0Spin->setEnabled(isAnalog);
            level1Spin->setEnabled(isAnalog);
            couplingCombo->setEnabled(isAnalog);
            termCheck->setEnabled(isAnalog);
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
                    if (pc) path = pc->currentIndex(); ++col;
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
            if (availTrigSrc & SPC_TMASK_EXT1)     se.append({SPC_TMASK_EXT1, 1});
            if (availTrigSrc & SPC_TMASK_SOFTWARE) se.append({SPC_TMASK_SOFTWARE, -1});
            for (int ch = 0; ch < 4; ++ch)
                if (availTrigSrc & (SPC_TMASK0_CH0 << ch))
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
                if (extLine <= 1) {
                    m_fifo->setExternalTrigger(mode, extLine, termCk ? termCk->isChecked() : true);
                    m_fifo->setTriggerCoupling(extLine, coupCb && coupCb->currentIndex() == 1);
                    m_fifo->setTriggerLevel(extLine, lvl0Sp->value(), lvl1Sp->value());
                }
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
            if (availClockModes & SPC_CM_INTPLL)      ce.append({SPC_CM_INTPLL});
            if (availClockModes & SPC_CM_EXTERNAL)    ce.append({SPC_CM_EXTERNAL});
            if (availClockModes & SPC_CM_EXTREFCLOCK) ce.append({SPC_CM_EXTREFCLOCK});
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
                        m_fifo->setReferenceClock(refSp->value() * 1000000);
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
        if (fifoCb && segSp && preSp && loopSp && woffSp && wsizeSp && bufSp && notSp) {
            static const int mVals[4] = {SPC_REC_FIFO_SINGLE, SPC_REC_FIFO_MULTI, SPC_REC_FIFO_GATE, SPC_REC_FIFO_ABA};
            m_scanParams.fifoMode     = mVals[fifoCb->currentIndex()];
            m_scanParams.segmentSize  = segSp->value();
            m_scanParams.preTrigger   = preSp->value();
            m_scanParams.loopCount    = loopSp->value();
            m_scanParams.windowOffset = woffSp->value();
            m_scanParams.depth        = wsizeSp->value();
            m_config->saveScanParams(m_scanParams);
            ui->spinDepth->setValue(m_scanParams.depth);
            ui->spinWinOffset->setValue(m_scanParams.windowOffset);
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
            int newBufMB = bufSp->value(), newNotKB = notSp->value();
            int oldBufMB = m_config->loadBufferSizeMB(), oldNotKB = m_config->loadNotifySizeKB();
            m_config->saveBufferSizeMB(newBufMB);
            m_config->saveNotifySizeKB(newNotKB);
            if (m_fifo->isOpen() && (newBufMB != oldBufMB || newNotKB != oldNotKB)) {
                m_fifo->freeBuffer();
                int64_t bufSz = static_cast<int64_t>(newBufMB) * 1024 * 1024;
                int segBytes = m_scanParams.segmentSize * 2;
                if (segBytes > 0) bufSz = (bufSz / segBytes) * segBytes;
                m_fifo->allocateBuffer(bufSz, static_cast<int32_t>(newNotKB) * 1024);
            }
        }
    }
}

void MainWindow::onAbout()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("关于 光声显微镜螺旋扫描采集软件"));
    auto *layout = new QVBoxLayout(&dlg);
    auto *label = new QLabel(tr("螺旋扫描光声显微镜数据采集控制软件\n"
                                "PA Spiral PAM\n\n"
                                "支持 Spectrum M4i 系列采集卡\n"
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

    auto *clipCombo = new QComboBox(&dlg);
    clipCombo->addItem(tr("削顶 (1.1x headroom)"), true);
    clipCombo->addItem(tr("不削顶 (max → white)"), false);
    clipCombo->setCurrentIndex(m_config->loadColorClipping() ? 0 : 1);
    layout->addRow(tr("映射削顶:"), clipCombo);

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
    m_config->saveColorMapping(newMapping);
    m_config->saveColorClipping(newClipping);
    ui->cscanWidget->setColorParams(newMapping, newClipping);
}

// ===== Helpers =====

void MainWindow::updateButtonStates(ScanState state)
{
    bool isRunning = (state == ScanState::Armed || state == ScanState::Scanning);
    bool isIdle = (state == ScanState::Idle || state == ScanState::Done || state == ScanState::Error);

    ui->btnScanStart->setEnabled(isIdle);
    ui->btnScanStop->setEnabled(isRunning);
    ui->spinDepth->setEnabled(isIdle);
    ui->spinImgW->setEnabled(isIdle);
    ui->spinImgH->setEnabled(isIdle);
    ui->editCsvPath->setEnabled(isIdle);

    ui->btnCardStart->setEnabled(isIdle);
    ui->btnCardStop->setEnabled(isRunning);
    ui->btnForceTrig->setEnabled(true);
    ui->btnFpgaStart->setEnabled(true);
    ui->btnFpgaStop->setEnabled(true);
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
    m_config->saveScanParams(m_scanParams);
    m_config->saveSaveEnabled(ui->chkSaveEnable->isChecked());
    m_config->saveSavePath(ui->editSavePath->text());
    m_config->saveTrajectoryCsvPath(ui->editCsvPath->text().trimmed());
}

void MainWindow::loadSettings()
{
    m_scanParams = m_config->loadScanParams();
    ui->chkSaveEnable->setChecked(m_config->loadSaveEnabled());
    ui->editSavePath->setText(m_config->loadSavePath());
    ui->editCsvPath->setText(m_config->loadTrajectoryCsvPath());
    ui->cscanWidget->setColorParams(m_config->loadColorMapping(), m_config->loadColorClipping());
}
