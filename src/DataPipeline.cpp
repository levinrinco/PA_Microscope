#include "DataPipeline.h"
#include "FileWriter.h"
#include "SpectrumFifo.h"
#include <QDebug>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

DataPipeline::DataPipeline(SpectrumFifo *fifo, QObject *parent)
    : QObject(parent), m_fifo(fifo)
{
    // Reset here rather than in run() so a helper thread that starts before
    // run() cannot read a previous scan's count. One pipeline == one scan.
    m_processedAlines.store(0, std::memory_order_release);
    m_saveThread = std::thread(&DataPipeline::saveWorker, this);
}

DataPipeline::~DataPipeline()
{
    requestStop();
    m_saveStop.store(true, std::memory_order_release);
    m_saveCv.notify_all();
    m_bufPoolCv.notify_all();
    if (m_saveThread.joinable()) m_saveThread.join();
}

void DataPipeline::requestStop()
{
    m_stopRequested.store(1, std::memory_order_release);
    m_bufPoolCv.notify_all();
}

void DataPipeline::setSaveFormats(bool rasterDat, bool rasterPng,
                                  bool rasterPngDepth,
                                  bool trajNpy, bool trajPcd, bool trajMip,
                                  bool trajDat, bool trajMipDepth)
{
    m_saveDat = rasterDat;
    m_savePng = rasterPng;
    m_saveNpy = trajNpy;
    m_savePcd = trajPcd;
    m_saveMip = trajMip;
    m_saveTrajDat = trajDat;
    // One depth-PNG request serves both modes: each mode only ever consults it
    // for its own outputs, and disallowing both is the same as not saving.
    m_saveDepthPng = rasterPngDepth || trajMipDepth;
}

void DataPipeline::setSaveDataFormat(DataFormat fmt)
{
    m_saveDataFormat = fmt;
}

void DataPipeline::setSaveSegmentation(PcdSegMode mode, float threshold)
{
    m_saveSegMode = mode;
    m_saveSegThreshold = threshold;
}

void DataPipeline::setScanRadiusM(double radiusM)
{
    m_saveRadiusM = radiusM;
}

void DataPipeline::setDepthConfig(const DepthConfig &config)
{
    std::lock_guard<std::mutex> lock(m_depthConfigMutex);
    m_depthConfig = config;
}

void DataPipeline::setTrajectoryBscanRange(int first, int last)
{
    first = std::max(1, first);
    last = std::max(first, last);
    m_trajectoryBscanRange.store((static_cast<uint64_t>(first) << 32) |
                                 static_cast<uint32_t>(last), std::memory_order_relaxed);
}

DepthConfig DataPipeline::depthConfigSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_depthConfigMutex);
    return m_depthConfig;
}

std::vector<double> DataPipeline::getLatestAline()
{
    std::lock_guard<std::mutex> lock(m_latestMutex);
    return m_latestAline;
}

void DataPipeline::saveWorker()
{
    for (;;) {
        SaveTask task;
        {
            std::unique_lock<std::mutex> lock(m_saveMutex);
            m_saveCv.wait(lock, [this] {
                return m_saveStop.load(std::memory_order_acquire) || !m_saveQueue.empty();
            });
            if (m_saveStop.load(std::memory_order_acquire) && m_saveQueue.empty())
                break;
            task = std::move(m_saveQueue.front());
            m_saveQueue.pop();
        }

        bool saved = true;
        if (m_fileWriter && m_fileWriter->isEnabled()) {
            if (task.mode == ScanMode::Raster) {
                if (task.saveDat && task.rasterData) {
                    // Combine with && so a failed .dat write cannot be masked by
                    // a later successful .png write in the same task.
                    saved = m_fileWriter->saveRasterBscan(*task.rasterData, task.frameIdx,
                                                  task.depth, task.total,
                                                  task.dataFormat) && saved;
                }
                if (task.savePng && task.cscanMip &&
                    task.cscanW > 0 && task.cscanH > 0) {
                    saved = m_fileWriter->saveRasterCscanPng(
                        *task.cscanMip, task.cscanW, task.cscanH, task.frameIdx,
                        task.colorMap, task.colorMapping, task.colorClipping) && saved;
                }
                if (task.saveDepthPng && task.depthMap &&
                    task.cscanW > 0 && task.cscanH > 0) {
                    saved = m_fileWriter->saveDepthPng(
                        *task.depthMap, task.cscanW, task.cscanH, task.depth,
                        task.frameIdx, task.depthConfig,
                        task.cscanMip ? task.cscanMip.get() : nullptr) && saved;
                }
            } else if (task.mode == ScanMode::Trajectory && task.trajectoryData &&
                       m_saveMapInfo &&
                       (task.saveNpy || task.saveMip || task.savePcd ||
                        task.saveDatTraj || (task.saveDepthPng && task.depthMap))) {
                // The depth map was gridded for the frame that produced it;
                // refuse to hand it to a writer configured for another grid.
                const bool depthMatchesGrid = task.depthMap &&
                    task.frameW == m_saveW && task.frameH == m_saveH;
                saved = m_fileWriter->saveTrajectoryFrame(
                    task.trajectoryData->rawAlines.get(), task.total, task.depth,
                    *m_saveMapInfo, m_saveW, m_saveH, task.frameIdx,
                    task.dataFormat, task.colorMap, task.colorMapping,
                    task.colorClipping,
                    task.saveNpy, task.saveMip, task.savePcd,
                    task.saveDatTraj,
                    depthMatchesGrid ? task.depthMap.get() : nullptr,
                    task.depthConfig, task.depth,
                    task.segMode, task.segThreshold, task.scanRadiusM);
            }
        }
        if (!saved) {
            m_stopRequested.store(1, std::memory_order_release);
            emit errorOccurred(QString("Failed to save frame %1; complete saving is not guaranteed")
                                   .arg(task.frameIdx));
        }

        // Return exactly the saved allocation to the pool. This bounds memory
        // even when disk writing is slower than a continuous acquisition.
        if (task.bufferSlot >= 0 && task.trajectoryData) {
            std::lock_guard<std::mutex> lock(m_bufPoolMutex);
            m_bufPool[task.bufferSlot].rawAlines =
                std::move(task.trajectoryData->rawAlines);
            m_bufPool[task.bufferSlot].rawSize = task.trajectoryData->rawSize;
            m_bufPool[task.bufferSlot].alinePeaks =
                std::move(task.trajectoryData->alinePeaks);
            m_freeBufs.push_back(task.bufferSlot);
            m_bufPoolCv.notify_one();
        }
    }
}

