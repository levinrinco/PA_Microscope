#include "SerialDebugDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSerialPortInfo>
#include <QShortcut>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Long serial sessions must not grow without bound. The buffer is deliberately
// finite: a debug assistant that eats gigabytes is worse than losing old lines.
constexpr qint64 kMaxReceiveBytes = 512 * 1024;
constexpr int kMaxReceiveBlocks = 20000;

const QString kGreen = QStringLiteral("#1b7f3a");
const QString kRed = QStringLiteral("#b00020");
const QString kGray = QStringLiteral("#666666");
const QString kOrange = QStringLiteral("#b26a00");

} // namespace

SerialDebugDialog::SerialDebugDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("串口调试助手"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    buildUi();

    connect(&m_serial, &QSerialPort::readyRead,
            this, &SerialDebugDialog::readAvailable);
    connect(&m_serial, &QSerialPort::errorOccurred,
            this, &SerialDebugDialog::handlePortError);
    connect(&m_serial, &QSerialPort::bytesWritten, this, [this](qint64 bytes) {
        m_txTotal += bytes;
        updateCounterLabel();
    });

    m_loopTimer = new QTimer(this);
    connect(m_loopTimer, &QTimer::timeout, this, &SerialDebugDialog::sendData);

    connect(m_refreshButton, &QPushButton::clicked,
            this, &SerialDebugDialog::refreshPorts);
    connect(m_openButton, &QPushButton::clicked,
            this, &SerialDebugDialog::togglePort);
    connect(m_sendButton, &QPushButton::clicked,
            this, &SerialDebugDialog::sendData);
    connect(m_clearSendButton, &QPushButton::clicked, this, [this] {
        m_sendEdit->clear();
        m_sendEdit->setFocus();
    });
    connect(m_clearReceiveButton, &QPushButton::clicked,
            this, &SerialDebugDialog::clearReceive);
    connect(m_clearCountersButton, &QPushButton::clicked,
            this, &SerialDebugDialog::clearCounters);
    connect(m_releaseAcquisitionButton, &QPushButton::clicked, this, [this] {
        const QString port = m_acquisitionPort;
        emit acquisitionPortReleaseRequested();
        // The main window clears m_acquisitionPort synchronously when it
        // accepts the release. Keep the released device selected for the next
        // "打开串口" click.
        if (!port.isEmpty() && m_acquisitionPort.isEmpty() && !m_serial.isOpen()) {
            const int index = m_portCombo->findData(port);
            if (index >= 0)
                m_portCombo->setCurrentIndex(index);
            else
                m_portCombo->setEditText(port);
        }
        if (!port.isEmpty() && m_acquisitionPort.isEmpty()) {
            setMessage(m_serial.isOpen()
                           ? tr("采集卡串口 %1 已释放；关闭当前调试串口后可打开它。").arg(port)
                           : tr("采集卡串口 %1 已释放，可以点击“打开串口”。").arg(port),
                       false);
        }
    });

    connect(m_hexDisplayCheck, &QCheckBox::toggled,
            this, &SerialDebugDialog::syncDisplayOptions);
    connect(m_timestampCheck, &QCheckBox::toggled,
            this, &SerialDebugDialog::syncDisplayOptions);
    connect(m_hexSendCheck, &QCheckBox::toggled,
            this, &SerialDebugDialog::syncSendOptions);
    connect(m_loopCheck, &QCheckBox::toggled,
            this, &SerialDebugDialog::updateLoopSend);
    connect(m_loopIntervalSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int ms) {
        if (m_loopTimer->isActive())
            m_loopTimer->start(ms);
    });

    // DTR/RTS matter for boards that use them as enable/reset lines. Applying
    // them only while open avoids pretending the state was set beforehand.
    connect(m_dtrCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_serial.isOpen() && !m_closing)
            m_serial.setDataTerminalReady(on);
    });
    connect(m_rtsCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_serial.isOpen() && !m_closing)
            m_serial.setRequestToSend(on);
    });

    // Ctrl+Enter sends from anywhere in the dialog. Plain Enter in the send
    // box still inserts a newline, which is what a protocol editor needs.
    auto *sendShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this);
    connect(sendShortcut, &QShortcut::activated, this, &SerialDebugDialog::sendData);

    updateControls();
    syncSendOptions();
    updateCounterLabel();
    setConnectionStatus(tr("未打开"), kGray);
}

