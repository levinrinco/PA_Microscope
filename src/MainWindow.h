#pragma once

#include <QMainWindow>
#include <QThread>
#include <QTimer>
#include "Types.h"

class AlineWidget;
class CscanWidget;
class ColorBar;
class SpectrumFifo;
class FpgaSerial;
class ScanController;
class DataPipeline;
class FileWriter;
class ConfigManager;

class QLabel;

namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    // Card control
    void onCardStart();
    void onCardStop();
    void onForceTrigger();
    void onCardInfoReady(const CardInfo &info);

    // FPGA
    void onFpgaStart();
    void onFpgaStop();

    // Scan lifecycle
    void onScanStart();
    void onScanStop();
    void onScanStarted();
    void onScanFinished();
    void onScanStateChanged(ScanState state);
    void onScanError(const QString &msg);

    // Data display
    void onScanProgress(int current, int total);

    // Serial port
    void onSerialPortChanged(int index);
    void refreshSerialPorts();

    // Browse paths
    void onBrowseSavePath();
    void onBrowseCsvPath();

    // Menu actions
    void onSettingsCard();
    void onDisplaySettings();
    void onAbout();

private:
    void setupStatusBar();
    void setupConnections();
    void initializeCard();
    void spawnPipeline();
    void saveSettings();
    void loadSettings();

    void positionColorBars();
    void updateButtonStates(ScanState state);
    void showError(const QString &title, const QString &detail);
    void logStatus(const QString &msg);

    static QString decodeM2Status(int32_t status);

    // .ui generated
    Ui::MainWindow *ui;

    // Status bar
    QLabel *m_statusScanProgress;
    QLabel *m_statusAlineCount;
    QLabel *m_statusCardConn;

    // Core objects
    SpectrumFifo *m_fifo = nullptr;
    FpgaSerial *m_fpga = nullptr;
    ScanController *m_scanCtrl = nullptr;
    DataPipeline *m_pipeline = nullptr;
    FileWriter *m_writer = nullptr;
    ConfigManager *m_config = nullptr;
    ColorBar *m_cscanBar = nullptr;

    QThread *m_workerThread = nullptr;
    QTimer *m_alineTimer = nullptr;
    QTimer *m_serialRefreshTimer = nullptr;

    ScanParams m_scanParams;

    // Trajectory data — owned here so it outlives the scan
    std::vector<TrajectoryPoint> m_trajectoryPoints;
    std::vector<MapInfo> m_trajectoryMapInfo;

    bool m_firstDataSent = false;
};