void DataPipeline::handoffMipFrame()
{
    const int completed = m_activeMipSlot;
    int next = -1;
    {
        std::lock_guard<std::mutex> lock(m_mipPoolMutex);
        if (!m_freeMipSlots.empty()) {
            next = m_freeMipSlots.back();
            m_freeMipSlots.pop_back();
        }
    }

    if (next < 0) {
        // No spare slot, so this frame has no depth plane: mark it invalid so a
        // stale plane from an earlier frame is not emitted in its place.
        ++m_mipExhaustCount;
        m_depthPlaneValid = false;
        std::fill(m_mipPool[completed].sum.begin(),
                  m_mipPool[completed].sum.end(), 0.0f);
        std::fill(m_mipPool[completed].weight.begin(),
                  m_mipPool[completed].weight.end(), 0.0f);
        std::fill(m_mipPool[completed].depthSum.begin(),
                  m_mipPool[completed].depthSum.end(), 0.0f);
        return;
    }

    m_activeMipSlot = next;
    m_pendingMipSlot = completed;
    m_pendingMipFrameIdx = m_frameIdx;
}

void DataPipeline::releaseMipSlot(int slotIdx)
{
    if (slotIdx < 0 || slotIdx >= 3) return;
    std::fill(m_mipPool[slotIdx].sum.begin(), m_mipPool[slotIdx].sum.end(), 0.0f);
    std::fill(m_mipPool[slotIdx].weight.begin(), m_mipPool[slotIdx].weight.end(), 0.0f);
    std::fill(m_mipPool[slotIdx].depthSum.begin(), m_mipPool[slotIdx].depthSum.end(), 0.0f);
    std::lock_guard<std::mutex> lock(m_mipPoolMutex);
    m_freeMipSlots.push_back(slotIdx);
}

