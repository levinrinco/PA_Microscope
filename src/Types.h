#pragma once

#include <cstdint>
#include <QString>
#include <QMetaType>
#include <vector>

// The acquisition hardware is shared by both applications; only the way an
// A-line index is mapped to image space differs.
enum class ScanMode {
    Raster = 0,
    Trajectory = 1
};
Q_DECLARE_METATYPE(ScanMode)

// Stored data type for saved .dat / .npy files.
enum class DataFormat {
    Float64 = 0,
    Float32 = 1,
    Float16 = 2
};
Q_DECLARE_METATYPE(DataFormat)

// PCD point-cloud background filtering / vessel segmentation mode.
enum class PcdSegMode {
    Off = 0,      // keep every pixel-column peak
    Otsu = 1,     // automatic Otsu threshold on en-face peak map
    Manual = 2    // fixed threshold (fraction of global peak)
};

// ===== Scan parameters =====
struct ScanParams {
    ScanMode scanMode = ScanMode::Trajectory;

    // --- Raster scan fields ---
    int nFast = 1000;               // A-lines per fast-axis row (one B-scan)
    int nSlow = 1000;               // number of slow-axis rows

    int depth   = 600;              // window size — how many samples saved/displayed per Aline
    int windowOffset = 200;         // where the window starts within the full segment
    int segmentSize = 4096;         // total hardware samples per trigger (Seg)
    int preTrigger   = 16;          // pre-trigger samples within segment (Pre)
    int fifoMode     = 0x00000020;  // SPC_REC_FIFO_MULTI
    int loopCount    = 0;           // 0 = endless
    uint64_t channelMask = 0x00000001; // CHANNEL0

    // --- Spiral scan fields ---
    QString trajectoryCsvPath;       // 螺旋轨迹 CSV 文件路径
    int totalAlines = 0;             // 从 CSV 自动检测（只读）
    int trajectoryBscanStart = 1;    // 1-based acquisition ordinal, inclusive
    int trajectoryBscanEnd = 1000;   // inclusive; clamped to loaded frame length
    int imageWidth = 500;            // 输出网格像素宽度
    int imageHeight = 500;           // 输出网格像素高度
    double scanRadiusM = 0.005;      // 扫描半径（米），从 CSV 自动检测
    int cscanUpdateInterval = 500;   // 每 N 条 A-line 刷新一次 C-scan

    // --- Saved-file options (per mode) ---
    DataFormat dataFormat = DataFormat::Float64; // .dat / .npy element type
    bool saveDat  = false;   // raster: raw B-scan .dat
    bool savePng  = false;   // raster: final C-scan MIP .png
    bool saveDepthPng = false; // raster: depth-encoded C-scan .png
    bool saveNpy  = true;    // trajectory: gridded volume .npy
    bool savePcd  = false;   // trajectory: peak point cloud .pcd
    bool saveMip  = true;    // trajectory: MIP .png
    bool saveMipDepth = false; // trajectory: depth-encoded MIP .png
    bool saveDatTraj = false;// trajectory/spiral: raw per-frame waveform .dat (a volume of B-scans)
    PcdSegMode pcdSegMode = PcdSegMode::Otsu;   // vessel/background separation
    float pcdSegThreshold = 0.25f;              // fraction of global peak (manual mode)

    bool isRaster() const { return scanMode == ScanMode::Raster; }
    bool isTrajectory() const { return scanMode == ScanMode::Trajectory; }

    // ===== Per-mode output selection =====
    // This struct carries the fields of BOTH modes at once, so a flat
    // `saveDat || savePng || saveNpy || ...` is wrong: a stale toggle left over
    // from the inactive mode would request saving (and create a session folder)
    // for a scan that has no output enabled at all. Always ask these helpers
    // instead of testing the raw fields.
    bool rasterHasOutput() const { return saveDat || savePng || saveDepthPng; }
    bool trajectoryHasOutput() const
    {
        return saveNpy || savePcd || saveMip || saveDatTraj || saveMipDepth;
    }
    /// True when the ACTIVE mode has at least one output format enabled.
    bool hasAnyOutput() const
    {
        return isRaster() ? rasterHasOutput() : trajectoryHasOutput();
    }
};

// ===== Spectrum card info (simplified) =====
struct CardInfo {
    QString cardType;
    int serialNumber = 0;
    long long memSizeBytes = 0;     // Spectrum SDK uses long long (its int64)
    int functionType = 0;       // SPCM_TYPE_AI / AO / DI / DO / DIO
    int maxChannels = 0;
    int bytesPerSample = 2;     // int16 = 2
    long long maxSamplerate = 0;
    long long minSamplerate = 0;
    bool isOpen = false;
};

// ===== FIFO status snapshot =====
struct FifoStatus {
    int32_t m2Status = 0;
    long long availUserBytes = 0;
    long long availUserPos = 0;
    int32_t fillSizePromille = 0;
};

// ===== Scan state =====
enum class ScanState {
    Idle,
    Armed,
    Scanning,
    Done,
    Error,
    Stopping
};
Q_DECLARE_METATYPE(ScanState)

// ===== Spiral trajectory types =====

// Bilinear interpolation mapping: one A-line → 4 surrounding pixels + weights
struct MapInfo {
    int idx00, idx01, idx10, idx11;  // 4 pixel indices in flat grid [imageW * imageH]
    float w00, w01, w10, w11;        // bilinear weights (sum ≈ 1)
};

// Single trajectory point
struct TrajectoryPoint {
    float x_m = 0.0f;    // X position in meters
    float y_m = 0.0f;    // Y position in meters
    float r_m = 0.0f;    // radial distance
    float theta = 0.0f;  // angle in radians
};

// Triple-buffer MIP accumulator slot for non-blocking frame handoff
struct MipAccum {
    std::vector<float> sum;      // [gridSize] accumulated peak * weight
    std::vector<float> weight;   // [gridSize] accumulated weight
    // Depth encoding: accumulated (peak-depth * peak * weight). Dividing by
    // `sum` (not by `weight`) yields the intensity-weighted mean depth of the
    // A-lines that landed on this pixel — the same "where is the signal"
    // answer the raster path gets from its own per-pixel accumulation, at the
    // cost of one more multiply-add per interpolated corner.
    std::vector<float> depthSum; // [gridSize] accumulated peak-depth * peak * weight
};
