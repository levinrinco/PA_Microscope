#include "DataPipeline.h"
#include "FileWriter.h"
#include "ScanController.h"
#include "SpectrumFifo.h"
#include "FpgaSerial.h"
#include "ColorMap.h"
#include "DepthEncode.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <chrono>
#include <future>
#include <iostream>
#include <cstring>
#include <algorithm>

// This executable deliberately does not link the hardware SDK. Supply a finite
// DMA block through the real SpectrumFifo interface without opening any device.
namespace {
std::vector<int16_t> samples;
std::atomic<bool> stopped{false};
std::atomic<bool> released{false};
int64_t consumed = 0;
int64_t initialPos = 0;
int32_t notifyOverride = 0;
std::function<void()> waitHook;
bool check(bool ok, const char *message) {
    if (!ok) std::cerr << "FAIL: " << message << '\n';
    return ok;
}
template<class Predicate> bool awaitEvent(Predicate ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready() && std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
QString sessionPath(const QString &root) {
    const auto dirs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    return dirs.size() == 1 ? root + '/' + dirs.front() : QString();
}

/// Stand in for the GUI thread's stop of a finished single-frame scan.
///
/// A real scan ends like this: the pipeline sees the frame complete, asks the
/// controller to stop, and ScanController calls stopAcquisition() — which is
/// what unblocks the acquisition thread stuck in waitForData(). These offline
/// tests drive DataPipeline::run() directly, so unless something plays that
/// part the acquisition thread never returns and run() never reaches the join.
///
/// The stop is issued only after the host queue is empty, i.e. after the
/// processing loop has taken every sample. Stopping earlier would let the
/// acquisition loop publish m_acquisitionDone while A-lines were still queued,
/// and the processing loop would leave a frame half-processed.
void stopFlow(SpectrumFifo &fifo, DataPipeline &pipeline, int64_t expectAlines)
{
    // Wait until the processing loop has consumed the whole simulated block,
    // then stop the card — exactly the order a real single-frame scan uses (the
    // pipeline asks the controller to stop once the frame is complete).
    //
    // Neither "the host queue is empty" nor "the counter stopped moving" is good
    // enough: a bulk dequeue removes samples from the queue before it processes
    // them, and the fake producer hands the block out in batches, so both
    // signals can fire while half a frame is still in flight. That is how a
    // 6-A-line frame came out as "frames= 0 alines= 3": the card was stopped
    // before the second batch was ever read, and the frame-complete outputs
    // (the C-scan PNG and the depth PNG) were never written at all.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (pipeline.processedAlines() < expectAlines
           && std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    fifo.stopAcquisition();
}
}

SpectrumFifo::SpectrumFifo() {
    m_pData = samples.data();
    m_bufferSize = static_cast<int64_t>(samples.size() * sizeof(int16_t));
    m_notifySize = notifyOverride ? notifyOverride : static_cast<int32_t>(m_bufferSize);
}
SpectrumFifo::~SpectrumFifo() = default;
bool SpectrumFifo::setupFifoMode(int32_t, uint64_t, int64_t, int64_t, int64_t) { return true; }
bool SpectrumFifo::redefineTransfer() { return true; }
bool SpectrumFifo::startAcquisition() { stopped = false; released = false; consumed = 0; return true; }
bool SpectrumFifo::stopAcquisition() { stopped = true; return true; }
uint32_t SpectrumFifo::waitForData() {
    if (waitHook) waitHook();
    if (stopped) return ERR_ABORT;
    if (!released) return ERR_OK;
    // Everything offered so far has been taken: a real card reports "nothing
    // new" here (ERR_TIMEOUT) and waits to be stopped, which is what the
    // acquisition loop expects. Only stopAcquisition() ends the block — the
    // offline tests supply that stop through stopFlow(), the way the GUI thread
    // does during a real single-frame scan.
    QThread::msleep(1);
    return ERR_TIMEOUT;
}
FifoStatus SpectrumFifo::readFifoStatus() const {
    FifoStatus s;
    s.availUserBytes = m_bufferSize - consumed;
    s.availUserPos = (initialPos + consumed) % m_bufferSize;
    return s;
}
bool SpectrumFifo::readFifoStatus(FifoStatus &s) const { s = readFifoStatus(); return true; }
bool SpectrumFifo::releaseData(int64_t bytes) {
    consumed += bytes; released = consumed == m_bufferSize; return true;
}
QString SpectrumFifo::lastErrorText() const { return "fake device"; }
QString SpectrumFifo::translateError(uint32_t) { return "fake device error"; }

bool testTrajectoryDatOnly(const QString &root) {
    samples = {-1, 1, -2, 2, -3, 3, -4, 4};
    SpectrumFifo fifo;
    fifo.startAcquisition();
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(/*continuous=*/false)) return false;
    std::vector<MapInfo> map(2, {0, 1, 2, 3, 1, 0, 0, 0});
    DataPipeline pipeline(&fifo);
    pipeline.setMapInfo(&map);
    pipeline.setGridSize(2, 2);
    pipeline.setSegmentSize(4);
    pipeline.setLiveDepth(4);
    pipeline.setLiveWindowOffset(0);
    pipeline.setSaveContext(&writer, &map, 2, 2);
    pipeline.setSaveFormats(false, false, /*depthPng=*/false, false, false, false, true, false);
    pipeline.setSaveDataFormat(DataFormat::Float16);
    pipeline.run();
    QFile file(sessionPath(root) + "/frame_00000.dat");
    if (!check(file.open(QIODevice::ReadOnly), "DAT-only pipeline creates frame")) return false;
    const auto bytes = file.readAll();
    const uint16_t expected[] = {0xbc00, 0x3c00, 0xc000, 0x4000,
                                 0xc200, 0x4200, 0xc400, 0x4400};
    if (!check(bytes.size() == sizeof(expected), "trajectory half payload size")) return false;
    for (size_t i = 0; i < 8; ++i) {
        uint16_t actual;
        std::memcpy(&actual, bytes.constData() + i * 2, 2);
        if (!check(actual == expected[i], "trajectory half preserves signed waveform")) return false;
    }
    return check(QDir(sessionPath(root)).entryList(QDir::Files).size() == 1,
                 "DAT-only does not create other outputs");
}