SerialDebugDialog::~SerialDebugDialog()
{
    if (m_loopTimer)
        m_loopTimer->stop();
    if (m_serial.isOpen())
        m_serial.close();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void SerialDebugDialog::buildUi()
{
    resize(860, 660);
    setMinimumSize(780, 560);
    setSizeGripEnabled(true);

    auto *outer = new QVBoxLayout(this);
    outer->setSpacing(10);

    // ---- Port settings ----
    auto *portBox = new QGroupBox(tr("端口设置"), this);
    auto *portGrid = new QGridLayout(portBox);
    portGrid->setHorizontalSpacing(8);
    portGrid->setVerticalSpacing(6);

    m_portCombo = new QComboBox(portBox);
    m_portCombo->setObjectName(QStringLiteral("portCombo"));
    m_portCombo->setEditable(true);
    m_portCombo->setInsertPolicy(QComboBox::NoInsert);
    m_portCombo->setMinimumContentsLength(20);
    m_portCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_portCombo->setToolTip(tr("列出系统检测到的串口，也可以直接输入设备路径，例如 /dev/ttyUSB0。"));
    if (m_portCombo->lineEdit())
        m_portCombo->lineEdit()->setPlaceholderText(tr("选择或输入串口设备"));

    m_refreshButton = new QPushButton(tr("刷新"), portBox);
    m_refreshButton->setObjectName(QStringLiteral("refreshButton"));
    m_refreshButton->setAutoDefault(false);
    m_refreshButton->setFixedWidth(80);
    m_refreshButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_refreshButton->setToolTip(tr("重新枚举串口。已插入但未列出的设备可以直接输入设备路径。"));

    m_baudCombo = new QComboBox(portBox);
    m_baudCombo->setObjectName(QStringLiteral("baudCombo"));
    m_baudCombo->setEditable(true);
    for (int baud : {1200, 2400, 4800, 9600, 19200, 38400, 57600,
                     115200, 230400, 460800, 921600, 1000000, 2000000}) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    m_baudCombo->setCurrentText(QStringLiteral("115200"));
    m_baudCombo->setValidator(new QIntValidator(1, 4000000, m_baudCombo));
    m_baudCombo->setToolTip(tr("可以输入自定义波特率（1–4000000）。"));

    m_dataBitsCombo = new QComboBox(portBox);
    m_dataBitsCombo->setObjectName(QStringLiteral("dataBitsCombo"));
    for (int bits : {5, 6, 7, 8})
        m_dataBitsCombo->addItem(QString::number(bits), bits);
    m_dataBitsCombo->setCurrentIndex(m_dataBitsCombo->findData(8));

    m_parityCombo = new QComboBox(portBox);
    m_parityCombo->setObjectName(QStringLiteral("parityCombo"));
    m_parityCombo->addItem(tr("无"), static_cast<int>(QSerialPort::NoParity));
    m_parityCombo->addItem(tr("偶校验"), static_cast<int>(QSerialPort::EvenParity));
    m_parityCombo->addItem(tr("奇校验"), static_cast<int>(QSerialPort::OddParity));
    m_parityCombo->addItem(tr("标记"), static_cast<int>(QSerialPort::MarkParity));
    m_parityCombo->addItem(tr("空格"), static_cast<int>(QSerialPort::SpaceParity));

    m_stopBitsCombo = new QComboBox(portBox);
    m_stopBitsCombo->setObjectName(QStringLiteral("stopBitsCombo"));
    m_stopBitsCombo->addItem(QStringLiteral("1"), static_cast<int>(QSerialPort::OneStop));
    m_stopBitsCombo->addItem(QStringLiteral("1.5"), static_cast<int>(QSerialPort::OneAndHalfStop));
    m_stopBitsCombo->addItem(QStringLiteral("2"), static_cast<int>(QSerialPort::TwoStop));

    m_flowCombo = new QComboBox(portBox);
    m_flowCombo->setObjectName(QStringLiteral("flowCombo"));
    m_flowCombo->addItem(tr("无"), static_cast<int>(QSerialPort::NoFlowControl));
    m_flowCombo->addItem(tr("硬件 RTS/CTS"), static_cast<int>(QSerialPort::HardwareControl));
    m_flowCombo->addItem(tr("软件 XON/XOFF"), static_cast<int>(QSerialPort::SoftwareControl));

    m_openButton = new QPushButton(tr("打开串口"), portBox);
    m_openButton->setObjectName(QStringLiteral("openButton"));
    m_openButton->setAutoDefault(false);
    m_openButton->setFixedWidth(120);
    m_openButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_dtrCheck = new QCheckBox(tr("DTR"), portBox);
    m_dtrCheck->setObjectName(QStringLiteral("dtrCheck"));
    m_dtrCheck->setChecked(true);
    m_dtrCheck->setToolTip(tr("数据终端就绪。部分 FPGA/单片机板把 DTR 当作使能或复位信号。"));

    m_rtsCheck = new QCheckBox(tr("RTS"), portBox);
    m_rtsCheck->setObjectName(QStringLiteral("rtsCheck"));
    m_rtsCheck->setToolTip(tr("请求发送。仅在需要手动控制该引脚时使用。"));

    m_statusLabel = new QLabel(tr("未打开"), portBox);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    portGrid->addWidget(new QLabel(tr("串口"), portBox), 0, 0);
    portGrid->addWidget(m_portCombo, 0, 1, 1, 4);
    portGrid->addWidget(m_refreshButton, 0, 5);

    portGrid->addWidget(new QLabel(tr("波特率"), portBox), 1, 0);
    portGrid->addWidget(m_baudCombo, 1, 1);
    portGrid->addWidget(new QLabel(tr("数据位"), portBox), 1, 2);
    portGrid->addWidget(m_dataBitsCombo, 1, 3);
    portGrid->addWidget(new QLabel(tr("校验"), portBox), 1, 4);
    portGrid->addWidget(m_parityCombo, 1, 5);

    portGrid->addWidget(new QLabel(tr("停止位"), portBox), 2, 0);
    portGrid->addWidget(m_stopBitsCombo, 2, 1);
    portGrid->addWidget(new QLabel(tr("流控"), portBox), 2, 2);
    portGrid->addWidget(m_flowCombo, 2, 3);
    portGrid->addWidget(m_openButton, 2, 4, 1, 2);

    portGrid->addWidget(m_dtrCheck, 3, 0);
    portGrid->addWidget(m_rtsCheck, 3, 1);
    portGrid->addWidget(m_statusLabel, 3, 2, 1, 4);

    m_acquisitionRow = new QWidget(portBox);
    auto *hintLayout = new QHBoxLayout(m_acquisitionRow);
    hintLayout->setContentsMargins(0, 0, 0, 0);
    hintLayout->setSpacing(8);

    m_acquisitionHint = new QLabel(m_acquisitionRow);
    m_acquisitionHint->setObjectName(QStringLiteral("acquisitionHint"));
    m_acquisitionHint->setWordWrap(true);
    m_acquisitionHint->setStyleSheet(QStringLiteral("color: %1;").arg(kOrange));
    hintLayout->addWidget(m_acquisitionHint, 1);

    m_releaseAcquisitionButton = new QPushButton(tr("释放采集卡串口"), m_acquisitionRow);
    m_releaseAcquisitionButton->setObjectName(QStringLiteral("releaseAcquisitionButton"));
    m_releaseAcquisitionButton->setAutoDefault(false);
    m_releaseAcquisitionButton->setToolTip(
        tr("扫描未进行时，先发送 F0 停止命令，再断开采集程序对该串口的占用，以便在本助手中调试。"));
    hintLayout->addWidget(m_releaseAcquisitionButton);

    m_acquisitionRow->hide();
    portGrid->addWidget(m_acquisitionRow, 4, 0, 1, 6);

    portGrid->setAlignment(m_refreshButton, Qt::AlignRight | Qt::AlignVCenter);
    portGrid->setAlignment(m_openButton, Qt::AlignRight | Qt::AlignVCenter);
    portGrid->setAlignment(m_statusLabel, Qt::AlignLeft | Qt::AlignVCenter);
    portGrid->setColumnStretch(1, 2);
    portGrid->setColumnStretch(3, 1);
    portGrid->setColumnStretch(5, 2);
    outer->addWidget(portBox);

    // ---- Receive ----
    auto *rxBox = new QGroupBox(tr("接收"), this);
    auto *rxLayout = new QVBoxLayout(rxBox);

    auto *rxTools = new QHBoxLayout();
    m_hexDisplayCheck = new QCheckBox(tr("十六进制显示"), rxBox);
    m_hexDisplayCheck->setObjectName(QStringLiteral("hexDisplayCheck"));
    m_timestampCheck = new QCheckBox(tr("时间戳"), rxBox);
    m_timestampCheck->setObjectName(QStringLiteral("timestampCheck"));
    m_autoScrollCheck = new QCheckBox(tr("自动滚动"), rxBox);
    m_autoScrollCheck->setObjectName(QStringLiteral("autoScrollCheck"));
    m_autoScrollCheck->setChecked(true);

    m_clearReceiveButton = new QPushButton(tr("清空接收"), rxBox);
    m_clearReceiveButton->setObjectName(QStringLiteral("clearReceiveButton"));
    m_clearReceiveButton->setAutoDefault(false);

    rxTools->addWidget(m_hexDisplayCheck);
    rxTools->addWidget(m_timestampCheck);
    rxTools->addWidget(m_autoScrollCheck);
    rxTools->addSpacing(12);
    rxTools->addWidget(m_clearReceiveButton);
    rxTools->addStretch(1);
    rxLayout->addLayout(rxTools);

    m_receiveView = new QPlainTextEdit(rxBox);
    m_receiveView->setObjectName(QStringLiteral("receiveView"));
    m_receiveView->setReadOnly(true);
    m_receiveView->setUndoRedoEnabled(false);
    m_receiveView->setMaximumBlockCount(kMaxReceiveBlocks);
    m_receiveView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_receiveView->setPlaceholderText(tr("接收到的数据会显示在这里"));
    rxLayout->addWidget(m_receiveView, 1);
    outer->addWidget(rxBox, 1);

    // ---- Send ----
    auto *txBox = new QGroupBox(tr("发送"), this);
    auto *txLayout = new QVBoxLayout(txBox);

    m_sendEdit = new QPlainTextEdit(txBox);
    m_sendEdit->setObjectName(QStringLiteral("sendEdit"));
    m_sendEdit->setFixedHeight(72);
    m_sendEdit->setTabChangesFocus(true);
    m_sendEdit->setPlaceholderText(tr("输入要发送的内容。Ctrl+Enter 发送。"));
    txLayout->addWidget(m_sendEdit);

    auto *txTools = new QHBoxLayout();
    m_hexSendCheck = new QCheckBox(tr("十六进制发送"), txBox);
    m_hexSendCheck->setObjectName(QStringLiteral("hexSendCheck"));

    txTools->addWidget(m_hexSendCheck);
    txTools->addSpacing(8);
    txTools->addWidget(new QLabel(tr("行尾"), txBox));

    m_lineEndingCombo = new QComboBox(txBox);
    m_lineEndingCombo->setObjectName(QStringLiteral("lineEndingCombo"));
    m_lineEndingCombo->addItem(tr("无"), 0);
    m_lineEndingCombo->addItem(QStringLiteral("\\n (LF)"), 1);
    m_lineEndingCombo->addItem(QStringLiteral("\\r\\n (CRLF)"), 2);
    txTools->addWidget(m_lineEndingCombo);

    txTools->addSpacing(12);
    m_loopCheck = new QCheckBox(tr("循环发送"), txBox);
    m_loopCheck->setObjectName(QStringLiteral("loopCheck"));
    txTools->addWidget(m_loopCheck);

    m_loopIntervalSpin = new QSpinBox(txBox);
    m_loopIntervalSpin->setObjectName(QStringLiteral("loopIntervalSpin"));
    m_loopIntervalSpin->setRange(10, 600000);
    m_loopIntervalSpin->setValue(1000);
    m_loopIntervalSpin->setSuffix(tr(" ms"));
    m_loopIntervalSpin->setEnabled(false);
    txTools->addWidget(m_loopIntervalSpin);

    m_clearSendButton = new QPushButton(tr("清空内容"), txBox);
    m_clearSendButton->setObjectName(QStringLiteral("clearSendButton"));
    m_clearSendButton->setAutoDefault(false);
    txTools->addWidget(m_clearSendButton);

    txTools->addStretch(1);
    m_sendButton = new QPushButton(tr("发送"), txBox);
    m_sendButton->setObjectName(QStringLiteral("sendButton"));
    m_sendButton->setAutoDefault(false);
    m_sendButton->setMinimumWidth(96);
    m_sendButton->setToolTip(tr("发送当前内容（Ctrl+Enter）。"));
    txTools->addWidget(m_sendButton);
    txLayout->addLayout(txTools);
    outer->addWidget(txBox);

    // ---- Bottom status ----
    auto *bottom = new QHBoxLayout();
    m_messageLabel = new QLabel(this);
    m_messageLabel->setObjectName(QStringLiteral("messageLabel"));
    m_messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_messageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMessage(QString(), false);

    m_counterLabel = new QLabel(this);
    m_counterLabel->setObjectName(QStringLiteral("counterLabel"));
    m_counterLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kGray));

    m_clearCountersButton = new QPushButton(tr("清空计数"), this);
    m_clearCountersButton->setObjectName(QStringLiteral("clearCountersButton"));
    m_clearCountersButton->setAutoDefault(false);

    auto *closeButton = new QPushButton(tr("关闭"), this);
    closeButton->setObjectName(QStringLiteral("closeButton"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    bottom->addWidget(m_messageLabel, 1);
    bottom->addWidget(m_counterLabel);
    bottom->addSpacing(8);
    bottom->addWidget(m_clearCountersButton);
    bottom->addWidget(closeButton);
    outer->addLayout(bottom);

    refreshPorts();
}

// ---------------------------------------------------------------------------
// Port lifecycle
// ---------------------------------------------------------------------------

void SerialDebugDialog::refreshPorts()
{
    const QString previous = selectedPort();

    m_portCombo->blockSignals(true);
    m_portCombo->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto &info : ports) {
        QString label = info.portName();
        if (!info.description().isEmpty())
            label += QStringLiteral(" — ") + info.description();
        m_portCombo->addItem(label, info.portName());
    }
    if (ports.isEmpty())
        m_portCombo->addItem(tr("未检测到串口"), QString());

    int index = m_portCombo->findData(previous);
    if (index >= 0) {
        m_portCombo->setCurrentIndex(index);
    } else if (!previous.isEmpty()) {
        // Keep a manually typed device path across refreshes.
        m_portCombo->setEditText(previous);
    } else {
        m_portCombo->setCurrentIndex(0);
    }
    m_portCombo->blockSignals(false);

    updateControls();
}

