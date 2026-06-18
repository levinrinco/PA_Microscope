#include "DataPipeline.h"
#include "SpectrumFifo.h"
#include "FileWriter.h"
#include <QDebug>
#include <algorithm>
#include <chrono>
#include <cmath>

extern uint32_t hotLUT[256];
extern int mapToIndex(double value, double maxVal, int mapping, bool clipping);

// ===== Lifecycle =====

DataPipeline::DataPipeline(SpectrumFifo *fifo, QObject *parent)
    : QObject(parent), m_fifo(fifo)
{
    m_replenishThread = std::thread(&DataPipeline::replenishWorker, this);
    m_saveThread = std::thread(&DataPipeline::saveWorker, this);
}

DataPipeline::~DataPipeline()
{
    m_replenishStop.store(true);
    m_replenishCv.notify_one();
    m_saveStop.store(true);
    m_saveCv.notify_one();
    if (m_replenishThread.joinable()) m_replenishThread.join();
    if (m_saveThread.joinable()) m_saveThread.join();
}

void DataPipeline::requestStop() { m_stopRequested.store(1, std::memory_order_release); }

std::vector<double> DataPipeline::getLatestAline()
{ std::lock_guard<std::mutex> lk(m_latestMutex); return m_latestAline; }

// ===== Replenish =====

void DataPipeline::replenishWorker()
{
    while (!m_replenishStop.load(std::memory_order_acquire)) {
        ReplenishTask task;
        { std::unique_lock<std::mutex> lk(m_replenishMutex);
          m_replenishCv.wait(lk, [this]{ return m_replenishStop.load() || !m_replenishQueue.empty(); });
          if (m_replenishStop.load() && m_replenishQueue.empty()) break;
          task = m_replenishQueue.front(); m_replenishQueue.pop(); }
        m_bufPool[task.slot].rawAlines.reset(new int16_t[task.fs]);
        m_bufPool[task.slot].rawSize = task.fs;
        m_bufPool[task.slot].alinePeaks.assign(task.total, 0.0f);
        std::lock_guard<std::mutex> lk(m_bufPoolMutex);
        m_freeBufs.push_back(task.slot);
    }
}

// ===== Save =====

void DataPipeline::saveWorker()
{
    while (!m_saveStop.load(std::memory_order_acquire)) {
        SaveTask task;
        { std::unique_lock<std::mutex> lk(m_saveMutex);
          m_saveCv.wait(lk, [this]{ return m_saveStop.load() || !m_saveQueue.empty(); });
          if (m_saveStop.load() && m_saveQueue.empty()) break;
          task = std::move(m_saveQueue.front()); m_saveQueue.pop(); }
        if (m_fileWriter && m_saveMapInfo && m_fileWriter->isEnabled())
            m_fileWriter->saveGriddedVolume(task.data->rawAlines.get(), task.total, task.ed,
                                            *m_saveMapInfo, m_saveW, m_saveH, task.frameIdx);
    }
}

// ===== MIP =====

void DataPipeline::handoffMipFrame()
{
    int completed = m_activeMipSlot, next;
    { std::lock_guard<std::mutex> lk(m_mipPoolMutex);
      if (m_freeMipSlots.empty()) {
          m_mipExhaustCount++;
          std::fill(m_mipPool[completed].sum.begin(), m_mipPool[completed].sum.end(), 0.0f);
          std::fill(m_mipPool[completed].weight.begin(), m_mipPool[completed].weight.end(), 0.0f);
          return;
      }
      next = m_freeMipSlots.back(); m_freeMipSlots.pop_back(); }
    m_activeMipSlot = next;
    m_pendingMipSlot = completed;
    m_pendingMipFrameIdx = m_frameIdx;
}

void DataPipeline::releaseMipSlot(int slotIdx)
{
    std::fill(m_mipPool[slotIdx].sum.begin(), m_mipPool[slotIdx].sum.end(), 0.0f);
    std::fill(m_mipPool[slotIdx].weight.begin(), m_mipPool[slotIdx].weight.end(), 0.0f);
    std::lock_guard<std::mutex> lk(m_mipPoolMutex);
    m_freeMipSlots.push_back(slotIdx);
}

// ===== run() =====