bool testRasterOrientation(const QString &root) {
    samples.clear();
    for (int16_t peak = 1; peak <= 6; ++peak)
        samples.insert(samples.end(), {static_cast<int16_t>(-peak), peak});
    SpectrumFifo fifo;
    fifo.startAcquisition();
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(/*continuous=*/false)) return false;
    DataPipeline pipeline(&fifo);
    pipeline.setScanMode(ScanMode::Raster);
    pipeline.setRasterSize(2, 3);
    pipeline.setSegmentSize(2);
    pipeline.setLiveDepth(2);
    pipeline.setLiveWindowOffset(0);
    pipeline.setSaveContext(&writer, nullptr, 0, 0);
    pipeline.setSaveFormats(false, true, /*depthPng=*/false, false, false, false, false, false);
    pipeline.setColorMap(1);
    pipeline.setColorParams(0, false);
    pipeline.run();
    QImage image(sessionPath(root) + "/cscan_00000.png");
    if (!check(image.width() == 3 && image.height() == 2,
               "raster PNG width=slow, height=fast")) return false;
    for (int slow = 0; slow < 3; ++slow)
        for (int fast = 0; fast < 2; ++fast)
            if (!check(qRed(image.pixel(slow, fast)) ==
                       mapToIndex(slow * 2 + fast + 1, 6, 0, false),
                       "raster PNG preserves every fast/slow pixel")) return false;
    // Single-shot: no per-frame folder level, and the session says so.
    if (!check(QFileInfo(sessionPath(root)).fileName().startsWith("Single_"),
               "single-shot session name carries the Single_ prefix")) return false;
    if (!check(QDir(sessionPath(root)).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
               "single-shot session creates no frame folders")) return false;
    return true;
}

// 0: manual stop, 1: acquisition error, 2: automatic single-frame completion.
bool testStopWhileSaving(const QString &root, int finishMode) {
    samples = {-1, 1};
    SpectrumFifo fifo;
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(/*continuous=*/false)) return false;
    FpgaSerial fpga;
    ScanController controller;
    DataPipeline pipeline(&fifo);
    pipeline.setScanMode(ScanMode::Raster);
    pipeline.setRasterSize(1, 1);
    pipeline.setSegmentSize(2);
    pipeline.setLiveDepth(2);
    pipeline.setLiveWindowOffset(0);
    pipeline.setContinuous(finishMode != 2);
    pipeline.setSaveContext(&writer, nullptr, 0, 0);
    pipeline.setSaveFormats(true, false, /*depthPng=*/false, false, false, false, false, false);
    controller.setSpectrumFifo(&fifo);
    controller.setFpgaSerial(&fpga);
    controller.setPipeline(&pipeline);
    QThread worker;
    pipeline.moveToThread(&worker);
    QObject::connect(&worker, &QThread::started, &pipeline, &DataPipeline::run);
    QObject::connect(&pipeline, &DataPipeline::scanFinished, &worker, &QThread::quit,
                     Qt::DirectConnection);
    QObject::connect(&pipeline, &DataPipeline::scanFinished,
                     &controller, &ScanController::onPipelineScanComplete);
    QObject::connect(&pipeline, &DataPipeline::acquisitionComplete,
                     &controller, &ScanController::onAcquisitionComplete);
    QObject::connect(&controller, &ScanController::pipelineStopRequested,
                     &pipeline, &DataPipeline::requestStop, Qt::DirectConnection);
    int starts = 0;
    QObject::connect(&controller, &ScanController::startWorkerThread, &controller,
                     [&] { ++starts; worker.start(); });
    std::promise<void> releaseSave;
    const auto gate = releaseSave.get_future().share();
    std::atomic<bool> saving{false};
    QObject::connect(&writer, &FileWriter::itemSaved, &writer, [&](int) {
        saving = true;
        gate.wait(); // deterministic slow disk: do not return the saved task yet
    }, Qt::DirectConnection);
    controller.startScan();
    bool ok = check(awaitEvent([&] { return saving.load(); }), "save worker reached barrier");
    if (finishMode == 1) controller.onPipelineError("simulated failure during save");
    else if (finishMode == 0) controller.stopScan();
    else ok = check(awaitEvent([&] { return controller.state() == ScanState::Stopping; }),
                    "single frame waits for writer before completion") && ok;
    ok = check(controller.state() == ScanState::Stopping && controller.isRunning(),
               "stop/error keeps scan busy while saving") && ok;
    controller.startScan();
    ok = check(starts == 1, "restart rejected while previous save is active") && ok;
    releaseSave.set_value();
    ok = check(awaitEvent([&] { return !controller.isRunning() && !worker.isRunning(); }),
               "stop completes after writer drains") && ok;
    const auto expectedState = finishMode == 1 ? ScanState::Error
                             : finishMode == 2 ? ScanState::Done : ScanState::Idle;
    ok = check(controller.state() == expectedState,
               "drain preserves requested terminal state") && ok;
    pipeline.requestStop();
    fifo.stopAcquisition();
    worker.quit();
    worker.wait();
    return ok;
}

