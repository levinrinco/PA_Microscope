#include "FileWriter.h"
#include "ColorMap.h"
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

namespace {

// ---------------------------------------------------------------------------
// IEEE-754 binary16 (half) conversion — for float16 .dat / .npy output
// ---------------------------------------------------------------------------
inline uint16_t floatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t  exp  = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t       mant = bits & 0x7FFFFFu;

    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);       // underflow -> 0
        mant |= 0x800000u;                                       // implicit 1
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t half = mant >> shift;
        // round to nearest even
        const uint32_t rem = mant & ((1u << shift) - 1u);
        if (rem > (1u << (shift - 1u)) ||
            (rem == (1u << (shift - 1u)) && (half & 1u)))
            ++half;
        return static_cast<uint16_t>(sign | half);
    }
    if (exp >= 31)                                               // overflow -> inf
        return static_cast<uint16_t>(sign | 0x7C00u);

    uint32_t half = static_cast<uint32_t>(exp) << 10 | (mant >> 13);
    if (mant & 0x1000u) {                                        // round to even
        if ((mant & 0x1FFFu) > 0x1000u || ((mant & 0x1FFFu) == 0x1000u && (half & 1u)))
            ++half;
    }
    return static_cast<uint16_t>(sign | half);
}

// Fixed-size conversion scratch (at most 512 KiB), independent of frame size.
// Preserve double precision for Float64 and the existing float -> half rounding.
template<class Output, class Input, class Convert>
bool writeConverted(QFile &file, const Input *src, size_t count, Convert convert)
{
    constexpr size_t chunkSamples = 65536;
    std::vector<Output> buffer(std::min(count, chunkSamples));
    for (size_t offset = 0; offset < count; offset += chunkSamples) {
        const size_t n = std::min(chunkSamples, count - offset);
        for (size_t i = 0; i < n; ++i) buffer[i] = convert(src[offset + i]);
        const qint64 bytes = static_cast<qint64>(n * sizeof(Output));
        if (file.write(reinterpret_cast<const char *>(buffer.data()), bytes) != bytes)
            return false;
    }
    return true;
}

template<class Input>
bool writeSamples(QFile &file, const Input *src, size_t count, DataFormat fmt)
{
    switch (fmt) {
    case DataFormat::Float64:
        return writeConverted<double>(file, src, count,
            [](Input v) { return static_cast<double>(v); });
    case DataFormat::Float16:
        return writeConverted<uint16_t>(file, src, count,
            [](Input v) { return floatToHalf(static_cast<float>(v)); });
    case DataFormat::Float32:
    default:
        return writeConverted<float>(file, src, count,
            [](Input v) { return static_cast<float>(v); });
    }
}

const char *npyDescr(DataFormat fmt)
{
    switch (fmt) {
    case DataFormat::Float64: return "<f8";
    case DataFormat::Float16: return "<f2";
    case DataFormat::Float32:
    default:                  return "<f4";
    }
}

// ---------------------------------------------------------------------------
// Otsu automatic thresholding on an en-face peak map (vessel/background split)
// ---------------------------------------------------------------------------
float otsuThreshold(const std::vector<float> &values)
{
    float maxV = 0.0f;
    for (float v : values) maxV = std::max(maxV, v);
    if (maxV <= 1e-12f) return 0.0f;

    std::array<int, 256> hist{};
    const size_t total = values.size();
    for (float v : values) {
        int bin = static_cast<int>(v / maxV * 255.0f);
        bin = std::max(0, std::min(255, bin));
        ++hist[bin];
    }

    double sum = 0.0;
    for (int t = 0; t < 256; ++t) sum += static_cast<double>(t) * hist[t];

    double sumB = 0.0;
    int wB = 0;
    double bestVar = -1.0;
    int bestT = 0;
    for (int t = 0; t < 256; ++t) {
        wB += hist[t];
        if (wB == 0) continue;
        const int wF = static_cast<int>(total) - wB;
        if (wF == 0) break;
        sumB += static_cast<double>(t) * hist[t];
        const double mB = sumB / wB;
        const double mF = (sum - sumB) / wF;
        const double var = static_cast<double>(wB) * wF * (mB - mF) * (mB - mF);
        if (var > bestVar) { bestVar = var; bestT = t; }
    }
    return (static_cast<float>(bestT) + 0.5f) / 255.0f * maxV;
}

} // namespace