void DataPipeline::run()
{
    if (!m_mapInfo || m_mapInfo->empty()) { emit errorOccurred("No MapInfo"); return; }

    int depth = m_liveDepth.load(), seg = m_segmentSize;
    int total = static_cast<int>(m_mapInfo->size());
    bool cont = m_continuous.load();
    int ed = (seg < depth) ? seg : depth;

    qInfo() << "==============================";
    qInfo() << "Pipeline START  seg=" << seg << " effDepth=" << ed << " alines=" << total;
    qInfo() << "==============================";

    m_stopRequested.store(0);
    m_effDepth = ed; m_frameSize = total * ed; m_totalAlinesPerFrame = total;
    m_diagEnqueued.store(0);

    // Queue init
    size_t cap = static_cast<size_t>(total) * ed * 2;
    m_rawQueue.reset(new moodycamel::ConcurrentQueue<int16_t>(cap));
    m_enqToken.reset(new moodycamel::ProducerToken(*m_rawQueue));
    m_deqToken.reset(new moodycamel::ConsumerToken(*m_rawQueue));

    // MIP pool
    int gs = m_imageW * m_imageH;
    for (int i = 0; i < 3; ++i) { m_mipPool[i].sum.assign(gs, 0.0f); m_mipPool[i].weight.assign(gs, 0.0f); }
    { std::lock_guard<std::mutex> lk(m_mipPoolMutex); m_freeMipSlots = {1,2}; }
    m_activeMipSlot = 0; m_mipExhaustCount = 0;

    // FrameBuf pool
    for (int b = 0; b < 3; ++b) { m_bufPool[b].rawAlines.reset(new int16_t[m_frameSize]); m_bufPool[b].rawSize = m_frameSize; m_bufPool[b].alinePeaks.assign(total, 0.0f); }
    { std::lock_guard<std::mutex> lk(m_bufPoolMutex); m_freeBufs = {1,2}; }
    m_activeBuf = 0;

    m_workEnvelope.assign(seg, 0.0); m_workRaw.assign(seg, 0.0);
    m_depthWarningEmitted.store(false);
    m_pos = 0; m_total = 0; m_frameIdx = 0; m_pendingMipSlot = -1;

    std::thread acq(&DataPipeline::acquisitionLoop, this);
    std::thread proc(&DataPipeline::processingLoop, this);
    acq.join(); proc.join();

    m_saveStop.store(true); m_saveCv.notify_one();
    m_replenishStop.store(true); m_replenishCv.notify_one();

    int64_t exp = int64_t(m_frameIdx) * total;
    int64_t diff = m_total - exp;
    qInfo() << "Pipeline END: frames=" << m_frameIdx << "alines=" << m_total << "diff=" << diff << "exhausts=" << m_mipExhaustCount;
    if (diff) qWarning() << "*** A-LINE MISMATCH *** diff=" << diff;
    emit scanFinished();
}

// ===== Acquisition =====

void DataPipeline::acquisitionLoop()
{
#ifdef Q_OS_WIN
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    int seg = m_segmentSize;
    int batch = seg * 256;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        uint32_t err = m_fifo->waitForData();
        if (err == ERR_TIMEOUT) continue;
        if (err == ERR_ABORT) break;
        if (err != ERR_OK) { emit errorOccurred(SpectrumFifo::translateError(err)); break; }

        FifoStatus st = m_fifo->readFifoStatus();
        if (st.availUserBytes < static_cast<int64_t>(m_fifo->notifySize())) continue;

        int64_t maxS = (m_fifo->bufferSize() - st.availUserPos) / sizeof(int16_t);
        int64_t raw = st.availUserBytes / sizeof(int16_t);
        int64_t avail = (raw < maxS) ? raw : maxS;
        if (avail < seg) { if (avail > 0) m_fifo->releaseData(avail * sizeof(int16_t)); continue; }

        int16_t *src = m_fifo->dataBufferAt(st.availUserPos);
        int64_t rem = avail, enq = 0;
        while (rem >= seg) {
            int64_t n = (rem < batch) ? rem : batch;
            n = (n / seg) * seg;
            m_rawQueue->enqueue_bulk(*m_enqToken, src, (size_t)n);
            src += n; rem -= n; enq += n;
        }
        if (enq > 0) { m_diagEnqueued.fetch_add(enq); m_fifo->releaseData(enq * sizeof(int16_t)); }
    }
}

// ===== Processing =====