bool testDrain(const QString &root, int mode) {
    samples.clear();
    // Forty complete frames plus one incomplete A-line (mode 3).
    for (int16_t i = 1; i <= 40; ++i)
        samples.insert(samples.end(), {static_cast<int16_t>(-i), i});
    if (mode == 3) samples.push_back(123);
    if (mode == 4) samples.insert(samples.end(), {-123, 123});
    initialPos = mode == 3 ? 6 : 0;
    if (initialPos) std::rotate(samples.begin(), samples.end() - initialPos / 2, samples.end());
    notifyOverride = mode == 1 || mode == 3 ? 4096 : 0;
    SpectrumFifo fifo;
    fifo.startAcquisition();
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(/*continuous=*/true)) return false;
    const int alinesPerFrame = mode == 4 ? 2 : 1;
    const int expectedFrames = 40 / alinesPerFrame;
    std::vector<MapInfo> map(alinesPerFrame, {0, 1, 2, 3, 1, 0, 0, 0});
    DataPipeline pipeline(&fifo);
    pipeline.setMapInfo(&map);
    pipeline.setGridSize(2, 2);
    pipeline.setSegmentSize(2);
    pipeline.setLiveDepth(2);
    pipeline.setLiveWindowOffset(0);
    pipeline.setContinuous(true);
    pipeline.setSaveContext(&writer, &map, 2, 2);
    pipeline.setSaveFormats(false, false, /*depthPng=*/false, false, false, false, true, false);
    writer.setRowsPerFrame(alinesPerFrame);
    pipeline.setSaveDataFormat(DataFormat::Float32);
    std::promise<void> releaseSave, releaseDma;
    auto saveGate = releaseSave.get_future().share();
    auto dmaGate = releaseDma.get_future().share();
    std::atomic<int> published{0}, saved{0}, errors{0};
    std::atomic<bool> inWait{false};
    QObject::connect(&pipeline, &DataPipeline::errorOccurred, &pipeline,
                     [&](const QString &) { ++errors; }, Qt::DirectConnection);
    QObject::connect(&pipeline, &DataPipeline::mipFrameCompleted, &pipeline,
                     [&](int slot, int) { ++published; pipeline.releaseMipSlot(slot); },
                     Qt::DirectConnection);
    QObject::connect(&writer, &FileWriter::itemSaved, &writer,
                     [&](int) { ++saved; saveGate.wait(); }, Qt::DirectConnection);
    if (mode == 2) waitHook = [&] { inWait = true; dmaGate.wait(); };
    if (mode == 1 || mode == 3 || mode == 4) pipeline.requestStop();
    auto run = std::async(std::launch::async, [&] { pipeline.run(); });
    bool ok = true;
    if (mode == 0)
        ok = check(awaitEvent([&] { return published >= 3; }), "frame pool exhausted before stop");
    if (mode == 2)
        ok = check(awaitEvent([&] { return inWait.load(); }), "producer blocked before final enqueue");
    pipeline.requestStop();
    if (mode == 2)
        ok = check(run.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
                   "stop must wait for producer EOF") && ok;
    releaseDma.set_value();
    releaseSave.set_value();
    run.get();
    ok = check(saved == expectedFrames && errors == 0, "stop saves every complete frame") && ok;
    for (int i = 0; i < expectedFrames; ++i) {
        QFile file(sessionPath(root) + QString("/frame_%1.dat").arg(i, 5, 10, QChar('0')));
        if (!file.open(QIODevice::ReadOnly)) { ok = false; continue; }
        std::vector<float> values(alinesPerFrame * 2);
        const size_t bytes = values.size() * sizeof(float);
        const qint64 read = file.read(reinterpret_cast<char *>(values.data()), bytes);
        ok = check(read == static_cast<qint64>(bytes) && file.atEnd(), "complete frame payload size") && ok;
        for (int a = 0; a < alinesPerFrame; ++a) {
            const int value = i * alinesPerFrame + a + 1;
            ok = check(values[a * 2] == -value && values[a * 2 + 1] == value,
                       "saved frames retain exact ordered samples") && ok;
        }
    }
    // Trajectory keeps one self-contained file per frame in the session folder,
    // so the session must hold exactly one file per frame and no frame folders.
    ok = check(QDir(sessionPath(root)).entryList(QDir::Files).size() == expectedFrames,
               "no incomplete or duplicate frame saved") && ok;
    ok = check(QDir(sessionPath(root)).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
               "trajectory creates no per-frame folders") && ok;
    ok = check(QFileInfo(sessionPath(root)).fileName().startsWith("Continuous_"),
               "continuous session name carries the Continuous_ prefix") && ok;
    waitHook = {}; initialPos = 0; notifyOverride = 0;
    return ok;
}

