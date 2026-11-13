#pragma once

// ---------------------------------------------------------------------------
// SerialDebugDialog —— 串口调试助手
//
// 独立于 FpgaSerial 的串口终端，用于现场排查波特率、接线、协议字节和
// 外设应答。对话框本身不会发送采集卡的 0xF1/0xF0 启停命令；只有用户
// 显式“释放采集卡串口”时，主窗口才会在断开前发送一次 F0 停止命令。
//
// 与采集卡串口的边界：
//   · 采集程序占用的端口在这里会被拒绝打开。同一个 tty 上两个读者会把
//     应答字节分流，表现为“采集卡偶尔收不到回包”一类很难定位的故障。
//   · 反过来，MainWindow 也不会在调试助手占用端口时再打开同一个 tty。
//
// 接收缓冲有上限（512 KB），超过后清空并继续，避免长时间调试吃满内存。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QDialog>
#include <QSerialPort>
#include <QStringConverter>

#include <deque>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

class SerialDebugDialog : public QDialog {
    Q_OBJECT
public:
    explicit SerialDebugDialog(QWidget *parent = nullptr);
    ~SerialDebugDialog() override;

    /// 采集卡当前占用的串口名（空表示未占用）。只用于提示和拒绝冲突打开。
    void setAcquisitionPort(const QString &portName);

    bool isPortOpen() const;
    QString portName() const;

    /// 把串口名规范化为可比较的设备标识：
    /// ttyUSB0 / /dev/ttyUSB0 / /dev/serial/by-id/... 指向同一 tty；
    /// Windows 上 COM3 与 \\.\COM3 也视为同一端口。
    static QString normalizedPortName(const QString &name);

signals:
    /// 打开/关闭串口后发出，供主窗口判断端口是否会被两边同时占用。
    void portStateChanged();
    /// 用户要求释放采集卡串口。主窗口确认当前没有扫描后再断开。
    void acquisitionPortReleaseRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void refreshPorts();
    void togglePort();
    void sendData();
    void readAvailable();
    void handlePortError(QSerialPort::SerialPortError error);
    void clearReceive();
    void clearCounters();
    void rebuildReceiveView();
    void updateLoopSend();
    void updateControls();
    void updateCounterLabel();
    void syncDisplayOptions();
    void syncSendOptions();

private:
    void buildUi();
    void openPort();
    void closePort();

    /// 把一帧新收到的字节追加到显示缓冲并立即渲染。
    void appendReceived(const QByteArray &bytes);
    /// 渲染新到达的一个接收块（历史重放由 rebuildReceiveView 负责）。
    void renderChunk(const QByteArray &bytes, qint64 timestampMs);
    void insertRawText(const QString &text);
    void setConnectionStatus(const QString &text, const QString &color);
    void setMessage(const QString &text, bool error);
    QString selectedPort() const;
    int selectedBaud() const;
    bool composePayload(QByteArray *payload, QString *error) const;

    struct RxChunk {
        qint64 timestampMs = 0;
        QByteArray bytes;
    };

    QSerialPort m_serial;
    QString m_acquisitionPort;
    std::deque<RxChunk> m_rxChunks;
    qint64 m_rxBytes = 0;      ///< 当前显示缓冲字节数；超过上限会整体清空
    qint64 m_rxTotal = 0;      ///< 自上次计数清零以来的接收字节数
    qint64 m_txTotal = 0;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    bool m_opening = false;    ///< open() 内部的错误由 openPort 统一报告
    bool m_closing = false;

    // ---- 端口 ----
    QComboBox *m_portCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QComboBox *m_dataBitsCombo = nullptr;
    QComboBox *m_parityCombo = nullptr;
    QComboBox *m_stopBitsCombo = nullptr;
    QComboBox *m_flowCombo = nullptr;
    QPushButton *m_openButton = nullptr;
    QCheckBox *m_dtrCheck = nullptr;
    QCheckBox *m_rtsCheck = nullptr;
    QLabel *m_statusLabel = nullptr;
    QWidget *m_acquisitionRow = nullptr;
    QLabel *m_acquisitionHint = nullptr;
    QPushButton *m_releaseAcquisitionButton = nullptr;

    // ---- 接收 ----
    QCheckBox *m_hexDisplayCheck = nullptr;
    QCheckBox *m_timestampCheck = nullptr;
    QCheckBox *m_autoScrollCheck = nullptr;
    QPushButton *m_clearReceiveButton = nullptr;
    QPlainTextEdit *m_receiveView = nullptr;

    // ---- 发送 ----
    QPlainTextEdit *m_sendEdit = nullptr;
    QCheckBox *m_hexSendCheck = nullptr;
    QComboBox *m_lineEndingCombo = nullptr;
    QCheckBox *m_loopCheck = nullptr;
    QSpinBox *m_loopIntervalSpin = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_clearSendButton = nullptr;
    QTimer *m_loopTimer = nullptr;

    // ---- 底部状态 ----
    QLabel *m_messageLabel = nullptr;
    QLabel *m_counterLabel = nullptr;
    QPushButton *m_clearCountersButton = nullptr;
};