void DataPipeline::processingLoop()
{
    int seg = m_segmentSize, total = m_totalAlinesPerFrame, ed = m_effDepth;
    bool cont = m_continuous.load();
    int batch = seg * 256, depth = m_liveDepth.load();
    std::vector<int16_t> wbuf(batch);

    while (true) {
        size_t dq = m_rawQueue->try_dequeue_bulk(*m_deqToken, wbuf.data(), batch);
        if (dq == 0) { if (m_stopRequested.load()) break; std::this_thread::sleep_for(std::chrono::microseconds(50)); continue; }
        if (dq % seg) qWarning() << "[ALIGN] dq" << dq << "seg" << seg;
        int nAl = (int)(dq / seg);
        FrameBuf *buf = &m_bufPool[m_activeBuf];

        for (int a = 0; a < nAl; ++a) {
            const int16_t *ap = &wbuf[a * seg];
            int wo = m_liveWindowOffset.load(), win = m_liveDepth.load();
            if (win > depth) win = depth; if (wo < 0) wo = 0; if (wo >= seg) wo = 0;
            if (wo + win > seg) { win = seg - wo; if (!m_depthWarningEmitted.exchange(true)) qWarning() << "clamping depth to" << win; if (win <= 0) { win = seg; wo = 0; } }

            double dc = 0; for (int i = 0; i < win; ++i) dc += ap[wo + i]; dc /= win;
            float peak = 0;
            for (int i = 0; i < win; ++i) { double v = std::abs((double)ap[wo+i] - dc); m_workEnvelope[i] = v; m_workRaw[i] = (double)ap[wo+i] - dc; if (v > peak) peak = (float)v; }

            int off = m_pos * ed, cl = std::min(win, ed);
            std::copy(ap + wo, ap + wo + cl, buf->rawAlines.get() + off);
            if (cl < ed) std::fill(buf->rawAlines.get() + off + cl, buf->rawAlines.get() + off + ed, 0);
            buf->alinePeaks[m_pos] = peak;

            float p = std::max(peak, 0.0f); const MapInfo &mi = (*m_mapInfo)[m_pos]; MipAccum &acc = m_mipPool[m_activeMipSlot];
            acc.sum[mi.idx00] += p * mi.w00; acc.weight[mi.idx00] += mi.w00;
            acc.sum[mi.idx01] += p * mi.w01; acc.weight[mi.idx01] += mi.w01;
            acc.sum[mi.idx10] += p * mi.w10; acc.weight[mi.idx10] += mi.w10;
            acc.sum[mi.idx11] += p * mi.w11; acc.weight[mi.idx11] += mi.w11;

            { std::lock_guard<std::mutex> lk(m_latestMutex); m_latestAline.assign(m_workRaw.begin(), m_workRaw.begin() + win); }
            ++m_pos; ++m_total;
            if (m_pos % 1000 == 0) emit scanProgress(m_pos, total);

            if (m_pos >= total) {
                m_pos = 0; handoffMipFrame();
                int completed = m_activeBuf;
                { std::lock_guard<std::mutex> lk(m_bufPoolMutex); m_activeBuf = m_freeBufs.back(); m_freeBufs.pop_back(); }
                auto sp = std::make_shared<FrameBuf>();
                sp->rawAlines = std::move(m_bufPool[completed].rawAlines);
                sp->rawSize = m_bufPool[completed].rawSize;
                sp->alinePeaks = std::move(m_bufPool[completed].alinePeaks);
                { std::lock_guard<std::mutex> lk(m_replenishMutex); m_replenishQueue.push({completed, total * ed, total}); }
                m_replenishCv.notify_one();
                { std::lock_guard<std::mutex> lk(m_saveMutex); m_saveQueue.push({std::move(sp), total, ed, m_frameIdx}); }
                m_saveCv.notify_one();
                ++m_frameIdx; buf = &m_bufPool[m_activeBuf];
                if (!cont) goto procExit;
            }
        }
        if (m_pendingMipSlot >= 0) { emit mipFrameCompleted(m_pendingMipSlot, m_pendingMipFrameIdx); m_pendingMipSlot = -1; }
    }
procExit:
    if (m_pendingMipSlot >= 0) { emit mipFrameCompleted(m_pendingMipSlot, m_pendingMipFrameIdx); m_pendingMipSlot = -1; }
}