// Raster continuous session: raw B-scan rows go into a per-frame folder and are
// numbered locally from 0 inside it (layout B). Uses a single-frame geometry so
// the assertions do not depend on how many batches the fake DMA hands over.
bool testRasterPerFrameFolders(const QString &root)
{
    // 1 frame = 1 row of 1 A-line (nFast=1, nSlow=1) = 1 segment of 2 samples.
    samples = {-7, 7};

    SpectrumFifo fifo;
    fifo.startAcquisition();
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(/*continuous=*/true)) return false;
    writer.setRowsPerFrame(1);            // nSlow = 1 row per frame

    DataPipeline pipeline(&fifo);
    pipeline.setScanMode(ScanMode::Raster);
    pipeline.setRasterSize(1, 1);         // nFast = 1, nSlow = 1
    pipeline.setSegmentSize(2);
    pipeline.setLiveDepth(2);
    pipeline.setLiveWindowOffset(0);
    pipeline.setContinuous(true);
    pipeline.setSaveContext(&writer, nullptr, 0, 0);
    pipeline.setSaveFormats(/*rasterDat=*/true, false, /*depthPng=*/false, false, false, false, false, false);
    pipeline.setSaveDataFormat(DataFormat::Float32);
    pipeline.requestStop();
    auto run = std::async(std::launch::async, [&] { pipeline.run(); });
    if (!check(run.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
               "raster continuous run terminates after stop"))
        return false;
    run.get();

    const QString session = sessionPath(root);
    if (!check(QFileInfo(session).fileName().startsWith("Continuous_"),
               "continuous raster session carries the Continuous_ prefix"))
        return false;

    // The raw row must be inside frame_00000/ and named 00000.dat (row number
    // local to the frame), not loose in the session root.
    const QString folder = session + "/frame_00000";
    if (!check(QDir(folder).exists(), "raw rows live in a per-frame folder"))
        return false;
    if (!check(QDir(session).entryList(QDir::Files).isEmpty(),
               "raster continuous keeps no loose .dat in the session root"))
        return false;
    const QStringList rows = QDir(folder).entryList(QDir::Files, QDir::Name);
    if (!check(rows.size() == 1 && rows.at(0) == "00000.dat",
               "rows are numbered locally from zero inside the frame folder"))
        return false;

    QFile file(folder + "/00000.dat");
    if (!check(file.open(QIODevice::ReadOnly), "row file opens")) return false;
    std::vector<float> values(2);
    const qint64 read = file.read(reinterpret_cast<char *>(values.data()), 2 * sizeof(float));
    if (!check(read == static_cast<qint64>(2 * sizeof(float)) && file.atEnd(),
               "row payload size"))
        return false;
    if (!check(values[0] == -7.0f && values[1] == 7.0f,
               "row keeps the exact signed samples"))
        return false;
    return true;
}

