#include "FileWriter.h"
#include <QDir>
#include <QImage>
#include <QFile>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <thread>

extern uint32_t hotLUT[256];
extern int mapToIndex(double value, double maxVal, int mapping, bool clipping);

FileWriter::FileWriter(QObject *parent)
    : QObject(parent) {}
FileWriter::~FileWriter() = default;

void FileWriter::setSavePath(const QString &dir) { m_savePath = dir; }
void FileWriter::setEnabled(bool enabled)        { m_enabled = enabled; }

void FileWriter::beginSession()
{
    QDir baseDir(m_savePath);
    if (!baseDir.exists())
        baseDir.mkpath(".");

    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    m_sessionDir = ts;
    if (!baseDir.mkdir(ts))
        qWarning() << "FileWriter: cannot create session dir" << ts;
    else
        qInfo() << "FileWriter: session" << ts;
}

bool FileWriter::saveGriddedVolume(const int16_t *rawAlines,
                                   int alineCount, int depth,
                                   const std::vector<MapInfo> &mapInfo,
                                   int imageW, int imageH,
                                   int frameIdx)
{
    if (!m_enabled) return true;
    if (m_sessionDir.isEmpty()) return false;

    QDir baseDir(m_savePath);

    QString dir = baseDir.absoluteFilePath(m_sessionDir);

    // Grid
    std::vector<float> volume, mip;
    gridVolume(rawAlines, alineCount, depth, mapInfo, imageW, imageH, volume, mip);

    // Save
    QString mipPath = dir + QString("/mip_%1.png").arg(frameIdx, 5, 10, QChar('0'));
    QString volPath = dir + QString("/volume_%1.npy").arg(frameIdx, 5, 10, QChar('0'));

    if (!saveMipPng(mipPath, mip, imageW, imageH)) return false;
    if (!saveVolumeNpy(volPath, volume, imageW, imageH, depth)) return false;
    return true;
}

// ===== 3D Gridding (multi-threaded) =====

struct ThreadAccum {
    std::vector<float> volume;
    std::vector<float> weight;
    std::vector<float> mip;
};

static void gridChunk(const int16_t *rawAlines,
                      const std::vector<MapInfo> &mapInfo,
                      int startIdx, int endIdx,
                      int depth, int imageW, int imageH,
                      ThreadAccum &out)
{
    int totalVoxels = imageW * imageH * depth;
    out.volume.assign(totalVoxels, 0.0f);
    out.weight.assign(totalVoxels, 0.0f);
    out.mip.assign(imageW * imageH, 0.0f);

    for (int i = startIdx; i < endIdx; ++i) {
        const int16_t *raw = rawAlines + static_cast<size_t>(i) * depth;
        const MapInfo &m = mapInfo[i];

        double dcSum = 0.0;
        for (int d = 0; d < depth; ++d) dcSum += static_cast<double>(raw[d]);
        double dcMean = dcSum / depth;

        for (int d = 0; d < depth; ++d) {
            float val = std::abs(static_cast<float>(static_cast<double>(raw[d]) - dcMean));

            int idx00 = m.idx00 * depth + d;
            out.volume[idx00] += val * m.w00; out.weight[idx00] += m.w00;
            if (val > out.mip[m.idx00]) out.mip[m.idx00] = val;

            int idx01 = m.idx01 * depth + d;
            out.volume[idx01] += val * m.w01; out.weight[idx01] += m.w01;
            if (val > out.mip[m.idx01]) out.mip[m.idx01] = val;

            int idx10 = m.idx10 * depth + d;
            out.volume[idx10] += val * m.w10; out.weight[idx10] += m.w10;
            if (val > out.mip[m.idx10]) out.mip[m.idx10] = val;

            int idx11 = m.idx11 * depth + d;
            out.volume[idx11] += val * m.w11; out.weight[idx11] += m.w11;
            if (val > out.mip[m.idx11]) out.mip[m.idx11] = val;
        }
    }
}

void FileWriter::gridVolume(const int16_t *rawAlines,
                            int alineCount, int depth,
                            const std::vector<MapInfo> &mapInfo,
                            int imageW, int imageH,
                            std::vector<float> &volumeOut,
                            std::vector<float> &mipOut)
{
    int totalVoxels = imageW * imageH * depth;

    unsigned int nThreads = std::thread::hardware_concurrency();
    if (nThreads < 2 || alineCount < 5000) {
        // Single-threaded for small data or single-core
        ThreadAccum acc;
        gridChunk(rawAlines, mapInfo, 0, alineCount, depth, imageW, imageH, acc);
        volumeOut.swap(acc.volume);
        mipOut.swap(acc.mip);
        for (int i = 0; i < totalVoxels; ++i)
            if (acc.weight[i] > 0.0f) volumeOut[i] /= acc.weight[i];
        return;
    }

    if (nThreads > 8) nThreads = 8;  // diminishing returns beyond 8

    qInfo() << "Gridding with" << nThreads << "threads,"
            << alineCount << "alines";

    std::vector<ThreadAccum> results(nThreads);
    std::vector<std::thread> threads;
    threads.reserve(nThreads);

    int chunkSize = alineCount / static_cast<int>(nThreads);
    for (unsigned int t = 0; t < nThreads; ++t) {
        int start = static_cast<int>(t) * chunkSize;
        int end = (t == nThreads - 1) ? alineCount : start + chunkSize;
        threads.emplace_back(gridChunk,
                             std::cref(rawAlines), std::cref(mapInfo),
                             start, end,
                             depth, imageW, imageH,
                             std::ref(results[t]));
    }

    for (auto &th : threads) th.join();

    // Merge: volume/weight = element-wise sum; MIP = element-wise max
    volumeOut.assign(totalVoxels, 0.0f);
    std::vector<float> weightAccum(totalVoxels, 0.0f);
    mipOut.assign(imageW * imageH, 0.0f);

    for (unsigned int t = 0; t < nThreads; ++t) {
        for (int i = 0; i < totalVoxels; ++i) {
            volumeOut[i] += results[t].volume[i];
            weightAccum[i] += results[t].weight[i];
        }
        for (int j = 0; j < imageW * imageH; ++j) {
            if (results[t].mip[j] > mipOut[j])
                mipOut[j] = results[t].mip[j];
        }
    }

    // Normalize
    for (int i = 0; i < totalVoxels; ++i)
        if (weightAccum[i] > 0.0f) volumeOut[i] /= weightAccum[i];
}