FileWriter::FileWriter(QObject *parent)
    : QObject(parent) {}
FileWriter::~FileWriter() = default;

void FileWriter::setSavePath(const QString &dir) { m_savePath = dir; }
void FileWriter::setEnabled(bool enabled)        { m_enabled = enabled; }

// ---------------------------------------------------------------------------
// Output layout
//
//   Single_<timestamp>/          one frame, written straight into the session
//   Continuous_<timestamp>/      one sub-directory per frame for the bulk
//       frame_00000/             raw data (raster: one .dat per B-scan row,
//           00000.dat            trajectory: one .dat for the whole frame)
//           ...
//       cscan_00000.png          single-file-per-frame products stay at the
//       volume_00000.npy         top level, as requested
//       mip_00000.png
//       cloud_00000.pcd
//
// Only the raw data gets a frame sub-directory (it is the one output that is a
// directory-full of files). This mirrors the agreed layout B.
// ---------------------------------------------------------------------------

// Directory holding one frame's raster raw rows. Empty when outputs live
// directly in the session (single-shot, or saving disabled). Only raster needs
// this: a trajectory frame's raw data is a single file, not a directory-full.
QString FileWriter::frameOutputDir(int frameIdx) const
{
    if (m_sessionDir.isEmpty() || !m_continuous) return QString();
    return m_sessionDir + QString("/frame_%1").arg(frameIdx, 5, 10, QChar('0'));
}

bool FileWriter::beginSession(bool continuous)
{
    m_savedItems.store(0, std::memory_order_relaxed);
    // Saving is disabled for this scan, so there is nothing to store: never
    // touch the filesystem. Guarding here instead of trusting every caller to
    // skip the call is what keeps a stray session folder from appearing.
    if (!m_enabled) return true;

    if (m_savePath.trimmed().isEmpty()) {
        emit errorOccurred("Save path is empty");
        return false;
    }
    QDir baseDir(m_savePath);
    if (!baseDir.exists() && !baseDir.mkpath(".")) {
        emit errorOccurred(QString("Cannot create directory: %1").arg(m_savePath));
        return false;
    }

    // The name states up front whether this session holds one frame or many.
    const QString base = (continuous ? QStringLiteral("Continuous_")
                                     : QStringLiteral("Single_"))
                         + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");

    QString ts = base;
    if (baseDir.exists(ts)) {
        int suffix = 1;
        do {
            ts = QString("%1-%2").arg(base).arg(suffix++, 3, 10, QChar('0'));
        } while (baseDir.exists(ts));
    }
    m_sessionDir = ts;
    m_continuous = continuous;
    if (!baseDir.mkdir(ts)) {
        m_sessionDir.clear();
        m_continuous = false;
        emit errorOccurred(QString("Cannot create session directory: %1/%2")
                           .arg(m_savePath, ts));
        return false;
    }
    qInfo() << "FileWriter: session" << baseDir.absoluteFilePath(ts)
            << (continuous ? "(continuous, per-frame raw folders)"
                           : "(single-shot)");
    return true;
}

