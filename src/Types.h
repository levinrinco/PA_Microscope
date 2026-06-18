#pragma once

#include <cstdint>
#include <QString>
#include <QMetaType>
#include <vector>

// ===== Scan parameters =====
struct ScanParams {
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
    int imageWidth = 500;            // 输出网格像素宽度
    int imageHeight = 500;           // 输出网格像素高度
    double scanRadiusM = 0.005;      // 扫描半径（米），从 CSV 自动检测
    int cscanUpdateInterval = 500;   // 每 N 条 A-line 刷新一次 C-scan

    // Convenience: is this a spiral scan?
    bool isSpiral() const { return !trajectoryCsvPath.isEmpty(); }
};

// ===== Spectrum card info (simplified) =====
struct CardInfo {
    QString cardType;
    int serialNumber = 0;
    int64_t memSizeBytes = 0;
    int functionType = 0;       // SPCM_TYPE_AI / AO / DI / DO / DIO
    int maxChannels = 0;
    int bytesPerSample = 2;     // int16 = 2
    int64_t maxSamplerate = 0;
    int64_t minSamplerate = 0;
    bool isOpen = false;
};

// ===== FIFO status snapshot =====
struct FifoStatus {
    int32_t m2Status = 0;
    int64_t availUserBytes = 0;
    int64_t availUserPos = 0;
    int32_t fillSizePromille = 0;
};

// ===== Scan state =====
enum class ScanState {
    Idle,
    Armed,
    Scanning,
    Done,
    Error
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
    std::vector<float> sum;     // [gridSize] accumulated peak * weight
    std::vector<float> weight;  // [gridSize] accumulated weight
};
