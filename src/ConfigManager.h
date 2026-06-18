#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include "Types.h"

class ConfigManager : public QObject {
    Q_OBJECT
public:
    explicit ConfigManager(QObject *parent = nullptr);

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

    // 0=linear, 1=log
    int  loadColorMapping() const;
    void saveColorMapping(int mode);
    bool loadColorClipping() const;
    void saveColorClipping(bool clip);

    bool loadSaveEnabled() const;
    void saveSaveEnabled(bool enabled);

    QString loadSavePath() const;
    void saveSavePath(const QString &path);

    QString loadSerialPort() const;
    void saveSerialPort(const QString &port);

    int loadBufferSizeMB() const;
    void saveBufferSizeMB(int mb);
    int loadNotifySizeKB() const;
    void saveNotifySizeKB(int kb);

    // Spiral parameters
    QString loadTrajectoryCsvPath() const;
    void saveTrajectoryCsvPath(const QString &path);

private:
    QSettings m_settings;
};