bool FileWriter::saveRasterBscan(const std::vector<double> &rawBscan,
                                 int bscanIndex, int depth, int nFast,
                                 DataFormat fmt)
{
    if (!m_enabled) return true;
    if (m_sessionDir.isEmpty() || depth <= 0 || nFast <= 0 ||
        rawBscan.size() < static_cast<size_t>(depth) * nFast)
        return false;

    // bscanIndex is a GLOBAL row number across the whole session (frame n starts
    // at n*nSlow). In continuous mode each frame's rows go into their own
    // folder and are numbered locally again from 0, so the files of one frame
    // read 00000.dat .. 00999.dat instead of 01000.dat .. 01999.dat.
    const int rowsPerFrame = std::max(1, m_rowsPerFrame);
    QString relPath;
    if (m_continuous) {
        const int frameIdx = bscanIndex / rowsPerFrame;
        const int rowIdx   = bscanIndex % rowsPerFrame;
        relPath = QString("%1/%2.dat")
                      .arg(frameOutputDir(frameIdx))
                      .arg(rowIdx, 5, 10, QChar('0'));
    } else {
        relPath = QString("%1/%2.dat").arg(m_sessionDir).arg(bscanIndex);
    }

    const QString path = QDir(m_savePath).absoluteFilePath(relPath);
    if (m_continuous && !QDir().mkpath(QFileInfo(path).absolutePath())) {
        emit errorOccurred(QString("Cannot create frame directory: %1")
                               .arg(QFileInfo(path).absolutePath()));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        emit errorOccurred(QString("Cannot create: %1").arg(path));
        return false;
    }

    // Already a double source; do not duplicate the whole row for conversion.
    const size_t count = static_cast<size_t>(depth) * nFast;
    const qint64 before = file.pos();
    const bool written = writeSamples(file, rawBscan.data(), count, fmt);
    const qint64 bytes = file.pos() - before;

    const size_t expected = count * (fmt == DataFormat::Float64 ? 8
                                   : fmt == DataFormat::Float16 ? 2 : 4);
    if (!written || bytes != static_cast<qint64>(expected) || !file.flush()) {
        emit errorOccurred(QString("Incomplete B-scan write: %1").arg(path));
        return false;
    }
    m_savedItems.fetch_add(1, std::memory_order_relaxed);
    emit itemSaved(bscanIndex);
    return true;
}

bool FileWriter::saveRasterCscanPng(const std::vector<float> &mip,
                                    int imageW, int imageH, int frameIdx,
                                    int colormap, int colorMapping, bool colorClipping)
{
    if (!m_enabled) return true;
    if (m_sessionDir.isEmpty() || imageW <= 0 || imageH <= 0 ||
        static_cast<int>(mip.size()) < imageW * imageH)
        return false;

    const QString path = QDir(m_savePath).absoluteFilePath(
        m_sessionDir + QString("/cscan_%1.png").arg(frameIdx, 5, 10, QChar('0')));
    return saveMipPng(path, mip, imageW, imageH, colormap,
                      colorMapping, colorClipping);
}

bool FileWriter::saveDepthPng(const std::vector<float> &depthMap,
                              int imageW, int imageH, int windowDepth,
                              int frameIdx,
                              const DepthConfig &config,
                              const std::vector<float> *intensity)
{
    if (!m_enabled) return true;
    if (m_sessionDir.isEmpty() || imageW <= 0 || imageH <= 0 ||
        static_cast<int>(depthMap.size()) < imageW * imageH)
        return false;

    const QString path = QDir(m_savePath).absoluteFilePath(
        m_sessionDir + QString("/depth_%1.png").arg(frameIdx, 5, 10, QChar('0')));
    return saveDepthImagePng(path, depthMap, intensity, imageW, imageH,
                             windowDepth, config);
}

