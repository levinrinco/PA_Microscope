#pragma once

#include <QObject>
#include <QDateTime>
#include <QString>
#include <vector>
#include <atomic>
#include "Types.h"
#include "DepthEncode.h"

class FileWriter : public QObject {
    Q_OBJECT
public:
    explicit FileWriter(QObject *parent = nullptr);
    ~FileWriter();

    void setSavePath(const QString &dir);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }
    int64_t savedItemCount() const { return m_savedItems.load(std::memory_order_relaxed); }

    /// Number of raster B-scan rows (nSlow) that make up one frame. Needed to
    /// split a continuous session's globally-numbered rows into per-frame
    /// folders. Must be set before a scan starts.
    void setRowsPerFrame(int rows) { m_rowsPerFrame = rows; }

    /// Call at scan start — creates the session folder. Its name is prefixed
    /// `Single_` or `Continuous_`; in continuous mode each frame's raw data
    /// goes into its own `frame_XXXXX/` sub-folder, while single-file outputs
    /// (.npy/.pcd/.png) stay directly in the session folder.
    bool beginSession(bool continuous);

    /// Save one raster B-scan as DC-centered data in the requested element type
    /// (little-endian float64 / float32 / float16).
    bool saveRasterBscan(const std::vector<double> &rawBscan,
                         int bscanIndex, int depth, int nFast,
                         DataFormat fmt);

    /// Save the final raster C-scan MIP en-face image as a PNG, colored with
    /// the same foreground mapping/colormap as the on-screen C-scan view.
    bool saveRasterCscanPng(const std::vector<float> &mip,
                            int imageW, int imageH, int frameIdx,
                            int colormap, int colorMapping, bool colorClipping);

    /// Save a depth-encoded en-face image as a PNG. \a depthMap holds one depth
    /// index per pixel (row-major, imageW x imageH) and \a intensity the
    /// matching intensity plane used to modulate brightness; pass an empty
    /// intensity plane for a pure depth rendering. \a windowDepth is the signal
    /// window in samples, i.e. the depth axis the colors are scaled against —
    /// it must be the acquisition window, not imageH.
    bool saveDepthPng(const std::vector<float> &depthMap,
                      int imageW, int imageH, int windowDepth, int frameIdx,
                      const DepthConfig &config,
                      const std::vector<float> *intensity = nullptr);

    /// Grid raw A-lines → 3D volume (.npy) + MIP (.png) + peak point cloud (.pcd).
    /// The volume is computed once and dispatched to the requested outputs.
    /// @param fmt          element type for the .npy volume
    /// @param saveVolume   write volume_*.npy
    /// @param saveMip      write mip_*.png (colored like the on-screen MIP)
    /// @param savePcd      write cloud_*.pcd (segmented peak point cloud)
    /// @param saveRawDat   write wave_*.dat (raw per-frame waveform, alines x depth)
    /// @param segMode      PCD background filtering: Off / Otsu / Manual
    /// @param segThreshold fraction of the global peak for Manual mode
    /// @param scanRadiusM  scan radius (m) used to convert pixels back to meters
    bool saveTrajectoryFrame(const int16_t *rawAlines,
                             int alineCount, int depth,
                             const std::vector<MapInfo> &mapInfo,
                             int imageW, int imageH,
                             int frameIdx,
                             DataFormat fmt,
                             int colormap, int colorMapping, bool colorClipping,
                             bool saveVolume, bool saveMip, bool savePcd,
                             bool saveRawDat,
                             const std::vector<float> *depthMap,
                             const DepthConfig &depthConfig,
                             int windowDepth,
                             PcdSegMode segMode, float segThreshold,
                             double scanRadiusM);

signals:
    void itemSaved(int index);
    void errorOccurred(const QString &message);

private:
    std::atomic<int64_t> m_savedItems{0};
    QString m_savePath;
    QString m_sessionDir;    // current session subfolder (cleared at endSession)
    bool m_enabled = false;
    bool m_continuous = false;   // continuous session -> per-frame raw folders
    int  m_rowsPerFrame = 0;     // raster nSlow, for global-row -> frame mapping

    /// Session-relative folder holding one frame's raw data. Empty when the
    /// outputs live directly in the session folder (single-shot or disabled).
    QString frameOutputDir(int frameIdx) const;

    void gridVolume(const int16_t *rawAlines,
                    int alineCount, int depth,
                    const std::vector<MapInfo> &mapInfo,
                    int imageW, int imageH,
                    std::vector<float> &volumeOut,
                    std::vector<float> &mipOut, bool keepVolume = true);

    bool saveMipPng(const QString &path, const std::vector<float> &mip,
                    int imageW, int imageH, int colormap,
                    int colorMapping, bool colorClipping);

    /// Colorize a depth map and store it as a PNG. Shared by the raster C-scan
    /// and trajectory MIP depth outputs so both produce an identical palette.
    bool saveDepthImagePng(const QString &path,
                           const std::vector<float> &depthMap,
                           const std::vector<float> *intensity,
                           int imageW, int imageH, int windowDepth,
                           const DepthConfig &config);

    bool saveVolumeNpy(const QString &path, const std::vector<float> &volume,
                       int imageW, int imageH, int depth, DataFormat fmt);

    bool saveTrajectoryPcd(const QString &path, const std::vector<float> &volume,
                           int imageW, int imageH, int depth,
                           double scanRadiusM,
                           PcdSegMode segMode, float segThreshold);
};