// Depth encoding end to end. The fake DMA gives every A-line a known envelope
// peak, so the pixels of depth_00000.png can be predicted from the shared
// encoder — this is what proves the pipeline tracks the peak at the right
// depth AND that the raster plane is laid out in the same orientation as the
// intensity C-scan.
bool testRasterDepthPng(const QString &root) {
    samples.clear();
    // Six A-lines (fast=2, slow=3, depth=4), one complete raster frame.  Each
    // line has zero mean, so DC removal preserves the deliberately-positioned
    // envelope peak.  The second row also has lower peak intensity, which
    // verifies that the saved depth image receives the matching intensity
    // plane for modulation.
    // The fake caches samples.data() in its constructor, so the vector must be
    // filled BEFORE the fifo is created — filling it afterwards can reallocate
    // and leave the fake pointing at freed memory.
    const int16_t rows[6][4] = {
        { 5, -1, -1, -3}, {-1,  5, -1, -3},
        {-1, -3,  5, -1}, {-1, -3, -1,  5},
        { 4, -2, -1, -1}, {-2, -1, -1,  4}
    };
    for (const auto &row : rows)
        for (int16_t value : row) samples.push_back(value);
    SpectrumFifo fifo;
    fifo.startAcquisition();
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(/*continuous=*/false)) return false;

    DataPipeline pipeline(&fifo);
    pipeline.setScanMode(ScanMode::Raster);
    pipeline.setRasterSize(2, 3);
    pipeline.setSegmentSize(4);
    pipeline.setLiveDepth(4);
    pipeline.setLiveWindowOffset(0);
    pipeline.setSaveContext(&writer, nullptr, 0, 0);
    pipeline.setSaveFormats(false, false, /*depthPng=*/true,
                            false, false, false, false, false);
    pipeline.setDepthEncodeEnabled(true);
    pipeline.setSaveDepthMapsEnabled(true);
    DepthConfig depthConfig;
    depthConfig.maxIndex = 3;
    depthConfig.minIntensityFraction = 0.01;
    depthConfig.mode = DepthDisplayMode::Modulated;
    pipeline.setDepthConfig(depthConfig);
    std::thread stopper(stopFlow, std::ref(fifo), std::ref(pipeline), /*expectAlines=*/6);
    pipeline.run();
    stopper.join();

    const QString path = sessionPath(root) + "/depth_00000.png";
    QImage depthImage(path);
    if (!check(!depthImage.isNull(), "raster depth PNG exists")) return false;
    if (!check(depthImage.width() == 3 && depthImage.height() == 2,
               "raster depth PNG is nSlow wide and nFast high")) return false;

    // The pipeline stores raster planes as [fast][slow], matching the PNG's
    // rows and columns. Peaks are 5,5,4 in each row; depths are 0,2,0 and
    // 1,3,3 respectively.
    const float intensities[6] = {5.0f, 5.0f, 4.0f, 5.0f, 5.0f, 4.0f};
    const float depthPlane[6]  = {0.0f, 2.0f, 0.0f, 1.0f, 3.0f, 3.0f};
    QImage expected;
    encodeDepthImage(depthPlane, intensities, 3, 2, /*windowDepth=*/4, depthConfig, expected);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            const QRgb want = expected.pixel(x, y);
            const QRgb got = depthImage.pixel(x, y);
            if (!check(qRed(got) == qRed(want) && qGreen(got) == qGreen(want)
                       && qBlue(got) == qBlue(want),
                       "raster depth PNG matches the encoder for every pixel"))
                return false;
            if (!check(qAlpha(got) == 255, "depth PNG is opaque")) return false;
        }
    }
    return true;
}

// Trajectory depth: the depth map is built per pixel from the accumulator, and
// the saved PNG has to carry it. The failure this guards against is subtle: an
// earlier implementation reduced the accumulator AFTER handing the MIP slot
// over, so the depth plane was built from an already-cleared slot and came out
// all zeros -- a completely black image that still "saved successfully".
//
// The window is deliberately longer than one sample. With depth == 1, DC removal
// leaves raw = x - mean(x) = 0 for every A-line, so every peak depth is 0 by
// construction and the test would prove nothing.
bool testTrajectoryDepthPng(const QString &root) {
    samples.clear();
    // Three A-lines of depth 4. DC removal subtracts the window mean, so the
    // envelope peak of each line sits at a different index:
    //   {-1,-3,-1, 5} mean 0 -> envelope {1,3,1,5}  peak 5 at index 3
    //   {-1,-3, 5,-1} mean 0 -> envelope {1,3,5,1}  peak 5 at index 2
    //   {-1, 5,-1,-3} mean 0 -> envelope {1,5,1,3}  peak 5 at index 1
    const int16_t rows[3][4] = {{-1, -3, -1, 5}, {-1, -3, 5, -1}, {-1, 5, -1, -3}};
    for (auto &row : rows)
        for (int16_t v : row) samples.push_back(v);
    // Fill first, then construct: see the note in testRasterDepthPng.
    SpectrumFifo fifo;
    fifo.startAcquisition();
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(/*continuous=*/false)) return false;

    // One A-line per grid pixel, so each output pixel has exactly one known
    // peak depth and the mapping to colours is unambiguous.
    std::vector<MapInfo> map(3);
    map[0] = {0, 0, 0, 0, 1.0f, 0.0f, 0.0f, 0.0f};
    map[1] = {1, 1, 1, 1, 1.0f, 0.0f, 0.0f, 0.0f};
    map[2] = {2, 2, 2, 2, 1.0f, 0.0f, 0.0f, 0.0f};

    DataPipeline pipeline(&fifo);
    pipeline.setMapInfo(&map);
    pipeline.setGridSize(3, 1);
    pipeline.setSegmentSize(4);
    pipeline.setLiveDepth(4);
    pipeline.setLiveWindowOffset(0);
    pipeline.setSaveContext(&writer, &map, 3, 1);
    pipeline.setSaveFormats(false, false, /*depthPng=*/false,
                            false, false, false, false, /*mipDepth=*/true);
    pipeline.setDepthEncodeEnabled(true);
    pipeline.setSaveDepthMapsEnabled(true);
    DepthConfig depthConfig;
    depthConfig.maxIndex = 3;              // depths 1..3 span the whole ramp
    depthConfig.minIntensityFraction = 0.01;
    depthConfig.mode = DepthDisplayMode::Pure;
    pipeline.setDepthConfig(depthConfig);
    std::thread stopper(stopFlow, std::ref(fifo), std::ref(pipeline), /*expectAlines=*/3);
    pipeline.run();
    stopper.join();

    const QString path = sessionPath(root) + "/depth_00000.png";
    QImage depthImage(path);
    if (!check(!depthImage.isNull(), "trajectory depth PNG exists")) return false;
    if (!check(depthImage.width() == 3 && depthImage.height() == 1,
               "trajectory depth PNG uses the output grid")) return false;

    // Each pixel must be painted with the palette entry of its own peak depth.
    // A plane built from a cleared slot would give three identical black pixels,
    // so both "not black" and "the three differ" are real checks.
    uint32_t lut[256];
    depthLut(depthConfig.colorMap, lut);
    const int expectedKeys[3] = {3, 2, 1};
    for (int x = 0; x < 3; ++x) {
        const int key = expectedKeys[x] * 255 / depthConfig.maxIndex;
        const uint32_t want = lut[key] & 0x00FFFFFFu;
        const QRgb got = depthImage.pixel(x, 0);
        const uint32_t gotBits = (static_cast<uint32_t>(qRed(got)))
                                 | (static_cast<uint32_t>(qGreen(got)) << 8)
                                 | (static_cast<uint32_t>(qBlue(got)) << 16);
        if (!check(gotBits == want,
                   "trajectory depth pixel carries its A-line peak depth"))
            return false;
    }
    if (!check(depthImage.pixel(0, 0) != depthImage.pixel(2, 0),
               "different peak depths give different colors"))
        return false;
    return true;
}

