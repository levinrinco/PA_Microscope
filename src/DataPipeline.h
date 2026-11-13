#pragma once

#include <QObject>
#include <QImage>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include "Types.h"
#include "DepthEncode.h"
#include "DisplayMailbox.h"
#include "concurrentqueue.h"

class SpectrumFifo;
class FileWriter;

struct FrameBuf {
    std::unique_ptr<int16_t[]> rawAlines;
    size_t                    rawSize = 0;
    std::vector<float>        alinePeaks;
};

class DataPipeline : public QObject {
    Q_OBJECT
public:
    explicit DataPipeline(SpectrumFifo *fifo, QObject *parent = nullptr);
    ~DataPipeline();

    void setScanMode(ScanMode mode) { m_scanMode = mode; }
    void setRasterSize(int nFast, int nSlow) { m_nFast = nFast; m_nSlow = nSlow; }
    void setMapInfo(const std::vector<MapInfo> *mapInfo) { m_mapInfo = mapInfo; }
    void setGridSize(int w, int h) { m_imageW = w; m_imageH = h; }
    void setUpdateInterval(int n) { m_updateInterval = n; }
    void setSegmentSize(int seg) { m_segmentSize = seg; }
    // 1-based inclusive acquisition ordinals; live edits apply at a frame boundary.
    void setTrajectoryBscanRange(int first, int last);
    void setContinuous(bool on)  { m_continuous.store(on, std::memory_order_relaxed); }
    // Configure before run(). Production UI polls; legacy signals are for
    // synchronous/offline consumers, never queued large-array GUI updates.
    void setDisplayPollingEnabled(bool on) { m_displayPolling = on; }
    DisplayUpdate takeDisplayUpdate() { return m_displayMailbox.take(); }
    void setLiveWindowOffset(int off) { m_liveWindowOffset.store(off, std::memory_order_relaxed); m_depthWarningEmitted.store(false, std::memory_order_relaxed); }
    void setLiveDepth(int d)          { m_liveDepth.store(d, std::memory_order_relaxed); m_depthWarningEmitted.store(false, std::memory_order_relaxed); }

public slots:
    void run();
    void requestStop();
    std::vector<double> getLatestAline();
    void setColorParams(int mapping, bool clipping) {
        m_colorMapping.store(mapping, std::memory_order_relaxed);
        m_colorClipping.store(clipping, std::memory_order_relaxed);
    }
    void setColorMap(int colormap) { m_colorMap.store(colormap, std::memory_order_relaxed); }

    const MipAccum& mipSlot(int idx) const { return m_mipPool[idx]; }
    void releaseMipSlot(int slotIdx);

    // Save context (set before scan, used by dedicated save thread)
    void setSaveContext(class FileWriter *writer, const std::vector<MapInfo> *mi,
                        int w, int h) {
        m_fileWriter = writer; m_saveMapInfo = mi; m_saveW = w; m_saveH = h;
    }

    // Per-format save toggles + stored data format (mode-scoped by MainWindow).
    void setSaveFormats(bool rasterDat, bool rasterPng, bool rasterPngDepth,
                        bool trajNpy, bool trajPcd, bool trajMip,
                        bool trajDat, bool trajMipDepth);
    void setSaveDataFormat(DataFormat fmt);
    void setSaveSegmentation(PcdSegMode mode, float threshold);
    void setScanRadiusM(double radiusM);

    // Depth encoding. Enabling it alone adds no file output — it makes the
    // processing thread track each A-line's envelope-peak depth so the live
    // display can be painted by depth. The two PNG flags decide what is stored.
    void setDepthEncodeEnabled(bool on) { m_depthEncode.store(on, std::memory_order_relaxed); }
    void setSaveDepthMapsEnabled(bool on) { m_saveDepthMaps.store(on, std::memory_order_relaxed); }
    /// Colormap / range / gate used when a depth map or depth PNG is produced.
    /// Set before the scan; the processing thread reads it in run().
    void setDepthConfig(const DepthConfig &config);

    /// True when the host queue has no samples left. Offline tests use this to
    /// decide when the simulated block has been fully taken, so they can stop
    /// the card the way the GUI thread would.
    bool queueEmpty() const { return m_rawQueue && m_rawQueue->size_approx() == 0; }

