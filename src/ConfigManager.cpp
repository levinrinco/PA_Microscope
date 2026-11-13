#include "ConfigManager.h"
#include "DepthEncode.h"
#include "third_party/Spectrum/include/regs.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <algorithm>

ConfigManager::ConfigManager(QObject *parent, const QString &settingsPath)
    : QObject(parent)
    , m_settings(
        settingsPath.isEmpty()
            ? QCoreApplication::applicationDirPath() + "/PA_Microscope.ini"
            : settingsPath,
        QSettings::IniFormat)
{
    migrateDefaults();
}

// One-shot migration for defaults that changed after the first release.
//
// The stored data format default moved from float32 to float64. Changing only
// the fallback in loadScanParams() would have no effect on any machine that has
// already run the program, because the value is persisted and QSettings falls
// back only when the key is absent. So the old default is rewritten once,
// guarded by a schema version. A user who deliberately picked float16 is left
// alone: only a value still sitting at the previous default is migrated.
void ConfigManager::migrateDefaults()
{
    constexpr int kConfigSchema = 2;
    const int version = m_settings.value(QStringLiteral("Meta/schema_version"), 1).toInt();
    if (version >= kConfigSchema) return;

    const int oldFloat32 = static_cast<int>(DataFormat::Float32);
    const int newFloat64 = static_cast<int>(DataFormat::Float64);
    for (const char *section : {"Raster", "Trajectory"}) {
        const QString key = QStringLiteral("%1/data_format").arg(QLatin1String(section));
        if (m_settings.contains(key) && m_settings.value(key).toInt() == oldFloat32)
            m_settings.setValue(key, newFloat64);
    }
    m_settings.setValue(QStringLiteral("Meta/schema_version"), kConfigSchema);
    m_settings.sync();
}

QString ConfigManager::modeSec(ScanMode mode)
{
    return (mode == ScanMode::Raster) ? QStringLiteral("Raster")
                                      : QStringLiteral("Trajectory");
}

QString ConfigManager::sec() const
{
    return modeSec(m_activeMode);
}

QString ConfigManager::secKey(const QString &key) const
{
    return sec() + "/" + key;
}

void ConfigManager::setActiveMode(ScanMode mode)
{
    m_activeMode = mode;
}

// ===== Scan-mode parameters (mode-scoped) =====

ScanParams ConfigManager::loadScanParams() const
{
    ScanParams p;
    p.scanMode    = loadScanMode();
    p.nFast       = m_settings.value(secKey("fast_points"), 1000).toInt();
    p.nSlow       = m_settings.value(secKey("slow_points"), 1000).toInt();
    p.depth        = m_settings.value(secKey("depth"), 600).toInt();
    p.windowOffset = m_settings.value(secKey("window_offset"), 200).toInt();
    p.segmentSize  = m_settings.value(secKey("segment_size"), 4096).toInt();
    p.preTrigger   = m_settings.value(secKey("pre_trigger"), 16).toInt();
    p.fifoMode     = m_settings.value(secKey("fifo_mode"), 0x00000020).toInt();
    p.loopCount    = m_settings.value(secKey("loop_count"), 0).toInt();
    p.channelMask  = m_settings.value(secKey("channel_mask"), QVariant::fromValue(1ULL)).toULongLong();
    // Trajectory-scoped fields
    p.trajectoryCsvPath = m_settings.value(secKey("trajectory_csv"), "").toString();
    p.imageWidth  = m_settings.value(secKey("image_width"), 500).toInt();
    p.imageHeight = m_settings.value(secKey("image_height"), 500).toInt();
    p.cscanUpdateInterval = m_settings.value(secKey("cscan_update_interval"), 500).toInt();
    p.trajectoryBscanStart = std::max(1, m_settings.value(secKey("bscan_start"), 1).toInt());
    p.trajectoryBscanEnd = std::max(p.trajectoryBscanStart,
        m_settings.value(secKey("bscan_end"), 1000).toInt());
    // Saved-file options
    p.dataFormat = static_cast<DataFormat>(
        m_settings.value(secKey("data_format"), static_cast<int>(DataFormat::Float64)).toInt());
    p.saveDat = m_settings.value(secKey("save_dat"), true).toBool();
    p.savePng = m_settings.value(secKey("save_png"), false).toBool();
    p.saveDepthPng = m_settings.value(secKey("save_depth_png"), false).toBool();
    p.saveNpy = m_settings.value(secKey("save_npy"), true).toBool();
    p.savePcd = m_settings.value(secKey("save_pcd"), false).toBool();
    p.saveMip = m_settings.value(secKey("save_mip"), true).toBool();
    p.saveMipDepth = m_settings.value(secKey("save_mip_depth"), false).toBool();
    p.saveDatTraj = m_settings.value(secKey("save_dat_traj"), false).toBool();
    p.pcdSegMode = static_cast<PcdSegMode>(
        m_settings.value(secKey("pcd_seg_mode"), static_cast<int>(PcdSegMode::Otsu)).toInt());
    p.pcdSegThreshold = m_settings.value(secKey("pcd_seg_threshold"), 0.25).toFloat();
    return p;
}

