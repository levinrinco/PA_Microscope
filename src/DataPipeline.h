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

    void setMapInfo(const std::vector<MapInfo> *mapInfo) { m_mapInfo = mapInfo; }
    void setGridSize(int w, int h) { m_imageW = w; m_imageH = h; }
    void setUpdateInterval(int n) { m_updateInterval = n; }
    void setSegmentSize(int seg) { m_segmentSize = seg; }
    void setContinuous(bool on)  { m_continuous.store(on, std::memory_order_relaxed); }
    void setLiveWindowOffset(int off) { m_liveWindowOffset.store(off, std::memory_order_relaxed); m_depthWarningEmitted.store(false, std::memory_order_relaxed); }
    void setLiveDepth(int d)          { m_liveDepth.store(d, std::memory_order_relaxed); m_depthWarningEmitted.store(false, std::memory_order_relaxed); }

public slots:
    void run();
    void requestStop();
    std::vector<double> getLatestAline();
    void setColorParams(int mapping, bool clipping) {
        m_colorMapping = mapping; m_colorClipping = clipping;
    }

    const MipAccum& mipSlot(int idx) const { return m_mipPool[idx]; }
    void releaseMipSlot(int slotIdx);

    // Save context (set before scan, used by dedicated save thread)
    void setSaveContext(class FileWriter *writer, const std::vector<MapInfo> *mi, int w, int h) {
        m_fileWriter = writer; m_saveMapInfo = mi; m_saveW = w; m_saveH = h;
    }

signals:
    void scanProgress(int currentAlines, int totalAlines);
    void fifoStatusChanged(const FifoStatus &status);
    void scanFinished();
    void errorOccurred(const QString &message);
    void mipFrameCompleted(int slotIdx, int frameIdx);
    void frameDataReady(std::shared_ptr<FrameBuf> frameData,
                        int alineCount, int depth, int frameIdx);

private:
    SpectrumFifo *m_fifo;
    std::atomic<int> m_stopRequested{0};
    const std::vector<MapInfo> *m_mapInfo = nullptr;
    int m_imageW = 500, m_imageH = 500;
    int m_updateInterval = 500;
    int m_segmentSize = 4096;
    std::atomic<int> m_liveWindowOffset{200};
    std::atomic<int> m_liveDepth{600};
    std::atomic<bool> m_continuous{false};
    std::atomic<bool> m_depthWarningEmitted{false};

    // Lock-free queue: acquisition → processing (allocated in run())
    std::unique_ptr<moodycamel::ConcurrentQueue<int16_t>> m_rawQueue;
    std::unique_ptr<moodycamel::ProducerToken> m_enqToken;
    std::unique_ptr<moodycamel::ConsumerToken> m_deqToken;

    // Triple-buffer FrameBuf pool (processing thread swaps, replenish thread fills)
    FrameBuf m_bufPool[3];
    int m_activeBuf = 0;
    std::vector<int> m_freeBufs;
    std::mutex m_bufPoolMutex;

    // Replenish thread
    struct ReplenishTask { int slot; int fs; int total; };
    std::queue<ReplenishTask> m_replenishQueue;
    std::mutex m_replenishMutex;
    std::condition_variable m_replenishCv;
    std::atomic<bool> m_replenishStop{false};
    std::thread m_replenishThread;
    void replenishWorker();

    // Dedicated save thread (never blocks main or processing threads)
    FileWriter *m_fileWriter = nullptr;
    const std::vector<MapInfo> *m_saveMapInfo = nullptr;
    int m_saveW = 500, m_saveH = 500;
    struct SaveTask { std::shared_ptr<FrameBuf> data; int total; int ed; int frameIdx; };
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
    int m_colorMapping = 0;
    bool m_colorClipping = true;

    // Processing state
    int m_pos = 0;
    int64_t m_total = 0;
    int m_frameIdx = 0;
    int m_effDepth = 0;
    int m_frameSize = 0;
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

    std::atomic<int64_t> m_diagEnqueued{0};
};