bool FileWriter::saveTrajectoryFrame(const int16_t *rawAlines,
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
                                     double scanRadiusM)
{
    if (!m_enabled) return true;
    if (m_sessionDir.isEmpty() || !rawAlines || alineCount <= 0 || depth <= 0 ||
        imageW <= 0 || imageH <= 0 ||
        static_cast<int>(mapInfo.size()) < alineCount) {
        emit errorOccurred("Invalid trajectory save request");
        return false;
    }

    QDir baseDir(m_savePath);
    const QString dir = baseDir.absoluteFilePath(m_sessionDir);

    // Raw DAT is already in its final layout and must not pay for a potentially
    // multi-GB gridded volume. Build the grid only for formats that consume it.
    std::vector<float> volume, mip;
    const bool needsGrid = saveVolume || saveMip || savePcd || depthMap;
    if (needsGrid)
        gridVolume(rawAlines, alineCount, depth, mapInfo, imageW, imageH, volume, mip,
                   saveVolume || savePcd);

    bool ok = true;
    if (saveVolume) {
        const QString volPath = dir + QString("/volume_%1.npy").arg(frameIdx, 5, 10, QChar('0'));
        ok = saveVolumeNpy(volPath, volume, imageW, imageH, depth, fmt) && ok;
    }
    if (saveMip) {
        const QString mipPath = dir + QString("/mip_%1.png").arg(frameIdx, 5, 10, QChar('0'));
        ok = saveMipPng(mipPath, mip, imageW, imageH, colormap,
                        colorMapping, colorClipping) && ok;
    }
    if (depthMap) {
        // The depth map arrives already gridded (the pipeline built it from
        // the same bilinear interpolation), and `mip` is the matching
        // intensity plane, so brightness modulation stays consistent with the
        // live view without recomputing the volume.
        const QString depthPath = dir +
            QString("/depth_%1.png").arg(frameIdx, 5, 10, QChar('0'));
        ok = saveDepthImagePng(depthPath, *depthMap, &mip, imageW, imageH,
                               windowDepth, depthConfig) && ok;
    }
    if (savePcd) {
        const QString pcdPath = dir + QString("/cloud_%1.pcd").arg(frameIdx, 5, 10, QChar('0'));
        ok = saveTrajectoryPcd(pcdPath, volume, imageW, imageH, depth,
                               scanRadiusM, segMode, segThreshold) && ok;
    }
    if (saveRawDat) {
        // Raw per-frame waveform: all A-lines of this frame, row-major
        // [aline][depth] — i.e. a "volume of B-scans" for the whole frame.
        // Trajectory needs no frame sub-folder: this is already exactly one
        // self-contained file per frame, unlike raster's one file per B-scan
        // row. Nesting it would only add a redundant directory level.
        const QString datPath = dir +
            QString("/frame_%1.dat").arg(frameIdx, 5, 10, QChar('0'));
        const size_t count = static_cast<size_t>(alineCount) * depth;
        QFile datFile(datPath);
        if (datFile.open(QIODevice::WriteOnly)) {
            const bool written = writeSamples(datFile, rawAlines, count, fmt);
            const size_t expected = count * (fmt == DataFormat::Float64 ? 8
                                           : fmt == DataFormat::Float16 ? 2 : 4);
            if (!written || datFile.pos() != static_cast<qint64>(expected) || !datFile.flush()) {
                emit errorOccurred(QString("Incomplete raw waveform write: %1").arg(datPath));
                ok = false;
            }
            datFile.close();
            qInfo() << "Raw waveform saved:" << datPath
                    << QString("[%1 x %2]").arg(alineCount).arg(depth);
        } else {
            emit errorOccurred(QString("Cannot create: %1").arg(datPath));
            ok = false;
        }
    }
    if (ok) {
        m_savedItems.fetch_add(1, std::memory_order_relaxed);
        emit itemSaved(frameIdx);
    }
    return ok;
}

// ===== 3D Gridding (multi-threaded) =====