void ConfigManager::saveScanParams(const ScanParams &params)
{
    saveScanMode(params.scanMode);
    m_settings.setValue(secKey("fast_points"), params.nFast);
    m_settings.setValue(secKey("slow_points"), params.nSlow);
    m_settings.setValue(secKey("depth"), params.depth);
    m_settings.setValue(secKey("window_offset"), params.windowOffset);
    m_settings.setValue(secKey("segment_size"), params.segmentSize);
    m_settings.setValue(secKey("pre_trigger"), params.preTrigger);
    m_settings.setValue(secKey("fifo_mode"), params.fifoMode);
    m_settings.setValue(secKey("loop_count"), params.loopCount);
    m_settings.setValue(secKey("channel_mask"), static_cast<qulonglong>(params.channelMask));
    m_settings.setValue(secKey("trajectory_csv"), params.trajectoryCsvPath);
    m_settings.setValue(secKey("image_width"), params.imageWidth);
    m_settings.setValue(secKey("image_height"), params.imageHeight);
    m_settings.setValue(secKey("cscan_update_interval"), params.cscanUpdateInterval);
    m_settings.setValue(secKey("bscan_start"), params.trajectoryBscanStart);
    m_settings.setValue(secKey("bscan_end"), params.trajectoryBscanEnd);
    m_settings.setValue(secKey("data_format"), static_cast<int>(params.dataFormat));
    m_settings.setValue(secKey("save_dat"), params.saveDat);
    m_settings.setValue(secKey("save_png"), params.savePng);
    m_settings.setValue(secKey("save_depth_png"), params.saveDepthPng);
    m_settings.setValue(secKey("save_npy"), params.saveNpy);
    m_settings.setValue(secKey("save_pcd"), params.savePcd);
    m_settings.setValue(secKey("save_mip"), params.saveMip);
    m_settings.setValue(secKey("save_mip_depth"), params.saveMipDepth);
    m_settings.setValue(secKey("save_dat_traj"), params.saveDatTraj);
    m_settings.setValue(secKey("pcd_seg_mode"), static_cast<int>(params.pcdSegMode));
    m_settings.setValue(secKey("pcd_seg_threshold"), params.pcdSegThreshold);
}

int ConfigManager::loadSampleRate() const
{
    return m_settings.value(secKey("sample_rate"), 250000000).toInt();
}

void ConfigManager::saveSampleRate(int rateHz)
{
    m_settings.setValue(secKey("sample_rate"), rateHz);
}

int ConfigManager::loadTriggerMode() const
{
    return m_settings.value(secKey("trigger_mode"), SPC_TM_POS).toInt();
}

void ConfigManager::saveTriggerMode(int mode)
{
    m_settings.setValue(secKey("trigger_mode"), mode);
}

int ConfigManager::loadTriggerExtLine() const
{
    return m_settings.value(secKey("trigger_ext_line"), 0).toInt();
}
void ConfigManager::saveTriggerExtLine(int extLine)
{
    m_settings.setValue(secKey("trigger_ext_line"), extLine);
}
int ConfigManager::loadTriggerLevel0() const
{
    return m_settings.value(secKey("trigger_level0"), 2000).toInt();
}
void ConfigManager::saveTriggerLevel0(int mv)
{
    m_settings.setValue(secKey("trigger_level0"), mv);
}
int ConfigManager::loadTriggerLevel1() const
{
    return m_settings.value(secKey("trigger_level1"), 0).toInt();
}
void ConfigManager::saveTriggerLevel1(int mv)
{
    m_settings.setValue(secKey("trigger_level1"), mv);
}
bool ConfigManager::loadTriggerCoupling() const
{
    return m_settings.value(secKey("trigger_coupling_ac"), false).toBool();
}
void ConfigManager::saveTriggerCoupling(bool ac)
{
    m_settings.setValue(secKey("trigger_coupling_ac"), ac);
}
int ConfigManager::loadTriggerDelay() const
{
    return m_settings.value(secKey("trigger_delay"), 0).toInt();
}
void ConfigManager::saveTriggerDelay(int samples)
{
    m_settings.setValue(secKey("trigger_delay"), samples);
}