    /// A-lines the processing loop has finished. A bulk dequeue removes samples
    /// from the queue before they are processed, so "queue is empty" is NOT the
    /// same as "everything has been handled"; a test that stops the card on an
    /// empty queue can cut a frame in half.
    int64_t processedAlines() const { return m_processedAlines.load(std::memory_order_acquire); }

signals:
    void firstDataReceived();
    void acquisitionComplete();
    void bscanDataReady(int rowIdx, const std::vector<double> &data,
                        int depth, int nFast);
    // Signed, DC-removed acquisition-window samples [point][depth], before gridding.
    void trajectoryBscanReady(int frameIdx, int first, int last,
                              const std::vector<double> &raw, int depth);
    void cscanRowReady(int slowIdx, const std::vector<float> &mipRow);
    /// Depth twin of cscanRowReady: envelope-peak depth index per fast-axis
    /// position. Emitted only while depth encoding is enabled.
    void cscanDepthRowReady(int slowIdx, const std::vector<float> &depthRow);
    void rasterRowCompleted(int currentRow, int totalRows);
    void frameStarted(int frameIdx);
    void scanProgress(int currentAlines, int totalAlines);
    void fifoStatusChanged(const FifoStatus &status);
    void scanFinished();
    void errorOccurred(const QString &message);
    void mipFrameCompleted(int slotIdx, int frameIdx);
    /// Depth map of a completed trajectory frame (row-major, imageW x imageH,
    /// values are absolute depth indices). Sent only when depth encoding is on.
    void mipDepthReady(int frameIdx, const std::vector<float> &depthMap,
                       int imageW, int imageH);

private:
    bool m_displayPolling = false;
    DisplayMailbox m_displayMailbox;
    SpectrumFifo *m_fifo;
    std::atomic<int> m_stopRequested{0};
    std::atomic<bool> m_acquisitionDone{false};
    ScanMode m_scanMode = ScanMode::Trajectory;
    int m_nFast = 1000;
    int m_nSlow = 1000;
    const std::vector<MapInfo> *m_mapInfo = nullptr;
    int m_imageW = 500, m_imageH = 500;
    int m_updateInterval = 500;
    int m_segmentSize = 4096;
    // Store both endpoints atomically so a frame never observes a mixed range.
    std::atomic<uint64_t> m_trajectoryBscanRange{(uint64_t{1} << 32) | 1000u};
    std::atomic<int> m_liveWindowOffset{200};
    std::atomic<int> m_liveDepth{600};
    std::atomic<bool> m_continuous{false};
    std::atomic<bool> m_depthWarningEmitted{false};

    // Lock-free queue: acquisition → processing (allocated in run())
    std::unique_ptr<moodycamel::ConcurrentQueue<int16_t>> m_rawQueue;
    std::unique_ptr<moodycamel::ProducerToken> m_enqToken;
    std::unique_ptr<moodycamel::ConsumerToken> m_deqToken;

    // Triple-buffer FrameBuf pool. A completed slot is returned by saveWorker,
    // bounding only saved-frame buffers, NOT the upstream raw sample queue.
    FrameBuf m_bufPool[3];
    int m_activeBuf = 0;
    std::vector<int> m_freeBufs;
    std::mutex m_bufPoolMutex;
    std::condition_variable m_bufPoolCv;

    // Dedicated save thread. Processing can wait for a free trajectory buffer.
    FileWriter *m_fileWriter = nullptr;
    const std::vector<MapInfo> *m_saveMapInfo = nullptr;
    int m_saveW = 500, m_saveH = 500;
    struct SaveTask {
        ScanMode mode = ScanMode::Trajectory;
        std::shared_ptr<FrameBuf> trajectoryData;
        std::shared_ptr<std::vector<double>> rasterData;   // raw B-scan (.dat)
        std::shared_ptr<std::vector<float>> cscanMip;      // final C-scan MIP (.png)
        // Final C-scan / MIP depth map (.png), row-major imageW x imageH.
        std::shared_ptr<std::vector<float>> depthMap;
        int cscanW = 0, cscanH = 0;
        // Grid size the depth map was built for (trajectory). Carried per task
        // so a task queued by a finished scan cannot be written with the grid
        // dimensions of the next one.
        int frameW = 0, frameH = 0;
        int total = 0;
        int depth = 0;
        int frameIdx = 0;
        int bufferSlot = -1;
        int colorMap = 0;
        int colorMapping = 0;
        bool colorClipping = true;
        DataFormat dataFormat = DataFormat::Float32;
        bool saveDat = true;    // raster raw .dat
        bool savePng = false;   // raster C-scan .png
        bool saveDepthPng = false; // raster/trajectory depth-encoded .png
        bool saveNpy = true;    // trajectory volume .npy
        bool savePcd = false;   // trajectory point cloud .pcd
        bool saveMip = true;    // trajectory MIP .png
        bool saveDatTraj = false; // trajectory/spiral raw per-frame waveform .dat
        PcdSegMode segMode = PcdSegMode::Otsu;
        float segThreshold = 0.25f;
        double scanRadiusM = 0.0;
        // Copied per task instead of pointing at pipeline state: the save
        // thread may still be rendering this task when a new scan starts and
        // overwrites the pipeline's configuration.
        DepthConfig depthConfig;
    };
    std::queue<SaveTask> m_saveQueue;
    std::mutex m_saveMutex;
    std::condition_variable m_saveCv;
    std::atomic<bool> m_saveStop{false};
    std::thread m_saveThread;
    void saveWorker();