// Saving is disabled and every point maps to the SAME C-scan pixel, so only
// acquisition-order waveform extraction can produce the expected slices.
bool testTrajectoryBscan(int first, int last, int points, int frames,
                         int offeredPoints, bool changeRange)
{
    samples.clear();
    for (int i = 0; i < offeredPoints; ++i) {
        const int16_t peak = static_cast<int16_t>(1 + i % 100);
        samples.insert(samples.end(), {30000, static_cast<int16_t>(100 + peak),
            static_cast<int16_t>(100 - 2 * peak), static_cast<int16_t>(100 + 4 * peak),
            -30000, 12345});
    }
    SpectrumFifo fifo;
    fifo.startAcquisition();
    std::vector<MapInfo> map(points, {0, 0, 0, 0, 1, 0, 0, 0});
    DataPipeline pipeline(&fifo);
    pipeline.setMapInfo(&map);
    pipeline.setGridSize(2, 2);
    pipeline.setSegmentSize(6);
    pipeline.setLiveWindowOffset(1);
    pipeline.setLiveDepth(3);
    pipeline.setContinuous(true);
    pipeline.setTrajectoryBscanRange(first, last);
    pipeline.setUpdateInterval(1);
    pipeline.setSaveFormats(false, false, false, false, false, false, false, false);
    struct Slice { int frame, first, last, depth; std::vector<double> raw; };
    std::vector<Slice> slices;
    QObject::connect(&pipeline, &DataPipeline::trajectoryBscanReady, &pipeline,
        [&](int frame, int begin, int end, const std::vector<double> &raw, int depth) {
            slices.push_back({frame, begin, end, depth, raw});
        }, Qt::DirectConnection);
    QObject::connect(&pipeline, &DataPipeline::mipFrameCompleted, &pipeline,
        [&](int slot, int) { pipeline.releaseMipSlot(slot); }, Qt::DirectConnection);
    QObject::connect(&pipeline, &DataPipeline::scanProgress, &pipeline,
        [&](int current, int) {
            if (changeRange && current == 1) pipeline.setTrajectoryBscanRange(points, points);
        }, Qt::DirectConnection);
    pipeline.requestStop(); // Drain the finite simulated DMA block.
    pipeline.run();
    if (!check(static_cast<int>(slices.size()) == frames,
               "one trajectory slice per completed selected interval")) return false;
    for (int f = 0; f < frames; ++f) {
        const auto &slice = slices[f];
        const int begin = (changeRange && f > 0) ? points : std::clamp(first, 1, points);
        const int end = (changeRange && f > 0) ? points : std::clamp(last, begin, points);
        if (!check(slice.frame == f && slice.first == begin && slice.last == end &&
                   slice.depth == 3 && slice.raw.size() == static_cast<size_t>(end - begin + 1) * 3,
                   "trajectory interval is inclusive and frame-local")) return false;
        for (int p = begin; p <= end; ++p) {
            const double peak = 1 + (f * points + p - 1) % 100;
            const size_t offset = static_cast<size_t>(p - begin) * 3;
            if (!check(slice.raw[offset] == 0 && slice.raw[offset + 1] == -3 * peak &&
                       slice.raw[offset + 2] == 3 * peak,
                       "B-scan preserves signed window samples and acquisition order before gridding"))
                return false;
        }
    }
    return true;
}

