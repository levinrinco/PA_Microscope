#pragma once

#include "Types.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// One pending display state, NOT an acquisition or save queue. The GUI polls
// this mailbox; a stalled GUI cannot retain an unbounded history of waveforms.
struct DisplayBscan {
    bool trajectory = false;
    int frame = 0, row = 0, first = 1, last = 1, depth = 0;
    std::vector<double> samples;
};

struct DisplayCscan {
    bool trajectory = false;
    int frame = -1, width = 0, height = 0, completedRows = 0;
    std::vector<float> intensity, depth;
};

struct DisplayUpdate {
    std::optional<DisplayBscan> bscan;
    std::optional<DisplayCscan> cscan;
    std::optional<std::pair<int, int>> progress;
    std::optional<FifoStatus> fifo;
};

class DisplayMailbox {
public:
    void publishBscan(int frame, int first, int last, int depth,
                      const std::vector<double> &samples) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setBscan(true, frame, 0, first, last, depth, samples);
    }

    void publishRasterRow(int frame, int row, int nFast, int nSlow, int depth,
                          const std::vector<double> &bscan,
                          const std::vector<float> &intensity,
                          const std::vector<float> &depthRow) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cscan.frame != frame || m_cscan.width != nSlow || m_cscan.height != nFast) {
            m_cscan = {};
            m_cscan.frame = frame;
            m_cscan.width = nSlow;
            m_cscan.height = nFast;
            m_cscan.intensity.assign(static_cast<size_t>(nFast) * nSlow, 0.0f);
            m_cscan.depth.assign(m_cscan.intensity.size(), 0.0f);
        }
        // Keep ALL columns of the current raster frame, even if the GUI skips
        // intermediate repaints. Reset only when the next frame has data.
        for (int fast = 0; fast < nFast; ++fast) {
            const size_t index = static_cast<size_t>(fast) * nSlow + row;
            m_cscan.intensity[index] = intensity[fast];
            m_cscan.depth[index] = depthRow[fast];
        }
        m_cscan.completedRows = row + 1;
        m_cscanDirty = true;
        setBscan(false, frame, row, 1, nFast, depth, bscan);
    }

    void publishMip(int frame, int width, int height, const MipAccum &slot,
                    const std::vector<float> *depth) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cscan.trajectory = true;
        m_cscan.frame = frame;
        m_cscan.width = width;
        m_cscan.height = height;
        m_cscan.intensity.resize(slot.sum.size());
        for (size_t i = 0; i < slot.sum.size(); ++i)
            m_cscan.intensity[i] = slot.weight[i] > 0.0f ? slot.sum[i] / slot.weight[i] : 0.0f;
        if (depth) m_cscan.depth = *depth;
        else m_cscan.depth.clear();
        // Intensity and depth are published under the same lock, never mixed
        // across frames when the GUI skips a completed frame.
        m_cscanDirty = true;
    }

    void publishProgress(int current, int total) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_progress = std::make_pair(current, total);
    }
    void publishFifo(const FifoStatus &status) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_fifo = status;
    }

    DisplayUpdate take() {
        DisplayUpdate update;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            update.bscan.swap(m_bscan);
            if (m_cscanDirty) {
                update.cscan = m_cscan;
                m_cscanDirty = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            update.progress.swap(m_progress);
            update.fifo.swap(m_fifo);
        }
        return update;
    }

private:
    void setBscan(bool trajectory, int frame, int row, int first, int last,
                  int depth, const std::vector<double> &samples) {
        if (!m_bscan) m_bscan.emplace();
        auto &b = *m_bscan;
        b.trajectory = trajectory;
        b.frame = frame;
        b.row = row;
        b.first = first;
        b.last = last;
        b.depth = depth;
        b.samples = samples;
    }
    std::mutex m_mutex;
    std::optional<DisplayBscan> m_bscan;
    DisplayCscan m_cscan;
    bool m_cscanDirty = false;
    // Acquisition status must never wait behind a large GUI image copy.
    std::mutex m_statusMutex;
    std::optional<std::pair<int, int>> m_progress;
    std::optional<FifoStatus> m_fifo;
};