    // MIP triple-buffer
    MipAccum m_mipPool[3];
    std::vector<int> m_freeMipSlots;
    std::mutex m_mipPoolMutex;
    int m_activeMipSlot = 0;
    int m_mipExhaustCount = 0;
    std::atomic<int> m_colorMapping{0};
    std::atomic<bool> m_colorClipping{true};
    std::atomic<int> m_colorMap{0};

    // Save flags / stored format (mode-scoped, set by MainWindow before scan)
    bool m_saveDat = true;
    bool m_savePng = false;
    bool m_saveDepthPng = false;
    bool m_saveNpy = true;
    bool m_savePcd = false;
    bool m_saveMip = true;
    bool m_saveTrajDat = false;   // trajectory/spiral raw per-frame waveform .dat
    DataFormat m_saveDataFormat = DataFormat::Float32;
    PcdSegMode m_saveSegMode = PcdSegMode::Otsu;
    float m_saveSegThreshold = 0.25f;
    double m_saveRadiusM = 0.0;

    // Depth encoding (live display + optional depth PNGs).
    std::atomic<bool> m_depthEncode{false};
    std::atomic<bool> m_saveDepthMaps{false};
    DepthConfig m_depthConfig;
    mutable std::mutex m_depthConfigMutex;
    // Per-A-line envelope-peak depth of the current raster row (nFast entries,
    // stored fast-major so it lines up with m_cscanAccum).
    std::vector<float> m_depthRow;
    // Raster depth plane (nFast x nSlow). Deliberately separate from
    // m_cscanAccum: the intensity plane is normalized by its own weight array,
    // so folding depth into it would corrupt the intensity values.
    std::vector<float> m_cscanDepth;

    // Trajectory depth map of the last completed frame, reused across frames so
    // a continuous scan does not allocate a grid-sized buffer per frame.
    std::vector<float> m_mipDepthPlane;
    int m_mipDepthW = 0, m_mipDepthH = 0;
    /// True when m_mipDepthPlane holds a plane built for the frame that is about
    /// to be emitted. Cleared when the MIP pool was exhausted instead.
    bool m_depthPlaneValid = false;

    // Raster C-scan MIP accumulation (nFast x nSlow), saved per frame PNG
    std::vector<float> m_cscanAccum;

    // Raster row accumulators (one completed fast-axis row at a time)
    std::vector<double> m_currentBscan;
    std::vector<double> m_currentRawBscan;
    std::vector<float> m_currentMipRow;

    // Processing state
    int m_pos = 0;
    int64_t m_total = 0;
    int m_frameIdx = 0;
    int m_effDepth = 0;
    size_t m_frameSize = 0;
    int m_totalAlinesPerFrame = 0;

    std::vector<double> m_workEnvelope;
    std::vector<double> m_workRaw;
    std::mutex m_latestMutex;
    std::vector<double> m_latestAline;

    // Deferred emits
    int m_pendingMipSlot = -1;
    int m_pendingMipFrameIdx = 0;

    void acquisitionLoop();
    void processingLoop();
    void handoffMipFrame();
    DepthConfig depthConfigSnapshot() const;

    std::atomic<int64_t> m_diagEnqueued{0};
    std::atomic<int64_t> m_processedAlines{0};
};