void DataPipeline::run()
{
    const bool raster = m_scanMode == ScanMode::Raster;
    if (raster) {
        if (m_nFast <= 0 || m_nSlow <= 0) {
            emit errorOccurred("Invalid raster dimensions");
            emit scanFinished();
            return;
        }
    } else if (!m_mapInfo || m_mapInfo->empty()) {
        emit errorOccurred("No trajectory mapping data");
        emit scanFinished();
        return;
    }

    const int segmentSize = m_segmentSize;
    const int configuredDepth = m_liveDepth.load(std::memory_order_relaxed);
    int initialOffset = std::max(0, m_liveWindowOffset.load(std::memory_order_relaxed));
    if (initialOffset >= segmentSize) initialOffset = 0;
    const int effectiveDepth = std::min(configuredDepth, segmentSize - initialOffset);
    const int64_t total64 = raster
        ? static_cast<int64_t>(m_nFast) * m_nSlow
        : static_cast<int64_t>(m_mapInfo->size());
    if (total64 <= 0 || total64 > std::numeric_limits<int>::max()) {
        emit errorOccurred("Scan contains too many A-lines");
        emit scanFinished();
        return;
    }
    const int totalPerFrame = static_cast<int>(total64);
    const bool continuous = m_continuous.load(std::memory_order_relaxed);

    if (segmentSize <= 0 || effectiveDepth <= 0 || totalPerFrame <= 0) {
        emit errorOccurred("Invalid scan size, segment size, or signal window");
        emit scanFinished();
        return;
    }

    qInfo() << "Pipeline START mode=" << (raster ? "raster" : "trajectory")
            << "seg=" << segmentSize << "depth=" << effectiveDepth
            << "alines/frame=" << totalPerFrame << "continuous=" << continuous;

    // Each scan owns a fresh pipeline. Preserve a stop requested before run().
    m_effDepth = effectiveDepth;
    const size_t totalSize = static_cast<size_t>(totalPerFrame);
    if (totalSize > std::numeric_limits<size_t>::max() /
                        static_cast<size_t>(effectiveDepth)) {
        emit errorOccurred("Frame dimensions overflow host address space");
        emit scanFinished();
        return;
    }
    m_frameSize = totalSize * static_cast<size_t>(effectiveDepth);
    m_totalAlinesPerFrame = totalPerFrame;
    m_diagEnqueued.store(0, std::memory_order_relaxed);
    m_pos = 0;
    m_total = 0;
    m_frameIdx = 0;
    m_pendingMipSlot = -1;
    m_depthWarningEmitted.store(false, std::memory_order_relaxed);

    // Keep host buffering independent of an entire image frame. The former
    // total*depth*2 capacity could reserve gigabytes for a 1000x1000 raster.
    try {
        const size_t notifySamples = m_fifo->notifySize() > 0
            ? static_cast<size_t>(m_fifo->notifySize()) / sizeof(int16_t) : 0;
        // This is an initial allocation hint, not a hard limit. Cap it so a
        // large legal segment/notify setting cannot reserve several GB before
        // the first sample arrives; ConcurrentQueue can still grow on demand.
        constexpr size_t kMaxInitialQueueSamples =
            (256u * 1024u * 1024u) / sizeof(int16_t);
        const size_t desiredCapacity = std::max(
            static_cast<size_t>(segmentSize) * 32u,
            notifySamples * 4u);
        const size_t queueCapacity = std::max(
            static_cast<size_t>(segmentSize),
            std::min(desiredCapacity, kMaxInitialQueueSamples));
        m_rawQueue = std::make_unique<moodycamel::ConcurrentQueue<int16_t>>(queueCapacity);
        m_enqToken = std::make_unique<moodycamel::ProducerToken>(*m_rawQueue);
        m_deqToken = std::make_unique<moodycamel::ConsumerToken>(*m_rawQueue);

        // A depth PNG is a trajectory output too. Leaving it out of this test
        // meant "only the depth PNG ticked" silently produced nothing.
        const bool saveTrajectory = !raster && m_fileWriter && m_saveMapInfo &&
                                    m_fileWriter->isEnabled() &&
                                    (m_saveNpy || m_saveMip || m_savePcd ||
                                     m_saveTrajDat || m_saveDepthPng);

        if (!raster) {
            const int gridSize = m_imageW * m_imageH;
            for (auto &slot : m_mipPool) {
                slot.sum.assign(gridSize, 0.0f);
                slot.weight.assign(gridSize, 0.0f);
                slot.depthSum.assign(gridSize, 0.0f);
            }
            {
                std::lock_guard<std::mutex> lock(m_mipPoolMutex);
                m_freeMipSlots = {1, 2};
            }
            m_activeMipSlot = 0;
            m_mipExhaustCount = 0;

            // Reused across frames; save tasks receive a copy so a slow write
            // cannot alias the plane for the next frame.
            m_mipDepthPlane.assign(gridSize, 0.0f);
            m_mipDepthW = m_imageW;
            m_mipDepthH = m_imageH;
            m_depthRow.assign(totalPerFrame, 0.0f);

            if (saveTrajectory) {
                for (auto &buffer : m_bufPool) {
                    buffer.rawAlines.reset(new int16_t[m_frameSize]);
                    buffer.rawSize = m_frameSize;
                    buffer.alinePeaks.assign(totalPerFrame, 0.0f);
                }
                std::lock_guard<std::mutex> lock(m_bufPoolMutex);
                m_freeBufs = {1, 2};
                m_activeBuf = 0;
            }
        } else {
            const size_t rowSamples = static_cast<size_t>(m_nFast) * effectiveDepth;
            m_currentBscan.assign(rowSamples, 0.0);
            m_currentRawBscan.assign(rowSamples, 0.0);
            m_currentMipRow.assign(m_nFast, 0.0f);
            m_depthRow.assign(m_nFast, 0.0f);
            m_cscanAccum.assign(static_cast<size_t>(m_nFast) * m_nSlow, 0.0f);
            m_cscanDepth.assign(static_cast<size_t>(m_nFast) * m_nSlow, 0.0f);
        }

        m_workEnvelope.assign(effectiveDepth, 0.0);
        m_workRaw.assign(effectiveDepth, 0.0);
    } catch (const std::bad_alloc &) {
        emit errorOccurred("Insufficient host memory for the selected scan dimensions");
        emit scanFinished();
        return;
    }

    std::thread acquisition(&DataPipeline::acquisitionLoop, this);
    std::thread processing(&DataPipeline::processingLoop, this);
    acquisition.join();
    processing.join();

    // A scan is complete only after all queued files have been written.
    m_saveStop.store(true, std::memory_order_release);
    m_saveCv.notify_all();
    if (m_saveThread.joinable()) m_saveThread.join();

    const int64_t expected = static_cast<int64_t>(m_frameIdx) * totalPerFrame + m_pos;
    const int64_t difference = m_total - expected;
    qInfo() << "Pipeline END frames=" << m_frameIdx << "alines=" << m_total
            << "difference=" << difference << "mip-exhausts=" << m_mipExhaustCount;
    if (difference != 0) qWarning() << "A-line accounting mismatch:" << difference;
    const int64_t queuedAlines = m_diagEnqueued.load(std::memory_order_relaxed) / m_segmentSize;
    qInfo() << "Drain accounting: queued alines=" << queuedAlines
            << "processed alines=" << m_total << "partial-frame alines=" << m_pos;
    if (m_continuous.load(std::memory_order_relaxed) && queuedAlines != m_total)
        emit errorOccurred("Not all queued A-lines were processed");
    emit scanFinished();
}

