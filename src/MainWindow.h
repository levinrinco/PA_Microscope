#pragma once

#include <QMainWindow>
#include <QThread>
#include <QTimer>
#include <vector>
#include "Types.h"
#include "DepthEncode.h"

class AlineWidget;
class BscanWidget;
class CscanWidget;
class DepthRulerWidget;
class ColorBar;
class DepthColorBar;
class SpectrumFifo;
class FpgaSerial;
class ScanController;
class DataPipeline;
class FileWriter;
class ConfigManager;
class SerialDebugDialog;

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
    void onScanModeChanged(int index);

    // Serial port
    void onSerialPortChanged(int index);
    void refreshSerialPorts();

    // Browse paths
    void onBrowseSavePath();
    void onBrowseCsvPath();

    // Menu actions
    void onSettingsCard();
    void onDisplaySettings();
    void onSerialDebugAssistant();
    void onAbout();

private:
    void setupStatusBar();
    void setupConnections();
    void initializeCard();
    void applyCardConfig();          // re-apply per-mode DAQ settings to the card
    bool validateAcquisitionParams(QString &error) const;
    void spawnPipeline();
    void refreshScanDisplay();
    void saveSettings();
    void loadSettings();
    void updateScanModeUi();
    void syncTrajectoryBscanControls();
    void updateTrajectoryBscanRange(bool startChanged);
    void loadParamsForMode(ScanMode mode);   // load + apply the mode's param set
    void persistScanParams();                // instant per-control auto-save

    void positionColorBars();
    void updateButtonStates(ScanState state);
    void showError(const QString &title, const QString &detail);
    void logStatus(const QString &msg);

    // Depth encoding
    DepthConfig loadDepthConfig() const;   // display settings -> config struct
    void applyDepthSettings();             // push settings to widget/pipeline/bar
    void setDepthEncodingEnabled(bool on); // live toggle, persists
    void applyDepthMap(int frameIdx, const std::vector<float> &depthMap,
                       int imageW, int imageH);
    void updateLastDepthImage();           // re-render cached map, no new frame

    static QString decodeM2Status(int32_t status);

    // .ui generated
    Ui::MainWindow *ui;

    // Status bar
    QLabel *m_statusScanProgress;
    QLabel *m_statusAlineCount;
    QLabel *m_statusCardConn;

    // 串口调试助手是模型对话框，复用同一个实例；关闭时释放串口。
    SerialDebugDialog *m_serialDebug = nullptr;

    // Core objects
    SpectrumFifo *m_fifo = nullptr;
    FpgaSerial *m_fpga = nullptr;
    ScanController *m_scanCtrl = nullptr;
    DataPipeline *m_pipeline = nullptr;
    FileWriter *m_writer = nullptr;
    ConfigManager *m_config = nullptr;
    ColorBar *m_cscanBar = nullptr;
    ColorBar *m_bscanBar = nullptr;
    DepthColorBar *m_depthBar = nullptr;
    DepthRulerWidget *m_bscanDepthRuler = nullptr;

    QThread *m_workerThread = nullptr;
    QTimer *m_alineTimer = nullptr;
    QTimer *m_serialRefreshTimer = nullptr;

    ScanParams m_scanParams;

    // Trajectory data — owned here so it outlives the scan
    std::vector<TrajectoryPoint> m_trajectoryPoints;
    std::vector<MapInfo> m_trajectoryMapInfo;

    bool m_firstDataSent = false;
    int64_t m_savedItemCount = 0;
    uint64_t m_pipelineGeneration = 0;
    bool m_configReady = false;   // guards auto-persist during construction

    // Last normalized MIP frame — kept so color settings can be re-applied instantly
    std::vector<float> m_lastMip;
    float m_lastMipMax = 0.0f;
    void recolorLastMip();

    // Last depth map of a completed trajectory frame. Kept next to m_lastMip so
    // a colormap or range change can repaint the depth image without waiting
    // for the next frame.
    std::vector<float> m_lastDepthMap;
    int m_lastDepthW = 0, m_lastDepthH = 0;
    bool m_depthEncode = false;   // mirror of Display/depth_encode
};