int ConfigManager::loadClockMode() const
{
    return m_settings.value(secKey("clock_mode"), SPC_CM_INTPLL).toInt();
}

void ConfigManager::saveClockMode(int mode)
{
    m_settings.setValue(secKey("clock_mode"), mode);
}

int ConfigManager::loadClockDivider() const
{
    return m_settings.value(secKey("clock_divider"), 1).toInt();
}
void ConfigManager::saveClockDivider(int div)
{
    m_settings.setValue(secKey("clock_divider"), div);
}
int ConfigManager::loadRefClockFreq() const
{
    return m_settings.value(secKey("ref_clock_mhz"), 10).toInt();
}
void ConfigManager::saveRefClockFreq(int mhz)
{
    m_settings.setValue(secKey("ref_clock_mhz"), mhz);
}

// ---- Per-channel settings ----

static QString chKeyMode(int ch, const char *suffix, const QString &sec)
{
    return QString("%1/Ch%2/%3").arg(sec).arg(ch).arg(suffix);
}

int ConfigManager::loadChannelPath(int ch) const
{ return m_settings.value(chKeyMode(ch, "input_path", sec()), 0).toInt(); }
void ConfigManager::saveChannelPath(int ch, int path)
{ m_settings.setValue(chKeyMode(ch, "input_path", sec()), path); }

int ConfigManager::loadChannelRange(int ch) const
{ return m_settings.value(chKeyMode(ch, "input_range", sec()), 1000).toInt(); }
void ConfigManager::saveChannelRange(int ch, int rangeMv)
{ m_settings.setValue(chKeyMode(ch, "input_range", sec()), rangeMv); }

bool ConfigManager::loadChannelTermination(int ch) const
{ return m_settings.value(chKeyMode(ch, "termination_50ohm", sec()), false).toBool(); }
void ConfigManager::saveChannelTermination(int ch, bool fiftyOhm)
{ m_settings.setValue(chKeyMode(ch, "termination_50ohm", sec()), fiftyOhm); }

bool ConfigManager::loadChannelCoupling(int ch) const
{ return m_settings.value(chKeyMode(ch, "coupling_ac", sec()), false).toBool(); }
void ConfigManager::saveChannelCoupling(int ch, bool ac)
{ m_settings.setValue(chKeyMode(ch, "coupling_ac", sec()), ac); }

bool ConfigManager::loadChannelDiffMode(int ch) const
{ return m_settings.value(chKeyMode(ch, "diff_mode", sec()), false).toBool(); }
void ConfigManager::saveChannelDiffMode(int ch, bool diff)
{ m_settings.setValue(chKeyMode(ch, "diff_mode", sec()), diff); }

bool ConfigManager::loadChannelBandwidth(int ch) const
{ return m_settings.value(chKeyMode(ch, "bw_limit", sec()), false).toBool(); }
void ConfigManager::saveChannelBandwidth(int ch, bool limited)
{ m_settings.setValue(chKeyMode(ch, "bw_limit", sec()), limited); }

int ConfigManager::loadChannelOffset(int ch) const
{ return m_settings.value(chKeyMode(ch, "offset_mv", sec()), 0).toInt(); }
void ConfigManager::saveChannelOffset(int ch, int offsetMv)
{ m_settings.setValue(chKeyMode(ch, "offset_mv", sec()), offsetMv); }

bool ConfigManager::loadTriggerTermination() const
{ return m_settings.value(secKey("trigger_term"), false).toBool(); }
void ConfigManager::saveTriggerTermination(bool term)
{ m_settings.setValue(secKey("trigger_term"), term); }

bool ConfigManager::loadClockOutput() const
{ return m_settings.value(secKey("clock_out"), false).toBool(); }
void ConfigManager::saveClockOutput(bool out)
{ m_settings.setValue(secKey("clock_out"), out); }

uint64_t ConfigManager::loadChannelMask() const
{
    return m_settings.value(secKey("channel_mask"), 1ULL).toULongLong();
}

void ConfigManager::saveChannelMask(uint64_t mask)
{
    m_settings.setValue(secKey("channel_mask"), static_cast<qulonglong>(mask));
}