QString SerialDebugDialog::selectedPort() const
{
    const QString text = m_portCombo->currentText().trimmed();
    if (text.isEmpty())
        return QString();

    const int index = m_portCombo->currentIndex();
    if (index >= 0 && text == m_portCombo->itemText(index))
        return m_portCombo->itemData(index).toString();

    // Editable combo: the user typed a path (or a label copied from the list).
    QString manual = text;
    const int separator = manual.indexOf(QStringLiteral(" — "));
    if (separator > 0)
        manual = manual.left(separator);
    return manual.trimmed();
}

int SerialDebugDialog::selectedBaud() const
{
    bool ok = false;
    const int baud = m_baudCombo->currentText().trimmed().toInt(&ok);
    return (ok && baud > 0) ? baud : 115200;
}

void SerialDebugDialog::togglePort()
{
    if (m_serial.isOpen())
        closePort();
    else
        openPort();
}

QString SerialDebugDialog::normalizedPortName(const QString &name)
{
    QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return trimmed;
#ifdef Q_OS_WIN
    // QSerialPortInfo reports "COM3"; users may type the device path form.
    if (trimmed.startsWith(QStringLiteral("\\\\.\\")))
        trimmed.remove(0, 4);
    else if (trimmed.startsWith(QStringLiteral("//./")))
        trimmed.remove(0, 4);
    return trimmed.toUpper();   // COM port names are case-insensitive
#else
    // ttyUSB0, /dev/ttyUSB0 and a /dev/serial/by-id symlink are one device.
    const QString path = trimmed.startsWith(QLatin1Char('/'))
                             ? trimmed
                             : QStringLiteral("/dev/") + trimmed;
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
#endif
}