namespace {

// The acquisition thread must not be preempted by ordinary desktop work while
// it drains the card's DMA buffer. SCHED_FIFO is the Linux counterpart of the
// Windows THREAD_PRIORITY_TIME_CRITICAL boost, but unlike Windows it needs
// CAP_SYS_NICE, and a FIFO thread that ever stops blocking can starve the rest
// of the machine. It is therefore opt-in through the environment:
//
//     PA_ACQ_RT_PRIORITY=80 ./PA_Microscope      # 1..99, needs privileges
//
// Without it the thread keeps the default CFS scheduling class, which is what
// a normal desktop install can actually obtain.
void applyAcquisitionThreadPriority()
{
#ifdef _WIN32
    // Opt-in, without changing the priority class of the whole process.
    const QString requested = qEnvironmentVariable("PA_ACQ_THREAD_PRIORITY");
    if (requested.isEmpty() || requested == "normal") return;
    int priority = THREAD_PRIORITY_NORMAL;
    if (requested == "highest") priority = THREAD_PRIORITY_HIGHEST;
    else if (requested == "time-critical") priority = THREAD_PRIORITY_TIME_CRITICAL;
    else {
        qWarning() << "Unknown PA_ACQ_THREAD_PRIORITY:" << requested;
        return;
    }
    if (!SetThreadPriority(GetCurrentThread(), priority))
        qWarning() << "Cannot set acquisition thread priority, Windows error:" << GetLastError();
    else
        qInfo() << "Acquisition thread priority:" << requested;
#else
    bool hasValue = false;
    const int requested = qEnvironmentVariableIntValue("PA_ACQ_RT_PRIORITY", &hasValue);
    if (!hasValue || requested <= 0)
        return;

    const int minPriority = sched_get_priority_min(SCHED_FIFO);
    const int maxPriority = sched_get_priority_max(SCHED_FIFO);
    if (minPriority < 0 || maxPriority < minPriority)
        return;   // SCHED_FIFO not available on this kernel

    sched_param param{};
    param.sched_priority = std::clamp(requested, minPriority, maxPriority);

    const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        qWarning() << "PA_ACQ_RT_PRIORITY=" << requested
                   << "ignored: cannot switch the acquisition thread to SCHED_FIFO:"
                   << strerror(rc) << "— run as root or grant CAP_SYS_NICE";
    } else {
        qInfo() << "Acquisition thread priority: SCHED_FIFO" << param.sched_priority;
    }
#endif
}

}  // namespace

void DataPipeline::acquisitionLoop()
{
    applyAcquisitionThreadPriority();
    const int segmentSize = m_segmentSize;
    constexpr size_t kTargetBatchBytes = 8u * 1024u * 1024u;
    const size_t samplesPerTargetBatch = kTargetBatchBytes / sizeof(int16_t);
    const size_t batchSegments = std::max<size_t>(
        1u, std::min<size_t>(256u, samplesPerTargetBatch /
                                      static_cast<size_t>(segmentSize)));
    const size_t batchSamples = static_cast<size_t>(segmentSize) * batchSegments;

    bool draining = false;
    for (;;) {
        if (m_stopRequested.load(std::memory_order_acquire) && !draining) {
            if (!m_fifo->stopAcquisition()) {
                emit errorOccurred("Failed to stop DMA before draining: " + m_fifo->lastErrorText());
                break;
            }
            draining = true;
        }
        const uint32_t error = draining ? ERR_OK : m_fifo->waitForData();
        if (error == ERR_TIMEOUT) continue;
        if (error == ERR_ABORT) draining = true;
        if (error != ERR_OK && error != ERR_ABORT) {
            emit errorOccurred(SpectrumFifo::translateError(error));
            m_stopRequested.store(1, std::memory_order_release);
            break;
        }

        FifoStatus status;
        if (!m_fifo->readFifoStatus(status)) {
            emit errorOccurred("Failed to inspect remaining DMA data: " + m_fifo->lastErrorText());
            m_stopRequested.store(1, std::memory_order_release);
            break;
        }
        if (m_displayPolling) m_displayMailbox.publishFifo(status);
        else emit fifoStatusChanged(status);
        if (!draining && status.availUserBytes < static_cast<int64_t>(m_fifo->notifySize()))
            continue;
        if (status.availUserBytes < 0 || status.availUserBytes > m_fifo->bufferSize() ||
            status.availUserPos < 0 || status.availUserPos >= m_fifo->bufferSize() ||
            status.availUserPos % sizeof(int16_t) != 0) {
            emit errorOccurred("Invalid DMA buffer range");
            m_stopRequested.store(1, std::memory_order_release);
            break;
        }

        const int64_t reportedSamples = status.availUserBytes / sizeof(int16_t);
        int64_t remaining = reportedSamples;
        if (remaining < segmentSize) {
            if (draining) {
                if (remaining > 0)
                    qInfo() << "Discarding incomplete DMA A-line samples:" << remaining;
                break;
            }
            continue;
        }

        int64_t enqueued = 0;
        std::vector<int16_t> wrapped;
        bool failed = false;
        while (remaining >= segmentSize) {
            int64_t count = std::min<int64_t>(remaining, static_cast<int64_t>(batchSamples));
            count = (count / segmentSize) * segmentSize;
            const int64_t bytePos = (status.availUserPos + enqueued * sizeof(int16_t))
                                    % m_fifo->bufferSize();
            const int64_t contiguous = (m_fifo->bufferSize() - bytePos) / sizeof(int16_t);
            const int16_t *source = m_fifo->dataBufferAt(bytePos);
            if (count > contiguous) {
                wrapped.resize(static_cast<size_t>(count));
                std::copy_n(source, contiguous, wrapped.data());
                std::copy_n(m_fifo->dataBufferAt(0), count - contiguous,
                            wrapped.data() + contiguous);
                source = wrapped.data();
            }
            if (!m_rawQueue->enqueue_bulk(*m_enqToken, source,
                                          static_cast<size_t>(count))) {
                emit errorOccurred("Host queue allocation failed");
                m_stopRequested.store(1, std::memory_order_release);
                failed = true;
                break;
            }
            remaining -= count;
            enqueued += count;
        }

        if (enqueued > 0) {
            m_diagEnqueued.fetch_add(enqueued, std::memory_order_relaxed);
            if (!m_fifo->releaseData(enqueued * sizeof(int16_t))) {
                emit errorOccurred("Failed to release DMA data: " + m_fifo->lastErrorText());
                m_stopRequested.store(1, std::memory_order_release);
                failed = true;
            }
        }
        if (failed) break;
    }
    // Publish only after the final enqueue; stop is a request, not producer EOF.
    m_acquisitionDone.store(true, std::memory_order_release);
}