int ConfigManager::loadBufferSizeMB() const
{ return m_settings.value(secKey("buffer_size_mb"), 2048).toInt(); }
void ConfigManager::saveBufferSizeMB(int mb)
{ m_settings.setValue(secKey("buffer_size_mb"), mb); }

int ConfigManager::loadNotifySizeKB() const
{ return m_settings.value(secKey("notify_size_kb"), 4096).toInt(); }
void ConfigManager::saveNotifySizeKB(int kb)
{ m_settings.setValue(secKey("notify_size_kb"), kb); }

// ---- Scan-mode helpers ----

ScanMode ConfigManager::loadScanMode() const
{
    // The scan mode itself is global (the UI shows which mode is active).
    const int value = m_settings.value("Scan/mode", static_cast<int>(ScanMode::Trajectory)).toInt();
    return value == static_cast<int>(ScanMode::Raster)
        ? ScanMode::Raster : ScanMode::Trajectory;
}

void ConfigManager::saveScanMode(ScanMode mode)
{ m_settings.setValue("Scan/mode", static_cast<int>(mode)); }

QString ConfigManager::loadTrajectoryCsvPath() const
{ return m_settings.value(secKey("trajectory_csv"), "").toString(); }
void ConfigManager::saveTrajectoryCsvPath(const QString &path)
{ m_settings.setValue(secKey("trajectory_csv"), path); }

// ===== Display settings (global) =====

int ConfigManager::loadColorMapping() const
{ return m_settings.value("Display/color_mapping", 0).toInt(); }
void ConfigManager::saveColorMapping(int mode)
{ m_settings.setValue("Display/color_mapping", mode); }

bool ConfigManager::loadColorClipping() const
{ return m_settings.value("Display/color_clipping", true).toBool(); }
void ConfigManager::saveColorClipping(bool clip)
{ m_settings.setValue("Display/color_clipping", clip); }

int ConfigManager::loadColorMap() const
{ return m_settings.value("Display/color_map", 0).toInt(); }
void ConfigManager::saveColorMap(int colormap)
{ m_settings.setValue("Display/color_map", colormap); }

// Depth encoding is a rendering choice, not an acquisition parameter, so it is
// stored globally next to the other display settings instead of per scan mode.
bool ConfigManager::loadDepthEncode() const
{ return m_settings.value("Display/depth_encode", false).toBool(); }
void ConfigManager::saveDepthEncode(bool on)
{ m_settings.setValue("Display/depth_encode", on); }

int ConfigManager::loadDepthDisplayMode() const
{ return m_settings.value("Display/depth_display_mode",
                         static_cast<int>(DepthDisplayMode::Modulated)).toInt(); }
void ConfigManager::saveDepthDisplayMode(int mode)
{ m_settings.setValue("Display/depth_display_mode", mode); }

int ConfigManager::loadDepthColorMap() const
{ return m_settings.value("Display/depth_color_map", 0).toInt(); }
void ConfigManager::saveDepthColorMap(int colormap)
{ m_settings.setValue("Display/depth_color_map", colormap); }

int ConfigManager::loadDepthMaxIndex() const
{ return m_settings.value("Display/depth_max_index", 0).toInt(); }
void ConfigManager::saveDepthMaxIndex(int maxIndex)
{ m_settings.setValue("Display/depth_max_index", maxIndex); }

int ConfigManager::loadDepthGatePercent() const
{ return m_settings.value("Display/depth_gate_percent", 5).toInt(); }
void ConfigManager::saveDepthGatePercent(int percent)
{ m_settings.setValue("Display/depth_gate_percent",
                      std::clamp(percent, 0, 100)); }

// ===== Save settings (global) =====

QString ConfigManager::loadSavePath() const
{
    // Default is platform-appropriate (Docs/PAM_Data), not a stale Windows path.
    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + "/PAM_Data/";
    return m_settings.value("Save/save_path", fallback).toString();
}

void ConfigManager::saveSavePath(const QString &path)
{
    m_settings.setValue("Save/save_path", path);
}

QString ConfigManager::loadSerialPort() const
{
    // Empty default: on Linux the COM-style names are meaningless; leave it to
    // auto-detection (refreshSerialPorts picks the first available port).
    return m_settings.value("FPGA/serial_port", "").toString();
}

void ConfigManager::saveSerialPort(const QString &port)
{
    m_settings.setValue("FPGA/serial_port", port);
}