void SerialDebugDialog::openPort()
{
    if (m_serial.isOpen())
        return;

    const QString port = selectedPort();
    if (port.isEmpty()) {
        setConnectionStatus(tr("未选择串口"), kRed);
        setMessage(tr("请先选择串口，或直接输入设备路径。"), true);
        return;
    }

    // Two readers on one tty split the response stream. Refuse explicitly
    // instead of letting a "successful" open hide the acquisition conflict.
    if (!m_acquisitionPort.isEmpty() &&
        normalizedPortName(port) == normalizedPortName(m_acquisitionPort)) {
        setConnectionStatus(tr("采集卡串口被占用"), kRed);
        setMessage(tr("这个串口正被采集程序使用；请先停止使用它的采集流程，或在调试助手中选择其他串口。"), true);
        return;
    }

    m_serial.setPortName(port);
    m_serial.setBaudRate(selectedBaud());
    m_serial.setDataBits(static_cast<QSerialPort::DataBits>(
        m_dataBitsCombo->currentData().toInt()));
    m_serial.setParity(static_cast<QSerialPort::Parity>(
        m_parityCombo->currentData().toInt()));
    m_serial.setStopBits(static_cast<QSerialPort::StopBits>(
        m_stopBitsCombo->currentData().toInt()));
    m_serial.setFlowControl(static_cast<QSerialPort::FlowControl>(
        m_flowCombo->currentData().toInt()));

    m_opening = true;
    const bool opened = m_serial.open(QIODevice::ReadWrite);
    const QString openError = m_serial.errorString();
    m_opening = false;

    if (!opened) {
        setConnectionStatus(tr("打开失败"), kRed);
        setMessage(openError, true);
        m_serial.clearError();
        return;
    }

    // Apply the pins after open; adapters without these lines simply report a
    // failure that is not fatal for data transfer.
    m_serial.setDataTerminalReady(m_dtrCheck->isChecked());
    m_serial.setRequestToSend(m_rtsCheck->isChecked());

    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    const QString actualBaud = QString::number(m_serial.baudRate());
    setConnectionStatus(tr("已打开 %1 · %2 bps").arg(port, actualBaud), kGreen);
    setMessage(QString(), false);
    updateControls();
    emit portStateChanged();
}