bool testPolledDisplay(const QString &root, bool raster, bool partial)
{
    constexpr int points = 12, depth = 4, frames = 16, nFast = 3, nSlow = 4;
    const int total = frames * points + (partial ? nFast : 0);
    samples.clear();
    for (int i = 0; i < total; ++i) {
        const int16_t peak = 1 + i % 100;
        samples.insert(samples.end(), {0, static_cast<int16_t>(-peak), 0, peak});
    }
    notifyOverride = 64;
    SpectrumFifo fifo;
    fifo.startAcquisition();
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    writer.setRowsPerFrame(nSlow);
    if (!writer.beginSession(true)) return false;
    const std::vector<MapInfo> map(points, {0, 1, 2, 3, 1, 0, 0, 0});
    DataPipeline pipeline(&fifo);
    pipeline.setScanMode(raster ? ScanMode::Raster : ScanMode::Trajectory);
    pipeline.setRasterSize(nFast, nSlow);
    pipeline.setMapInfo(&map);
    pipeline.setGridSize(2, 2);
    pipeline.setSegmentSize(depth);
    pipeline.setLiveWindowOffset(0);
    pipeline.setLiveDepth(depth);
    pipeline.setContinuous(true);
    pipeline.setDisplayPollingEnabled(true);
    pipeline.setDepthEncodeEnabled(true);
    pipeline.setTrajectoryBscanRange(2, 3);
    pipeline.setSaveContext(&writer, &map, 2, 2);
    pipeline.setSaveFormats(raster, false, false, false, false, false, !raster, false);
    pipeline.setSaveDataFormat(DataFormat::Float32);
    int oldDisplayEvents = 0;
    QObject::connect(&pipeline, &DataPipeline::bscanDataReady, &pipeline,
        [&](int, const std::vector<double> &, int, int) { ++oldDisplayEvents; }, Qt::DirectConnection);
    QObject::connect(&pipeline, &DataPipeline::trajectoryBscanReady, &pipeline,
        [&](int, int, int, const std::vector<double> &, int) { ++oldDisplayEvents; }, Qt::DirectConnection);
    QObject::connect(&pipeline, &DataPipeline::mipFrameCompleted, &pipeline,
        [&](int, int) { ++oldDisplayEvents; }, Qt::DirectConnection);
    pipeline.requestStop();
    pipeline.run(); // Deliberately never poll until every frame has been saved.
    const auto update = pipeline.takeDisplayUpdate();
    if (!check(oldDisplayEvents == 0 && update.bscan && update.cscan && update.progress && update.fifo,
               "polling retains latest state without emitting queued large payloads")) return false;
    const auto &b = *update.bscan;
    const int bFrame = partial ? frames : frames - 1;
    if (!check(b.frame == bFrame && b.trajectory == !raster && b.depth == depth,
               "latest B-scan survives a stalled GUI and stop")) return false;
    const int firstPoint = raster ? (partial ? 0 : (nSlow - 1) * nFast) : 1;
    const int displayedPoints = raster ? nFast : 2;
    if (!check(b.samples.size() == static_cast<size_t>(displayedPoints * depth),
               "latest B-scan has bounded one-slice payload")) return false;
    for (int p = 0; p < displayedPoints; ++p) {
        const double peak = 1 + (bFrame * points + firstPoint + p) % 100;
        if (!check(b.samples[p * depth + 1] == (raster ? peak : -peak) &&
                   b.samples[p * depth + 3] == peak,
                   "polling preserves raw trajectory signs and raster envelope")) return false;
    }
    const auto &c = *update.cscan;
    if (raster) {
        if (!check(c.frame == bFrame && c.width == nSlow && c.height == nFast,
                   "raster snapshot belongs to latest frame")) return false;
        for (int slow = 0; slow < nSlow; ++slow)
            for (int fast = 0; fast < nFast; ++fast) {
                const bool present = !partial || slow == 0;
                const float value = present ? 1 + (bFrame * points + slow * nFast + fast) % 100 : 0;
                const size_t index = fast * nSlow + slow;
                if (!check(c.intensity[index] == value && c.depth[index] == (present ? 1 : 0),
                           "skipped raster repaints lose no rows and never mix old frame columns")) return false;
            }
    } else {
        float sum = 0;
        for (int p = 0; p < points; ++p) sum += 1 + ((frames - 1) * points + p) % 100;
        if (!check(c.frame == frames - 1 && c.intensity[0] == sum / points && c.depth[0] == 1,
                   "trajectory latest completed intensity and depth remain paired")) return false;
    }
    const auto empty = pipeline.takeDisplayUpdate();
    if (!check(!empty.bscan && !empty.cscan && !empty.progress && !empty.fifo,
               "poll consumes dirty state without replaying old frames")) return false;
    const int saved = raster ? frames * nSlow + (partial ? 1 : 0) : frames;
    if (!check(writer.savedItemCount() == saved, "display coalescing never drops save tasks")) return false;
    const auto session = sessionPath(root);
    for (int item = 0; item < saved; ++item) {
        const QString name = raster
            ? QString("/frame_%1/%2.dat").arg(item / nSlow, 5, 10, QChar('0')).arg(item % nSlow, 5, 10, QChar('0'))
            : QString("/frame_%1.dat").arg(item, 5, 10, QChar('0'));
        QFile file(session + name);
        if (!file.open(QIODevice::ReadOnly)) return false;
        const auto bytes = file.readAll();
        const int alines = raster ? nFast : points;
        if (!check(bytes.size() == static_cast<qsizetype>(alines * depth * sizeof(float)),
                   "every saved payload is complete")) return false;
        for (int p = 0; p < alines; ++p) {
            const float peak = 1 + (item * alines + p) % 100;
            const float expected[] = {0, -peak, 0, peak};
            if (!check(std::memcmp(bytes.constData() + p * depth * sizeof(float), expected, sizeof(expected)) == 0,
                       "all stored frames retain signed raw samples despite skipped display")) return false;
        }
    }
    notifyOverride = 0;
    return true;
}

