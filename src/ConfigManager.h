#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include "Types.h"

/// All scan / DAQ / channel settings are persisted per scan mode
/// (Raster / Trajectory) so the two modes keep fully independent parameter
/// sets, including the hardware-level (DAQ) settings. Environment-level
/// values — save directory, master save enable, display colors, serial port —
/// stay global.
///
/// Use setActiveMode() once per mode switch; every load/save call then
/// reads/writes the section of the active mode. No load/save buttons are
/// required: MainWindow persists on every control change.
class ConfigManager : public QObject {
    Q_OBJECT
public:
    /// @param settingsPath optional override (tests use a temp file); empty =
    ///                     <applicationDir>/PA_Microscope.ini
    explicit ConfigManager(QObject *parent = nullptr,
                           const QString &settingsPath = QString());

    /// Select which mode's parameter set is read/written from now on.
    void setActiveMode(ScanMode mode);

    // ---- Scan-mode parameters (mode-scoped) ----
    ScanParams loadScanParams() const;
    void saveScanParams(const ScanParams &params);

    int loadSampleRate() const;
    void saveSampleRate(int rateHz);

    int loadTriggerMode() const;
    void saveTriggerMode(int mode);
    int loadTriggerExtLine() const;
    void saveTriggerExtLine(int extLine);
    int loadTriggerLevel0() const;
    void saveTriggerLevel0(int mv);
    int loadTriggerLevel1() const;
    void saveTriggerLevel1(int mv);
    bool loadTriggerCoupling() const;
    void saveTriggerCoupling(bool ac);
    int loadTriggerDelay() const;
    void saveTriggerDelay(int samples);

    int loadClockMode() const;
    void saveClockMode(int mode);
    int loadClockDivider() const;
    void saveClockDivider(int div);
    int loadRefClockFreq() const;
    void saveRefClockFreq(int mhz);

    // Per-channel settings
    int  loadChannelPath(int ch) const;
    void saveChannelPath(int ch, int path);
    int  loadChannelRange(int ch) const;
    void saveChannelRange(int ch, int rangeMv);
    bool loadChannelTermination(int ch) const;
    void saveChannelTermination(int ch, bool fiftyOhm);
    bool loadChannelCoupling(int ch) const;
    void saveChannelCoupling(int ch, bool ac);
    bool loadChannelDiffMode(int ch) const;
    void saveChannelDiffMode(int ch, bool diff);
    bool loadChannelBandwidth(int ch) const;
    void saveChannelBandwidth(int ch, bool limited);
    int  loadChannelOffset(int ch) const;
    void saveChannelOffset(int ch, int offsetMv);

    uint64_t loadChannelMask() const;
    void saveChannelMask(uint64_t mask);

    bool loadTriggerTermination() const;
    void saveTriggerTermination(bool term);
    bool loadClockOutput() const;
    void saveClockOutput(bool out);

    int loadBufferSizeMB() const;
    void saveBufferSizeMB(int mb);
    int loadNotifySizeKB() const;
    void saveNotifySizeKB(int kb);

    // Scan-mode (mode-scoped, kept here for UI convenience)
    ScanMode loadScanMode() const;
    void saveScanMode(ScanMode mode);

    QString loadTrajectoryCsvPath() const;
    void saveTrajectoryCsvPath(const QString &path);

    // ---- Display settings (global) ----
    // 0=linear, 1=log
    int  loadColorMapping() const;
    void saveColorMapping(int mode);
    bool loadColorClipping() const;
    void saveColorClipping(bool clip);

    // 0=hot, 1=grayscale
    int  loadColorMap() const;
    void saveColorMap(int colormap);

    // ---- Depth encoding (global display settings) ----
    // The depth PNG save toggles live in ScanParams (they are per-mode output
    // selection); everything here is how depth is DISPLAYED.
    bool loadDepthEncode() const;
    void saveDepthEncode(bool on);
    /// 0 = pure depth colors, 1 = depth hue modulated by intensity brightness.
    int  loadDepthDisplayMode() const;
    void saveDepthDisplayMode(int mode);
    /// 0=turbo, 1=jet, 2=cyan/magenta.
    int  loadDepthColorMap() const;
    void saveDepthColorMap(int colormap);
    /// Depth index at the far end of the colormap; <= 0 = full window.
    int  loadDepthMaxIndex() const;
    void saveDepthMaxIndex(int maxIndex);
    /// Fraction of the en-face maximum below which a pixel counts as background
    /// and is painted black. Stored as an integer percent for a rounder INI.
    int  loadDepthGatePercent() const;
    void saveDepthGatePercent(int percent);

    // ---- Save settings (global) ----
    QString loadSavePath() const;
    void saveSavePath(const QString &path);

    // ---- FPGA (global) ----
    QString loadSerialPort() const;
    void saveSerialPort(const QString &port);

private:
    QSettings m_settings;
    ScanMode m_activeMode = ScanMode::Trajectory;
    static QString modeSec(ScanMode mode);
    QString sec() const;                 // section prefix for the active mode
    QString secKey(const QString &key) const;
    void migrateDefaults();              // one-shot rewrite of changed defaults
};