void SerialDebugDialog::closePort()
{
    m_closing = true;
    if (m_loopTimer)
        m_loopTimer->stop();
    if (m_loopCheck) {
        QSignalBlocker blocker(m_loopCheck);
        m_loopCheck->setChecked(false);
    }
    if (m_serial.isOpen())
        m_serial.close();
    m_closing = false;

    setConnectionStatus(tr("未打开"), kGray);
    updateControls();
    // Also emitted when the dialog closes without ever opening a port: the main
    // window uses it to reconnect the acquisition port after a released port.
    emit portStateChanged();
}

bool SerialDebugDialog::isPortOpen() const
{
    return m_serial.isOpen();
}

QString SerialDebugDialog::portName() const
{
    return m_serial.isOpen() ? m_serial.portName() : QString();
}

void SerialDebugDialog::setAcquisitionPort(const QString &portName)
{
    m_acquisitionPort = portName.trimmed();
    if (m_acquisitionPort.isEmpty()) {
        m_acquisitionRow->hide();
    } else {
        m_acquisitionHint->setText(
            tr("采集卡串口：%1 —— 采集程序占用期间，调试助手不能同时打开同一端口。")
                .arg(m_acquisitionPort));
        m_releaseAcquisitionButton->setEnabled(true);
        m_acquisitionRow->show();
    }
}

