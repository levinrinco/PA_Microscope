#include "ConfigManager.h"
#include "third_party/Spectrum/include/regs.h"
#include <QCoreApplication>

ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent)
    , m_settings(
        QCoreApplication::applicationDirPath() + "/PA_Microscope.ini",
        QSettings::IniFormat)
{
}

ScanParams ConfigManager::loadScanParams() const
{
    ScanParams p;
    p.depth        = m_settings.value("Scan/depth", 600).toInt();
    p.windowOffset = m_settings.value("Scan/window_offset", 200).toInt();
    p.segmentSize  = m_settings.value("Scan/segment_size", 4096).toInt();
    p.preTrigger   = m_settings.value("Scan/pre_trigger", 16).toInt();
    p.fifoMode     = m_settings.value("Scan/fifo_mode", 0x00000020).toInt();
    p.loopCount    = m_settings.value("Scan/loop_count", 0).toInt();
    p.channelMask  = m_settings.value("Scan/channel_mask", QVariant::fromValue(1ULL)).toULongLong();
    // Spiral params
    p.trajectoryCsvPath = m_settings.value("Scan/trajectory_csv", "").toString();
    p.imageWidth  = m_settings.value("Scan/image_width", 500).toInt();
    p.imageHeight = m_settings.value("Scan/image_height", 500).toInt();
    p.cscanUpdateInterval = m_settings.value("Scan/cscan_update_interval", 500).toInt();
    return p;
}

void ConfigManager::saveScanParams(const ScanParams &params)
{
    m_settings.setValue("Scan/depth", params.depth);
    m_settings.setValue("Scan/window_offset", params.windowOffset);
    m_settings.setValue("Scan/segment_size", params.segmentSize);
    m_settings.setValue("Scan/pre_trigger", params.preTrigger);
    m_settings.setValue("Scan/fifo_mode", params.fifoMode);
    m_settings.setValue("Scan/loop_count", params.loopCount);
    m_settings.setValue("Scan/channel_mask", static_cast<qulonglong>(params.channelMask));
    m_settings.setValue("Scan/trajectory_csv", params.trajectoryCsvPath);
    m_settings.setValue("Scan/image_width", params.imageWidth);
    m_settings.setValue("Scan/image_height", params.imageHeight);
    m_settings.setValue("Scan/cscan_update_interval", params.cscanUpdateInterval);
}

int ConfigManager::loadSampleRate() const
{
    return m_settings.value("DAQ/sample_rate", 250000000).toInt();
}

void ConfigManager::saveSampleRate(int rateHz)
{
    m_settings.setValue("DAQ/sample_rate", rateHz);
}

int ConfigManager::loadTriggerMode() const
{
    return m_settings.value("DAQ/trigger_mode", SPC_TM_POS).toInt();
}

void ConfigManager::saveTriggerMode(int mode)
{
    m_settings.setValue("DAQ/trigger_mode", mode);
}

int ConfigManager::loadTriggerExtLine() const
{
    return m_settings.value("DAQ/trigger_ext_line", 0).toInt();
}
void ConfigManager::saveTriggerExtLine(int extLine)
{
    m_settings.setValue("DAQ/trigger_ext_line", extLine);
}
int ConfigManager::loadTriggerLevel0() const
{
    return m_settings.value("DAQ/trigger_level0", 2000).toInt();
}
void ConfigManager::saveTriggerLevel0(int mv)
{
    m_settings.setValue("DAQ/trigger_level0", mv);
}
int ConfigManager::loadTriggerLevel1() const
{
    return m_settings.value("DAQ/trigger_level1", 0).toInt();
}
void ConfigManager::saveTriggerLevel1(int mv)
{
    m_settings.setValue("DAQ/trigger_level1", mv);
}
bool ConfigManager::loadTriggerCoupling() const
{
    return m_settings.value("DAQ/trigger_coupling_ac", false).toBool();
}
void ConfigManager::saveTriggerCoupling(bool ac)
{
    m_settings.setValue("DAQ/trigger_coupling_ac", ac);
}
int ConfigManager::loadTriggerDelay() const
{
    return m_settings.value("DAQ/trigger_delay", 0).toInt();
}
void ConfigManager::saveTriggerDelay(int samples)
{
    m_settings.setValue("DAQ/trigger_delay", samples);
}

int ConfigManager::loadClockMode() const
{
    return m_settings.value("DAQ/clock_mode", SPC_CM_INTPLL).toInt();
}

void ConfigManager::saveClockMode(int mode)
{
    m_settings.setValue("DAQ/clock_mode", mode);
}

int ConfigManager::loadClockDivider() const
{
    return m_settings.value("DAQ/clock_divider", 1).toInt();
}
void ConfigManager::saveClockDivider(int div)
{
    m_settings.setValue("DAQ/clock_divider", div);
}
int ConfigManager::loadRefClockFreq() const
{
    return m_settings.value("DAQ/ref_clock_mhz", 10).toInt();
}
void ConfigManager::saveRefClockFreq(int mhz)
{
    m_settings.setValue("DAQ/ref_clock_mhz", mhz);
}

// ---- Per-channel settings ----