// ===== MIP → PNG =====

bool FileWriter::saveMipPng(const QString &path, const std::vector<float> &mip,
                            int imageW, int imageH)
{
    if (mip.empty()) return false;

    float maxVal = 1e-9f;
    for (float v : mip) if (v > maxVal) maxVal = v;
    double displayMax = static_cast<double>(maxVal) * 1.1;
    if (displayMax < 1e-9) displayMax = 1.0;

    QImage img(imageW, imageH, QImage::Format_RGB888);  // width=W, height=H

    unsigned int nThreads = std::thread::hardware_concurrency();
    if (nThreads < 2) nThreads = 1;
    if (nThreads > 8) nThreads = 8;

    std::vector<std::thread> threads;
    int chunkH = imageH / static_cast<int>(nThreads);
    for (unsigned int t = 0; t < nThreads; ++t) {
        int y0 = static_cast<int>(t) * chunkH;
        int y1 = (t == nThreads - 1) ? imageH : y0 + chunkH;
        threads.emplace_back([&, y0, y1]() {
            for (int y = y0; y < y1; ++y) {
                int rowBase = y * imageW;
                uchar *row = img.scanLine(y);  // row y
                for (int x = 0; x < imageW; ++x) {
                    int idx = mapToIndex(mip[rowBase + x], displayMax, 0, false);
                    uint32_t c = hotLUT[idx];
                    row[x * 3 + 0] = c & 0xFF;
                    row[x * 3 + 1] = (c >> 8) & 0xFF;
                    row[x * 3 + 2] = (c >> 16) & 0xFF;
                }
            }
        });
    }
    for (auto &th : threads) th.join();

    if (!img.save(path, "PNG")) {
        emit errorOccurred(QString("Cannot save PNG: %1").arg(path));
        return false;
    }
    qInfo() << "MIP saved:" << path;
    return true;
}

// ===== Volume → NPY =====
//
// NPY format (https://numpy.org/devdocs/reference/generated/numpy.lib.format.html):
//   6 bytes  magic: \x93NUMPY
//   1 byte   major version
//   1 byte   minor version
//   2 bytes  header length (uint16 LE)
//   N bytes  header: Python dict literal, space-padded to 8-byte alignment
//   ...      raw data in C order, float32 little-endian

bool FileWriter::saveVolumeNpy(const QString &path, const std::vector<float> &volume,
                               int imageW, int imageH, int depth)
{
    // Build header dict string
    // shape = (depth, height, width) — C order
    std::ostringstream hdr;
    hdr << "{'descr': '<f4', 'fortran_order': False, "
        << "'shape': (" << depth << ", " << imageH << ", " << imageW << ")}";

    std::string hdrStr = hdr.str();
    // Pad with spaces to align (header_len + 10) to 8 bytes
    // 10 = magic(6) + version(2) + header_len(2) = 10 bytes before data
    size_t totalHeaderLen = 10 + hdrStr.size();
    size_t padding = (8 - (totalHeaderLen % 8)) % 8;
    hdrStr.append(padding, ' ');
    // The padding must include a newline before spaces for v1.0 compatibility
    // Actually simpler: just add a newline + spaces
    hdrStr = hdr.str();
    hdrStr += '\n';
    // Re-pad
    totalHeaderLen = 10 + hdrStr.size();
    padding = (8 - (totalHeaderLen % 8)) % 8;
    hdrStr.append(padding, ' ');

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        emit errorOccurred(QString("Cannot create: %1").arg(path));
        return false;
    }

    // Magic
    file.write("\x93NUMPY", 6);
    // Version 1.0
    file.write("\x01\x00", 2);
    // Header length (uint16 LE)
    uint16_t hdrLen = static_cast<uint16_t>(hdrStr.size());
    file.write(reinterpret_cast<const char *>(&hdrLen), 2);
    // Header
    file.write(hdrStr.data(), static_cast<qint64>(hdrStr.size()));
    // Data
    file.write(reinterpret_cast<const char *>(volume.data()),
               static_cast<qint64>(volume.size() * sizeof(float)));

    file.close();
    qInfo() << "Volume saved:" << path
            << QString("[%1 x %2 x %3]").arg(depth).arg(imageH).arg(imageW);
    return true;
}