void SerialDebugDialog::closeEvent(QCloseEvent *event)
{
    closePort();
    QDialog::closeEvent(event);
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

void SerialDebugDialog::readAvailable()
{
    const QByteArray bytes = m_serial.readAll();
    if (bytes.isEmpty())
        return;
    m_rxTotal += bytes.size();
    appendReceived(bytes);
    updateCounterLabel();
}

void SerialDebugDialog::appendReceived(const QByteArray &bytes)
{
    if (m_rxBytes + bytes.size() > kMaxReceiveBytes) {
        const bool hadData = !m_rxChunks.empty();
        m_rxChunks.clear();
        m_rxBytes = 0;
        m_decoder = QStringDecoder(QStringDecoder::Utf8);
        m_receiveView->clear();
        if (hadData) {
            insertRawText(tr("……（接收缓冲达到 %1 KB，已清空，继续接收）\n")
                              .arg(kMaxReceiveBytes / 1024));
        }
    }

    const qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    m_rxChunks.push_back({stamp, bytes});
    m_rxBytes += bytes.size();
    renderChunk(bytes, stamp);
}

void SerialDebugDialog::renderChunk(const QByteArray &bytes, qint64 timestampMs)
{
    QString piece;
    if (m_hexDisplayCheck->isChecked()) {
        piece = QString::fromLatin1(bytes.toHex(' ')).toUpper();
    } else {
        // A stateful decoder keeps multi-byte UTF-8 sequences intact even when
        // the serial port delivers them in two readyRead chunks.
        piece = m_decoder.decode(bytes);
    }
    if (piece.isEmpty())
        return;

    if (m_timestampCheck->isChecked()) {
        piece = QStringLiteral("[%1] %2\n")
                    .arg(QDateTime::fromMSecsSinceEpoch(timestampMs)
                             .toString(QStringLiteral("HH:mm:ss.zzz")),
                         piece);
    }

    insertRawText(piece);
    if (m_autoScrollCheck->isChecked())
        m_receiveView->verticalScrollBar()->setValue(
            m_receiveView->verticalScrollBar()->maximum());
}

void SerialDebugDialog::insertRawText(const QString &text)
{
    QTextCursor cursor(m_receiveView->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
}

void SerialDebugDialog::rebuildReceiveView()
{
    QString text;
    // The live decoder must restart together with the rebuilt text, or a
    // partial multi-byte sequence from before a hex/text toggle would corrupt
    // the next live chunk.
    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const bool hex = m_hexDisplayCheck->isChecked();
    const bool timestamp = m_timestampCheck->isChecked();

    for (const auto &chunk : m_rxChunks) {
        QString piece = hex
            ? QString::fromLatin1(chunk.bytes.toHex(' ')).toUpper()
            : decoder.decode(chunk.bytes);
        if (piece.isEmpty())
            continue;
        if (timestamp) {
            piece = QStringLiteral("[%1] %2\n")
                        .arg(QDateTime::fromMSecsSinceEpoch(chunk.timestampMs)
                                 .toString(QStringLiteral("HH:mm:ss.zzz")),
                             piece);
        }
        text += piece;
    }
    m_receiveView->setPlainText(text);
    if (m_autoScrollCheck->isChecked())
        m_receiveView->verticalScrollBar()->setValue(
            m_receiveView->verticalScrollBar()->maximum());
}

void SerialDebugDialog::syncDisplayOptions()
{
    rebuildReceiveView();
}

void SerialDebugDialog::clearReceive()
{
    m_rxChunks.clear();
    m_rxBytes = 0;
    m_rxTotal = 0;
    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    m_receiveView->clear();
    updateCounterLabel();
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

void SerialDebugDialog::syncSendOptions()
{
    const bool hex = m_hexSendCheck->isChecked();
    m_lineEndingCombo->setEnabled(!hex);
    m_sendEdit->setPlaceholderText(
        hex ? tr("十六进制字节，例如：AA 55 01 0D 0A（空格、逗号可省略）")
            : tr("输入要发送的文本。Ctrl+Enter 发送。"));
}

bool SerialDebugDialog::composePayload(QByteArray *payload, QString *error) const
{
    const QString source = m_sendEdit->toPlainText();
    if (source.isEmpty()) {
        *error = tr("发送内容为空。");
        return false;
    }

    if (m_hexSendCheck->isChecked()) {
        QString cleaned = source;
        // Accept the common human formats: "AA 55", "0xAA,0x55", "AA-55".
        cleaned.remove(QRegularExpression(QStringLiteral("0[xX]")));
        cleaned.remove(QRegularExpression(QStringLiteral("[\\s,;:_-]")));
        static const QRegularExpression hexPattern(QStringLiteral("^[0-9a-fA-F]+$"));
        if (cleaned.isEmpty()) {
            *error = tr("没有可发送的十六进制字节。");
            return false;
        }
        if (!hexPattern.match(cleaned).hasMatch()) {
            *error = tr("十六进制内容包含非法字符。");
            return false;
        }
        if (cleaned.size() % 2 != 0) {
            *error = tr("十六进制内容必须由完整字节组成（字符数为偶数）。");
            return false;
        }
        *payload = QByteArray::fromHex(cleaned.toLatin1());
    } else {
        *payload = source.toUtf8();
        switch (m_lineEndingCombo->currentData().toInt()) {
        case 1: payload->append('\n'); break;
        case 2: payload->append("\r\n"); break;
        default: break;
        }
    }

    if (payload->isEmpty()) {
        *error = tr("发送内容为空。");
        return false;
    }
    return true;
}

void SerialDebugDialog::sendData()
{
    if (!m_serial.isOpen()) {
        setMessage(tr("串口未打开，无法发送。"), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    QByteArray payload;
    QString error;
    if (!composePayload(&payload, &error)) {
        setMessage(error, true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    // Loop sending can outrun a slow line. Bound the queue instead of growing
    // memory without limit; the operator can slow the interval down.
    if (m_serial.bytesToWrite() > 1024 * 1024) {
        setMessage(tr("发送队列积压超过 1 MB，已停止循环发送。"), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }

    if (m_serial.write(payload) < 0) {
        setMessage(tr("发送失败：%1").arg(m_serial.errorString()), true);
        if (m_loopCheck->isChecked())
            m_loopCheck->setChecked(false);
        return;
    }
    setMessage(tr("已发送 %1 字节。").arg(payload.size()), false);
}

void SerialDebugDialog::updateLoopSend()
{
    const bool open = m_serial.isOpen();
    if (m_loopCheck->isChecked() && !open) {
        QSignalBlocker blocker(m_loopCheck);
        m_loopCheck->setChecked(false);
    }

    if (m_loopCheck->isChecked())
        m_loopTimer->start(m_loopIntervalSpin->value());
    else
        m_loopTimer->stop();

    m_loopIntervalSpin->setEnabled(open && m_loopCheck->isChecked());
}

// ---------------------------------------------------------------------------
// Errors and state
// ---------------------------------------------------------------------------

void SerialDebugDialog::handlePortError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || m_opening || m_closing)
        return;

    const QString message = m_serial.errorString();
    switch (error) {
    case QSerialPort::ResourceError:
    case QSerialPort::DeviceNotFoundError:
    case QSerialPort::PermissionError:
        closePort();
        setConnectionStatus(tr("串口已断开"), kRed);
        setMessage(message, true);
        break;
    case QSerialPort::ReadError:
    case QSerialPort::WriteError:
        setMessage(tr("串口错误：%1").arg(message), true);
        break;
    default:
        break;
    }
}

void SerialDebugDialog::updateControls()
{
    if (!m_openButton)
        return;

    const bool open = m_serial.isOpen();
    m_openButton->setText(open ? tr("关闭串口") : tr("打开串口"));

    for (QWidget *widget : {static_cast<QWidget *>(m_portCombo),
                            static_cast<QWidget *>(m_baudCombo),
                            static_cast<QWidget *>(m_dataBitsCombo),
                            static_cast<QWidget *>(m_parityCombo),
                            static_cast<QWidget *>(m_stopBitsCombo),
                            static_cast<QWidget *>(m_flowCombo),
                            static_cast<QWidget *>(m_refreshButton)}) {
        widget->setEnabled(!open);
    }

    m_dtrCheck->setEnabled(open);
    m_rtsCheck->setEnabled(open);
    m_sendButton->setEnabled(open);
    m_loopCheck->setEnabled(open);

    if (!open) {
        if (m_loopCheck->isChecked()) {
            QSignalBlocker blocker(m_loopCheck);
            m_loopCheck->setChecked(false);
        }
        if (m_loopTimer)
            m_loopTimer->stop();
    }
    m_loopIntervalSpin->setEnabled(open && m_loopCheck->isChecked());
}

void SerialDebugDialog::updateCounterLabel()
{
    if (m_counterLabel) {
        m_counterLabel->setText(
            tr("接收 %1 B · 发送 %2 B").arg(m_rxTotal).arg(m_txTotal));
    }
}

void SerialDebugDialog::clearCounters()
{
    m_rxTotal = 0;
    m_txTotal = 0;
    updateCounterLabel();
}

void SerialDebugDialog::setConnectionStatus(const QString &text, const QString &color)
{
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600;").arg(color));
    m_statusLabel->setToolTip(text);
}

void SerialDebugDialog::setMessage(const QString &text, bool error)
{
    m_messageLabel->setText(text);
    m_messageLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(error ? kRed : kGray));
}