static QString chKey(int ch, const char *suffix) {
    return QString("Ch%1/%2").arg(ch).arg(suffix);
}

int ConfigManager::loadChannelPath(int ch) const
{ return m_settings.value(chKey(ch, "input_path"), 0).toInt(); }
void ConfigManager::saveChannelPath(int ch, int path)
{ m_settings.setValue(chKey(ch, "input_path"), path); }

int ConfigManager::loadChannelRange(int ch) const
{ return m_settings.value(chKey(ch, "input_range"), 1000).toInt(); }
void ConfigManager::saveChannelRange(int ch, int rangeMv)
{ m_settings.setValue(chKey(ch, "input_range"), rangeMv); }

bool ConfigManager::loadChannelTermination(int ch) const
{ return m_settings.value(chKey(ch, "termination_50ohm"), false).toBool(); }
void ConfigManager::saveChannelTermination(int ch, bool fiftyOhm)
{ m_settings.setValue(chKey(ch, "termination_50ohm"), fiftyOhm); }

bool ConfigManager::loadChannelCoupling(int ch) const
{ return m_settings.value(chKey(ch, "coupling_ac"), false).toBool(); }
void ConfigManager::saveChannelCoupling(int ch, bool ac)
{ m_settings.setValue(chKey(ch, "coupling_ac"), ac); }

bool ConfigManager::loadChannelDiffMode(int ch) const
{ return m_settings.value(chKey(ch, "diff_mode"), false).toBool(); }
void ConfigManager::saveChannelDiffMode(int ch, bool diff)
{ m_settings.setValue(chKey(ch, "diff_mode"), diff); }

bool ConfigManager::loadChannelBandwidth(int ch) const
{ return m_settings.value(chKey(ch, "bw_limit"), false).toBool(); }
void ConfigManager::saveChannelBandwidth(int ch, bool limited)
{ m_settings.setValue(chKey(ch, "bw_limit"), limited); }

int ConfigManager::loadChannelOffset(int ch) const
{ return m_settings.value(chKey(ch, "offset_mv"), 0).toInt(); }
void ConfigManager::saveChannelOffset(int ch, int offsetMv)
{ m_settings.setValue(chKey(ch, "offset_mv"), offsetMv); }

bool ConfigManager::loadTriggerTermination() const
{ return m_settings.value("DAQ/trigger_term", false).toBool(); }
void ConfigManager::saveTriggerTermination(bool term)
{ m_settings.setValue("DAQ/trigger_term", term); }

bool ConfigManager::loadClockOutput() const
{ return m_settings.value("DAQ/clock_out", false).toBool(); }
void ConfigManager::saveClockOutput(bool out)
{ m_settings.setValue("DAQ/clock_out", out); }

int ConfigManager::loadColorMapping() const
{ return m_settings.value("Display/color_mapping", 0).toInt(); }
void ConfigManager::saveColorMapping(int mode)
{ m_settings.setValue("Display/color_mapping", mode); }

bool ConfigManager::loadColorClipping() const
{ return m_settings.value("Display/color_clipping", true).toBool(); }
void ConfigManager::saveColorClipping(bool clip)
{ m_settings.setValue("Display/color_clipping", clip); }

uint64_t ConfigManager::loadChannelMask() const
{
    return m_settings.value("DAQ/channel_mask", 1ULL).toULongLong();
}

void ConfigManager::saveChannelMask(uint64_t mask)
{
    m_settings.setValue("DAQ/channel_mask", static_cast<qulonglong>(mask));
}

bool ConfigManager::loadSaveEnabled() const
{
    return m_settings.value("Save/save_enabled", false).toBool();
}

void ConfigManager::saveSaveEnabled(bool enabled)
{
    m_settings.setValue("Save/save_enabled", enabled);
}

QString ConfigManager::loadSavePath() const
{
    return m_settings.value("Save/save_path", "E:/PAM_Data/").toString();
}

void ConfigManager::saveSavePath(const QString &path)
{
    m_settings.setValue("Save/save_path", path);
}

QString ConfigManager::loadSerialPort() const
{
    return m_settings.value("FPGA/serial_port", "COM3").toString();
}

void ConfigManager::saveSerialPort(const QString &port)
{
    m_settings.setValue("FPGA/serial_port", port);
}

int ConfigManager::loadBufferSizeMB() const
{
    return m_settings.value("DAQ/buffer_size_mb", 2048).toInt();
}

void ConfigManager::saveBufferSizeMB(int mb)
{
    m_settings.setValue("DAQ/buffer_size_mb", mb);
}

int ConfigManager::loadNotifySizeKB() const
{
    return m_settings.value("DAQ/notify_size_kb", 4096).toInt();
}

void ConfigManager::saveNotifySizeKB(int kb)
{
    m_settings.setValue("DAQ/notify_size_kb", kb);
}

// ---- Spiral parameters ----

QString ConfigManager::loadTrajectoryCsvPath() const
{ return m_settings.value("Scan/trajectory_csv", "").toString(); }
void ConfigManager::saveTrajectoryCsvPath(const QString &path)
{ m_settings.setValue("Scan/trajectory_csv", path); }