bool testMailboxConcurrent()
{
    DisplayMailbox mailbox;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        std::vector<double> wave(1024);
        MipAccum slot;
        slot.sum.resize(4);
        slot.weight.assign(4, 1);
        std::vector<float> depth(4);
        for (int frame = 0; frame < 2000; ++frame) {
            std::fill(wave.begin(), wave.end(), -frame);
            mailbox.publishBscan(frame, 1, 256, 4, wave);
            std::fill(slot.sum.begin(), slot.sum.end(), frame);
            std::fill(depth.begin(), depth.end(), frame + 1);
            mailbox.publishMip(frame, 2, 2, slot, &depth);
        }
        done.store(true, std::memory_order_release);
    });
    bool ok = true;
    int lastBscan = -1, lastMip = -1;
    do {
        const auto update = mailbox.take();
        if (update.bscan) {
            const auto &b = *update.bscan;
            ok = ok && b.frame >= lastBscan && b.samples.size() == 1024;
            for (double value : b.samples) ok = ok && value == -b.frame;
            lastBscan = b.frame;
        }
        if (update.cscan) {
            const auto &c = *update.cscan;
            ok = ok && c.frame >= lastMip;
            for (size_t i = 0; i < c.intensity.size(); ++i)
                ok = ok && c.intensity[i] == c.frame && c.depth[i] == c.frame + 1;
            lastMip = c.frame;
        }
        std::this_thread::yield();
    } while (!done.load(std::memory_order_acquire) || lastBscan != 1999 || lastMip != 1999);
    producer.join();
    return check(ok, "concurrent display polling never tears or mixes payloads");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    initColorMap();
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    if (!testMailboxConcurrent()) return 1;
    for (bool raster : {false, true})
        for (bool partial : {false, true})
            if (!testPolledDisplay(root.path() + QString("/poll-%1-%2").arg(raster).arg(partial), raster, partial))
                return 1;
    bool ok = testTrajectoryDatOnly(root.path() + "/trajectory");
    ok = testTrajectoryBscan(2, 4, 5, 2, 10, true) && ok;
    ok = testTrajectoryBscan(1, 5, 5, 1, 5, false) && ok;
    ok = testTrajectoryBscan(99, 120, 5, 1, 5, false) && ok;
    ok = testTrajectoryBscan(0, -1, 5, 1, 5, false) && ok;
    ok = testTrajectoryBscan(2, 4, 5, 0, 3, false) && ok;
    ok = testTrajectoryBscan(1, 33124, 33124, 1, 33124, false) && ok;
    ok = testRasterOrientation(root.path() + "/raster") && ok;
    // Trajectory depth runs: it covers the crash that enabling depth encoding
    // used to cause in trajectory mode (an empty m_depthRow indexed out of
    // bounds) and the all-zero depth plane that made the saved PNG black.
    ok = testTrajectoryDepthPng(root.path() + "/trajdepth") && ok;
    ok = testRasterDepthPng(root.path() + "/rasterdepth") && ok;
    ok = testRasterPerFrameFolders(root.path() + "/rastercont") && ok;
    ok = testStopWhileSaving(root.path() + "/stop", 0) && ok;
    ok = testStopWhileSaving(root.path() + "/error", 1) && ok;
    ok = testStopWhileSaving(root.path() + "/complete", 2) && ok;
    for (int mode = 0; mode < 5; ++mode)
        ok = testDrain(root.path() + QString("/drain%1").arg(mode), mode) && ok;
    return ok ? 0 : 1;
}