void DataPipeline::processingLoop()
{
    const bool raster = m_scanMode == ScanMode::Raster;
    const bool continuous = m_continuous.load(std::memory_order_relaxed);
    const bool saveTrajectory = !raster && m_fileWriter && m_saveMapInfo &&
                                m_fileWriter->isEnabled() &&
                                (m_saveNpy || m_saveMip || m_savePcd ||
                                 m_saveTrajDat || m_saveDepthPng);
    const bool saveDepth = m_saveDepthPng && m_fileWriter && m_fileWriter->isEnabled();
    const int segmentSize = m_segmentSize;
    const int totalPerFrame = m_totalAlinesPerFrame;
    const int effectiveDepth = m_effDepth;
    constexpr size_t kTargetBatchBytes = 8u * 1024u * 1024u;
    const size_t samplesPerTargetBatch = kTargetBatchBytes / sizeof(int16_t);
    const size_t batchSegments = std::max<size_t>(
        1u, std::min<size_t>(256u, samplesPerTargetBatch /
                                      static_cast<size_t>(segmentSize)));
    const size_t batchSamples = static_cast<size_t>(segmentSize) * batchSegments;
    std::vector<int16_t> workBuffer(batchSamples);
    int bscanFirst = 0, bscanLast = 0; // zero-based within the current frame
    std::vector<double> trajectoryBscan;

    auto queueSaveTask = [this](SaveTask &&task) {
        {
            std::lock_guard<std::mutex> lock(m_saveMutex);
            m_saveQueue.push(std::move(task));
        }
        m_saveCv.notify_one();
    };

    // Turn a completed MIP accumulator slot into a per-pixel depth plane.
    // Dividing by `sum` rather than by `weight` makes this an intensity
    // weighted mean of the contributing A-lines' peak depths; a pixel no
    // A-line reached has sum == 0 and is reported as depth 0 / no signal.
    auto depthPlaneFromSlot = [this](const MipAccum &slot) {
        const size_t gridSize = m_mipDepthPlane.size();
        for (size_t i = 0; i < gridSize; ++i) {
            const float intensity = slot.sum[i];
            m_mipDepthPlane[i] = intensity > 1e-9f ? slot.depthSum[i] / intensity : 0.0f;
        }
    };

    // Finish a completed MIP slot: the intensity first (so the GUI has refreshed
    // m_lastMip), then the depth data. The depth image is modulated by the
    // intensity of the same frame, which only holds if this order is kept. The
    // depth plane was already built from the finished accumulator, so this must
    // NOT re-read the pool: handoffMipFrame() has advanced the active slot.
    auto finishMipFrame = [&](int slotIdx, int frameIdx) {
        emit mipFrameCompleted(slotIdx, frameIdx);
        if (!m_depthEncode.load(std::memory_order_relaxed) || !m_depthPlaneValid) return;
        emit mipDepthReady(frameIdx, m_mipDepthPlane, m_mipDepthW, m_mipDepthH);
    };

    while (true) {
        const bool acquisitionDone = m_acquisitionDone.load(std::memory_order_acquire);
        const size_t dequeued = m_rawQueue->try_dequeue_bulk(
            *m_deqToken, workBuffer.data(), static_cast<size_t>(batchSamples));
        if (dequeued == 0) {
            if (acquisitionDone) break;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }
        if (dequeued % segmentSize != 0)
            qWarning() << "Non-aligned host queue block:" << dequeued;

        const int alineCount = static_cast<int>(dequeued / segmentSize);
        for (int a = 0; a < alineCount; ++a) {
            if (!raster && m_pos == 0) {
                const uint64_t range = m_trajectoryBscanRange.load(std::memory_order_relaxed);
                bscanFirst = std::clamp(static_cast<int>(range >> 32), 1, totalPerFrame) - 1;
                bscanLast = std::clamp(static_cast<int>(range & 0xffffffffu),
                                      bscanFirst + 1, totalPerFrame) - 1;
                try {
                    trajectoryBscan.assign(
                        static_cast<size_t>(bscanLast - bscanFirst + 1) * effectiveDepth, 0.0);
                } catch (const std::exception &) {
                    emit errorOccurred("Insufficient memory for the selected trajectory B-scan range");
                    requestStop();
                    goto processingExit;
                }
            }
            const int16_t *aline = &workBuffer[a * segmentSize];
            int windowOffset = m_liveWindowOffset.load(std::memory_order_relaxed);
            int windowDepth = m_liveDepth.load(std::memory_order_relaxed);
            windowOffset = std::clamp(windowOffset, 0, segmentSize - 1);
            windowDepth = std::min(windowDepth, effectiveDepth);
            if (windowOffset + windowDepth > segmentSize) {
                windowDepth = segmentSize - windowOffset;
                if (!m_depthWarningEmitted.exchange(true))
                    qWarning() << "Signal window clamped to" << windowDepth;
            }
            if (windowDepth <= 0) continue;

            double dcMean = 0.0;
            for (int i = 0; i < windowDepth; ++i)
                dcMean += aline[windowOffset + i];
            dcMean /= windowDepth;

            float peak = 0.0f;
            int peakDepth = 0;
            for (int i = 0; i < windowDepth; ++i) {
                const double raw = static_cast<double>(aline[windowOffset + i]) - dcMean;
                const double envelope = std::abs(raw);
                m_workRaw[i] = raw;
                m_workEnvelope[i] = envelope;
                if (envelope > peak) {
                    peak = static_cast<float>(envelope);
                    // Free: the comparison already exists, so recording WHERE
                    // the maximum sits costs nothing on top of finding it.
                    peakDepth = i;
                }
            }
            // Where the peak sits inside the signal window. The scratch buffer
            // is sized per mode — one entry per fast-axis point for raster, one
            // per A-line for trajectory — so the index has to follow the same
            // rule. The bounds check turns a future mismatch into a dropped
            // sample rather than memory corruption.
            const int depthSlot = raster ? (m_pos % m_nFast) : m_pos;
            if (depthSlot >= 0 && depthSlot < static_cast<int>(m_depthRow.size()))
                m_depthRow[depthSlot] = static_cast<float>(peakDepth);

            {
                std::lock_guard<std::mutex> lock(m_latestMutex);
                m_latestAline.assign(m_workRaw.begin(),
                                     m_workRaw.begin() + windowDepth);
            }
            if (m_total == 0) emit firstDataReceived();

            if (raster) {
                const int fastIndex = m_pos % m_nFast;
                const int slowIndex = m_pos / m_nFast;
                const int destination = fastIndex * effectiveDepth;
                std::copy(m_workEnvelope.begin(),
                          m_workEnvelope.begin() + windowDepth,
                          m_currentBscan.begin() + destination);
                std::copy(m_workRaw.begin(), m_workRaw.begin() + windowDepth,
                          m_currentRawBscan.begin() + destination);
                if (windowDepth < effectiveDepth) {
                    std::fill(m_currentBscan.begin() + destination + windowDepth,
                              m_currentBscan.begin() + destination + effectiveDepth, 0.0);
                    std::fill(m_currentRawBscan.begin() + destination + windowDepth,
                              m_currentRawBscan.begin() + destination + effectiveDepth, 0.0);
                }
                m_currentMipRow[fastIndex] = peak;

                if (fastIndex == m_nFast - 1) {
                    if (m_displayPolling) {
                        m_displayMailbox.publishRasterRow(m_frameIdx, slowIndex,
                            m_nFast, m_nSlow, effectiveDepth, m_currentBscan,
                            m_currentMipRow, m_depthRow);
                    } else {
                        emit bscanDataReady(slowIndex, m_currentBscan,
                                            effectiveDepth, m_nFast);
                        emit cscanRowReady(slowIndex, m_currentMipRow);
                        if (m_depthEncode.load(std::memory_order_relaxed))
                            emit cscanDepthRowReady(slowIndex, m_depthRow);
                        emit rasterRowCompleted(slowIndex + 1, m_nSlow);
                    }

                    // Accumulate the final C-scan MIP (saved as PNG per frame)
                    if (slowIndex >= 0 && slowIndex < m_nSlow) {
                        for (int fast = 0; fast < m_nFast; ++fast) {
                            const size_t pixel = static_cast<size_t>(fast) * m_nSlow + slowIndex;
                            m_cscanAccum[pixel] = m_currentMipRow[fast];
                            // Same (fast, slow) indexing as the intensity plane,
                            // so the depth image comes out in exactly the same
                            // orientation as cscan_*.png.
                            m_cscanDepth[pixel] = m_depthRow[fast];
                        }
                    }

                    if (m_fileWriter && m_fileWriter->isEnabled() && m_saveDat) {
                        auto row = std::make_shared<std::vector<double>>();
                        row->swap(m_currentRawBscan);
                        SaveTask task;
                        task.mode = ScanMode::Raster;
                        task.rasterData = std::move(row);
                        task.total = m_nFast;
                        task.depth = effectiveDepth;
                        // Global row number across the whole session. FileWriter
                        // converts it back to (frame, row-within-frame) so that a
                        // continuous session gets one folder per frame.
                        task.frameIdx = slowIndex + m_frameIdx * m_nSlow;
                        task.dataFormat = m_saveDataFormat;
                        task.saveDat = true;
                        queueSaveTask(std::move(task));
                        m_currentRawBscan.assign(
                            static_cast<size_t>(m_nFast) * effectiveDepth, 0.0);
                    } else {
                        std::fill(m_currentRawBscan.begin(),
                                  m_currentRawBscan.end(), 0.0);
                    }
                    std::fill(m_currentBscan.begin(), m_currentBscan.end(), 0.0);
                    std::fill(m_currentMipRow.begin(), m_currentMipRow.end(), 0.0f);
                }
            } else {
                // Capture acquisition-order waveforms independently of saving
                // and of the bilinear C-scan accumulator below.
                if (m_pos >= bscanFirst && m_pos <= bscanLast) {
                    const size_t destination =
                        static_cast<size_t>(m_pos - bscanFirst) * effectiveDepth;
                    std::copy_n(m_workRaw.begin(), windowDepth,
                                trajectoryBscan.begin() + destination);
                    if (m_pos == bscanLast) {
                        if (m_displayPolling)
                            m_displayMailbox.publishBscan(m_frameIdx, bscanFirst + 1,
                                bscanLast + 1, effectiveDepth, trajectoryBscan);
                        else emit trajectoryBscanReady(m_frameIdx, bscanFirst + 1,
                                                  bscanLast + 1, trajectoryBscan, effectiveDepth);
                    }
                }
                if (saveTrajectory) {
                    FrameBuf &buffer = m_bufPool[m_activeBuf];
                    const size_t destination = static_cast<size_t>(m_pos) * effectiveDepth;
                    int16_t *output = buffer.rawAlines.get();
                    output += destination;
                    std::copy(aline + windowOffset,
                              aline + windowOffset + windowDepth,
                              output);
                    if (windowDepth < effectiveDepth) {
                        std::fill(output + windowDepth, output + effectiveDepth, 0);
                    }
                    buffer.alinePeaks[m_pos] = peak;
                }

                const MapInfo &mapping = (*m_mapInfo)[m_pos];
                MipAccum &accumulator = m_mipPool[m_activeMipSlot];
                const float depthValue = static_cast<float>(peakDepth);
                accumulator.sum[mapping.idx00] += peak * mapping.w00;
                accumulator.weight[mapping.idx00] += mapping.w00;
                accumulator.depthSum[mapping.idx00] += depthValue * peak * mapping.w00;
                accumulator.sum[mapping.idx01] += peak * mapping.w01;
                accumulator.weight[mapping.idx01] += mapping.w01;
                accumulator.depthSum[mapping.idx01] += depthValue * peak * mapping.w01;
                accumulator.sum[mapping.idx10] += peak * mapping.w10;
                accumulator.weight[mapping.idx10] += mapping.w10;
                accumulator.depthSum[mapping.idx10] += depthValue * peak * mapping.w10;
                accumulator.sum[mapping.idx11] += peak * mapping.w11;
                accumulator.weight[mapping.idx11] += mapping.w11;
                accumulator.depthSum[mapping.idx11] += depthValue * peak * mapping.w11;
            }

            ++m_pos;
            ++m_total;
            // Published for offline tests: the queue length alone cannot say
            // whether the processing loop is done, because a bulk dequeue may
            // still hold samples the queue no longer counts.
            m_processedAlines.store(m_total, std::memory_order_release);
            if (m_pos % std::max(1, m_updateInterval) == 0 ||
                m_pos == totalPerFrame) {
                if (m_displayPolling) m_displayMailbox.publishProgress(m_pos, totalPerFrame);
                else emit scanProgress(m_pos, totalPerFrame);
            }

            if (m_pos >= totalPerFrame) {
                if (!continuous) {
                    // Ask the GUI-thread controller to stop FPGA first, then
                    // the card. This also unblocks acquisitionLoop::waitForData.
                    m_stopRequested.store(1, std::memory_order_release);
                    emit acquisitionComplete();
                }
                if (!raster) {
                    // Reduce the finished accumulator into the depth plane BEFORE
                    // handoffMipFrame() runs: that call advances m_activeMipSlot
                    // to the next spare slot, so reading m_mipPool after it sees
                    // an empty slot. That is how the depth plane used to come out
                    // all zeros — a black depth image and a black saved PNG —
                    // while the same mistake also destroyed the raw `sum` the
                    // intensity view normalizes.
                    //
                    // m_mipDepthPlane is plain pipeline state, so it survives the
                    // releaseMipSlot() the GUI performs later; finishMipFrame
                    // emits it on this same thread, right after this block.
                    m_depthPlaneValid = false;
                    if (m_depthEncode.load(std::memory_order_relaxed) || saveDepth) {
                        depthPlaneFromSlot(m_mipPool[m_activeMipSlot]);
                        m_depthPlaneValid = true;
                    }
                    if (m_displayPolling) {
                        auto &slot = m_mipPool[m_activeMipSlot];
                        m_displayMailbox.publishMip(m_frameIdx, m_imageW, m_imageH, slot,
                            m_depthPlaneValid ? &m_mipDepthPlane : nullptr);
                        std::fill(slot.sum.begin(), slot.sum.end(), 0.0f);
                        std::fill(slot.weight.begin(), slot.weight.end(), 0.0f);
                        std::fill(slot.depthSum.begin(), slot.depthSum.end(), 0.0f);
                    } else {
                        handoffMipFrame();
                        if (m_pendingMipSlot >= 0) {
                            finishMipFrame(m_pendingMipSlot, m_pendingMipFrameIdx);
                            m_pendingMipSlot = -1;
                        }
                    }
                    if (saveTrajectory) {
                        int nextSlot = -1;
                        {
                            std::unique_lock<std::mutex> lock(m_bufPoolMutex);
                            m_bufPoolCv.wait(lock, [this] {
                                return !m_freeBufs.empty();
                            });
                            if (!m_freeBufs.empty()) {
                                nextSlot = m_freeBufs.back();
                                m_freeBufs.pop_back();
                            }
                        }
                        if (nextSlot < 0) goto processingExit;

                        const int completedSlot = m_activeBuf;
                        m_activeBuf = nextSlot;
                        auto completed = std::make_shared<FrameBuf>();
                        completed->rawAlines =
                            std::move(m_bufPool[completedSlot].rawAlines);
                        completed->rawSize = m_bufPool[completedSlot].rawSize;
                        completed->alinePeaks =
                            std::move(m_bufPool[completedSlot].alinePeaks);

                        SaveTask task;
                        task.mode = ScanMode::Trajectory;
                        task.trajectoryData = std::move(completed);
                        task.total = totalPerFrame;
                        task.depth = effectiveDepth;
                        task.frameIdx = m_frameIdx;
                        task.bufferSlot = completedSlot;
                        task.colorMap = m_colorMap.load(std::memory_order_relaxed);
                        task.colorMapping = m_colorMapping.load(std::memory_order_relaxed);
                        task.colorClipping = m_colorClipping.load(std::memory_order_relaxed);
                        task.dataFormat = m_saveDataFormat;
                        task.saveNpy = m_saveNpy;
                        task.saveMip = m_saveMip;
                        task.savePcd = m_savePcd;
                        task.saveDatTraj = m_saveTrajDat;
                        task.segMode = m_saveSegMode;
                        task.segThreshold = m_saveSegThreshold;
                        task.scanRadiusM = m_saveRadiusM;
                        task.saveDepthPng = saveDepth;
                        if (saveDepth) {
                            // Copy, not share: m_mipDepthPlane is reused by the
                            // next frame while this task may still be queued.
                            task.depthMap =
                                std::make_shared<std::vector<float>>(m_mipDepthPlane);
                            task.depthConfig = depthConfigSnapshot();
                            task.frameW = m_mipDepthW;
                            task.frameH = m_mipDepthH;
                        }
                        queueSaveTask(std::move(task));
                    }
                } else {
                    // Raster frame complete: persist the final C-scan MIP PNG
                    // (colored with the live foreground mapping), then reset.
                    const size_t accumSize = static_cast<size_t>(m_nFast) * m_nSlow;
                    std::shared_ptr<std::vector<float>> completedMip;
                    if ((m_savePng || saveDepth) && m_fileWriter &&
                        m_fileWriter->isEnabled() && m_cscanAccum.size() >= accumSize) {
                        completedMip = std::make_shared<std::vector<float>>();
                        completedMip->swap(m_cscanAccum);
                    }
                    if (m_savePng && completedMip) {
                        SaveTask t;
                        t.mode = ScanMode::Raster;
                        t.cscanMip = completedMip;
                        t.cscanW = m_nSlow;
                        t.cscanH = m_nFast;
                        t.frameIdx = m_frameIdx;
                        t.colorMap = m_colorMap.load(std::memory_order_relaxed);
                        t.colorMapping = m_colorMapping.load(std::memory_order_relaxed);
                        t.colorClipping = m_colorClipping.load(std::memory_order_relaxed);
                        t.dataFormat = m_saveDataFormat;
                        t.savePng = true;
                        queueSaveTask(std::move(t));
                    }
                    if (saveDepth && m_cscanDepth.size() >= accumSize) {
                        auto depth = std::make_shared<std::vector<float>>();
                        depth->swap(m_cscanDepth);
                        SaveTask t;
                        t.mode = ScanMode::Raster;
                        t.depthMap = std::move(depth);
                        t.cscanMip = completedMip;
                        // Same orientation as the intensity C-scan: width is
                        // the slow axis, height the fast axis.
                        t.cscanW = m_nSlow;
                        t.cscanH = m_nFast;
                        t.frameIdx = m_frameIdx;
                        t.depthConfig = depthConfigSnapshot();
                        t.depth = effectiveDepth;   // the depth axis for the colors
                        t.saveDepthPng = true;
                        queueSaveTask(std::move(t));
                        m_cscanDepth.assign(accumSize, 0.0f);
                    }
                    m_cscanAccum.assign(accumSize, 0.0f);
                }

                ++m_frameIdx;
                m_pos = 0;
                if (!continuous) goto processingExit;
                if (!m_displayPolling) emit frameStarted(m_frameIdx);
            }
        }

        if (m_pendingMipSlot >= 0) {
            finishMipFrame(m_pendingMipSlot, m_pendingMipFrameIdx);
            m_pendingMipSlot = -1;
        }
    }

processingExit:
    if (m_pendingMipSlot >= 0) {
        finishMipFrame(m_pendingMipSlot, m_pendingMipFrameIdx);
        m_pendingMipSlot = -1;
    }
}