void FileWriter::gridVolume(const int16_t *rawAlines,
                            int alineCount, int depth,
                            const std::vector<MapInfo> &mapInfo,
                            int imageW, int imageH,
                            std::vector<float> &volumeOut,
                            std::vector<float> &mipOut, bool keepVolume)
{
    const size_t gridSize = static_cast<size_t>(imageW) * imageH;
    const size_t totalVoxels = gridSize * depth;
    if (keepVolume) volumeOut.assign(totalVoxels, 0.0f);
    else volumeOut.clear();
    mipOut.assign(gridSize, 0.0f);

    // Bilinear weights do not depend on depth. Keeping one 2-D weight plane
    // instead of a full weight volume saves hundreds of MB at typical sizes.
    std::vector<float> weights(gridSize, 0.0f);
    std::vector<double> dcMeans(alineCount, 0.0);
    for (int i = 0; i < alineCount; ++i) {
        const int16_t *raw = rawAlines + static_cast<size_t>(i) * depth;
        for (int d = 0; d < depth; ++d) dcMeans[i] += raw[d];
        dcMeans[i] /= depth;

        const MapInfo &m = mapInfo[i];
        weights[m.idx00] += m.w00;
        weights[m.idx01] += m.w01;
        weights[m.idx10] += m.w10;
        weights[m.idx11] += m.w11;
    }

    unsigned int threadCount = std::thread::hardware_concurrency();
    threadCount = std::max(1u, std::min(8u, threadCount));
    threadCount = std::min(threadCount, static_cast<unsigned int>(depth));

    if (!keepVolume) {
        // PNG needs max_d(grid(envelope[d])), NOT grid(max_d(envelope[d])).
        // Keep the original accumulation order at each depth, but retain only
        // a layer and its running maximum per worker. Limit scratch to about
        // 32 MiB (or two planes for a grid larger than that).
        const size_t workersByMemory = std::max<size_t>(1,
            (32u * 1024u * 1024u) / (2 * gridSize * sizeof(float)));
        threadCount = static_cast<unsigned int>(std::min<size_t>(threadCount, workersByMemory));
        std::vector<std::vector<float>> layers(threadCount, std::vector<float>(gridSize));
        std::vector<std::vector<float>> maxima(threadCount, std::vector<float>(gridSize));
        std::vector<std::thread> workers;
        for (unsigned int t = 0; t < threadCount; ++t) {
            workers.emplace_back([&, t] {
                auto &layer = layers[t];
                auto &maximum = maxima[t];
                const int d0 = depth * static_cast<int>(t) / static_cast<int>(threadCount);
                const int d1 = depth * static_cast<int>(t + 1) / static_cast<int>(threadCount);
                for (int d = d0; d < d1; ++d) {
                    std::fill(layer.begin(), layer.end(), 0.0f);
                    for (int i = 0; i < alineCount; ++i) {
                        const float value = std::abs(static_cast<float>(
                            static_cast<double>(rawAlines[static_cast<size_t>(i) * depth + d]) - dcMeans[i]));
                        const auto &m = mapInfo[i];
                        layer[m.idx00] += value * m.w00;
                        layer[m.idx01] += value * m.w01;
                        layer[m.idx10] += value * m.w10;
                        layer[m.idx11] += value * m.w11;
                    }
                    for (size_t pixel = 0; pixel < gridSize; ++pixel) {
                        if (weights[pixel] > 0.0f) layer[pixel] /= weights[pixel];
                        maximum[pixel] = std::max(maximum[pixel], layer[pixel]);
                    }
                }
            });
        }
        for (auto &worker : workers) worker.join();
        for (const auto &maximum : maxima)
            for (size_t pixel = 0; pixel < gridSize; ++pixel)
                mipOut[pixel] = std::max(mipOut[pixel], maximum[pixel]);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (unsigned int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        const int d0 = depth * static_cast<int>(threadIndex) / static_cast<int>(threadCount);
        const int d1 = depth * static_cast<int>(threadIndex + 1) / static_cast<int>(threadCount);
        threads.emplace_back([&, d0, d1] {
            for (int i = 0; i < alineCount; ++i) {
                const int16_t *raw = rawAlines + static_cast<size_t>(i) * depth;
                const MapInfo &m = mapInfo[i];
                for (int d = d0; d < d1; ++d) {
                    const float value = std::abs(
                        static_cast<float>(static_cast<double>(raw[d]) - dcMeans[i]));
                    volumeOut[static_cast<size_t>(m.idx00) * depth + d] += value * m.w00;
                    volumeOut[static_cast<size_t>(m.idx01) * depth + d] += value * m.w01;
                    volumeOut[static_cast<size_t>(m.idx10) * depth + d] += value * m.w10;
                    volumeOut[static_cast<size_t>(m.idx11) * depth + d] += value * m.w11;
                }
            }

            for (size_t pixel = 0; pixel < gridSize; ++pixel) {
                const float weight = weights[pixel];
                if (weight <= 0.0f) continue;
                const size_t base = pixel * depth;
                for (int d = d0; d < d1; ++d)
                    volumeOut[base + d] /= weight;
            }
        });
    }
    for (auto &thread : threads) thread.join();

    for (size_t pixel = 0; pixel < gridSize; ++pixel) {
        const size_t base = pixel * depth;
        float maximum = 0.0f;
        for (int d = 0; d < depth; ++d)
            maximum = std::max(maximum, volumeOut[base + d]);
        mipOut[pixel] = maximum;
    }
}

// ===== MIP → PNG =====

bool FileWriter::saveMipPng(const QString &path, const std::vector<float> &mip,
                            int imageW, int imageH, int colormap,
                            int colorMapping, bool colorClipping)
{
    if (mip.empty()) return false;

    float maxVal = 1e-9f;
    for (float v : mip) if (v > maxVal) maxVal = v;
    const uint32_t *lut = colorLut(colormap);

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
                    int idx = mapToIndex(mip[rowBase + x], maxVal,
                                         colorMapping, colorClipping);
                    uint32_t c = lut[idx];
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

// ===== Depth map → PNG =====

bool FileWriter::saveDepthImagePng(const QString &path,
                                   const std::vector<float> &depthMap,
                                   const std::vector<float> *intensity,
                                   int imageW, int imageH, int windowDepth,
                                   const DepthConfig &config)
{
    if (imageW <= 0 || imageH <= 0 ||
        static_cast<int>(depthMap.size()) < imageW * imageH)
        return false;

    // Use the shared encoder rather than a private palette: the saved PNG and
    // the on-screen C-scan have to agree pixel for pixel, and one
    // implementation is the only way to keep that true.
    QImage img;
    encodeDepthImage(depthMap.data(),
                     (intensity && static_cast<int>(intensity->size()) >= imageW * imageH)
                         ? intensity->data() : nullptr,
                     imageW, imageH, windowDepth, config, img);

    if (!img.save(path, "PNG")) {
        emit errorOccurred(QString("Cannot save depth PNG: %1").arg(path));
        return false;
    }
    qInfo() << "Depth map saved:" << path;
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
                               int imageW, int imageH, int depth, DataFormat fmt)
{
    const size_t expectedValues = static_cast<size_t>(imageW) * imageH * depth;
    if (imageW <= 0 || imageH <= 0 || depth <= 0 || volume.size() < expectedValues) {
        emit errorOccurred(QString("Invalid volume dimensions for: %1").arg(path));
        return false;
    }
    // Build header dict string
    // shape = (depth, height, width) — C order
    std::ostringstream hdr;
    hdr << "{'descr': '" << npyDescr(fmt) << "', 'fortran_order': False, "
        << "'shape': (" << depth << ", " << imageH << ", " << imageW << ")}";

    std::string hdrStr = hdr.str();
    // NPY v1.0 requires the header to end in a newline and the full preamble
    // to be aligned. Put padding before (not after) that final newline.
    const size_t padding = (64 - ((10 + hdrStr.size() + 1) % 64)) % 64;
    hdrStr.append(padding, ' ');
    hdrStr += '\n';

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
    // Internally gridding is pixel-major [H,W,depth] for efficient scatter.
    // The public file contract is C-order [depth,H,W], so transpose on write.
    const size_t gridSize = static_cast<size_t>(imageW) * imageH;
    std::vector<float> plane(gridSize);
    for (int d = 0; d < depth; ++d) {
        for (size_t pixel = 0; pixel < gridSize; ++pixel)
            plane[pixel] = volume[pixel * depth + d];
        if (!writeSamples(file, plane.data(), gridSize, fmt) ||
            file.error() != QFileDevice::NoError) {
            emit errorOccurred(QString("Incomplete NPY write: %1").arg(path));
            return false;
        }
    }

    file.close();
    qInfo() << "Volume saved:" << path
            << QString("[%1 x %2 x %3]").arg(depth).arg(imageH).arg(imageW);
    return true;
}

// ===== Volume → PCD (peak point cloud with background filtering) =====

bool FileWriter::saveTrajectoryPcd(const QString &path, const std::vector<float> &volume,
                                   int imageW, int imageH, int depth,
                                   double scanRadiusM,
                                   PcdSegMode segMode, float segThreshold)
{
    if (imageW <= 0 || imageH <= 0 || depth <= 0 ||
        volume.size() < static_cast<size_t>(imageW) * imageH * depth)
        return false;

    const size_t gridSize = static_cast<size_t>(imageW) * imageH;

    // 1) En-face peak map + peak depth index per pixel column.
    std::vector<float> peakMap(gridSize, 0.0f);
    std::vector<int>  peakDepth(gridSize, 0);
    float globalMax = 0.0f;
    for (size_t pixel = 0; pixel < gridSize; ++pixel) {
        const size_t base = pixel * depth;
        float best = 0.0f;
        int bestD = 0;
        for (int d = 0; d < depth; ++d) {
            const float v = volume[base + d];
            if (v > best) { best = v; bestD = d; }
        }
        peakMap[pixel] = best;
        peakDepth[pixel] = bestD;
        globalMax = std::max(globalMax, best);
    }

    // 2) Background segmentation: keep only signal pixels (vessels).
    float threshold = 0.0f;
    switch (segMode) {
    case PcdSegMode::Manual:
        threshold = globalMax * segThreshold;
        break;
    case PcdSegMode::Otsu:
        threshold = otsuThreshold(peakMap);
        break;
    case PcdSegMode::Off:
    default:
        threshold = 0.0f;
        break;
    }
    // Never drop genuinely strong signal even when noise is zero.
    if (threshold <= 1e-12f && segMode != PcdSegMode::Off)
        threshold = globalMax * 1e-3f;

    // 3) Convert kept pixel columns to points via the scan grid.
    //    x,y in meters; z = peak depth in samples; intensity = peak value.
    struct Point { float x, y, z, i; };
    std::vector<Point> points;
    points.reserve(gridSize);
    const float fxScale = (imageW > 1) ? 2.0f / (imageW - 1) : 0.0f;
    const float fyScale = (imageH > 1) ? 2.0f / (imageH - 1) : 0.0f;
    for (size_t pixel = 0; pixel < gridSize; ++pixel) {
        if (peakMap[pixel] < threshold) continue;
        const int px = static_cast<int>(pixel % imageW);
        const int py = static_cast<int>(pixel / imageW);
        Point pt;
        pt.x = (px * fxScale - 1.0f) * static_cast<float>(scanRadiusM);
        pt.y = (py * fyScale - 1.0f) * static_cast<float>(scanRadiusM);
        pt.z = static_cast<float>(peakDepth[pixel]);
        pt.i = peakMap[pixel];
        points.push_back(pt);
    }

    if (points.empty()) {
        qWarning() << "PCD: no points after segmentation" << path;
        return false;
    }

    // 4) ASCII PCD (PCL v0.7).
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        emit errorOccurred(QString("Cannot create: %1").arg(path));
        return false;
    }
    QTextStream ts(&file);
    ts << "# .PCD v0.7 - Point Cloud Data file format\n"
       << "VERSION 0.7\n"
       << "FIELDS x y z intensity\n"
       << "SIZE 4 4 4 4\n"
       << "TYPE F F F F\n"
       << "COUNT 1 1 1 1\n"
       << "WIDTH " << points.size() << "\n"
       << "HEIGHT 1\n"
       << "VIEWPOINT 0 0 0 1 0 0 0\n"
       << "POINTS " << points.size() << "\n"
       << "DATA ascii\n";
    for (const Point &p : points)
        ts << p.x << ' ' << p.y << ' ' << p.z << ' ' << p.i << '\n';

    qInfo() << "PCD saved:" << path << points.size() << "points"
            << "(threshold" << threshold << ")";
    return true;
}
