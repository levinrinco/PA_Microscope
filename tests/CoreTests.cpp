#include "ColorMap.h"
#include "ConfigManager.h"
#include "DepthEncode.h"
#include "FileWriter.h"
#include "TrajectoryLoader.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>
#include <qfloat16.h>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool nearlyEqual(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

bool testTrajectoryLoader(const QString &root)
{
    const QString path = root + "/trajectory.csv";
    QFile csv(path);
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    csv.write("index,x_m,y_m\n0,-1,0\n1,0,0\n2,1,0\n");
    csv.close();

    TrajectoryLoader loader;
    std::vector<TrajectoryPoint> points;
    std::vector<MapInfo> mappings;
    double radius = 0.0;
    if (!require(loader.load(path, 5, 3, false, points, mappings, radius),
                 "trajectory CSV should load")) return false;
    if (!require(points.size() == 3 && mappings.size() == 3,
                 "trajectory point count")) return false;
    if (!require(std::fabs(radius - 1.05) < 1e-6, "trajectory radius")) return false;
    for (const MapInfo &mapping : mappings) {
        const float sum = mapping.w00 + mapping.w01 + mapping.w10 + mapping.w11;
        if (!require(std::fabs(sum - 1.0f) < 1e-5f, "bilinear weights sum to one"))
            return false;
        if (!require(mapping.idx00 >= 0 && mapping.idx11 < 15,
                     "bilinear indices are in range")) return false;
    }
    return true;
}

bool testFileWriter(const QString &root)
{
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!require(writer.beginSession(false), "session creation")) return false;

    const std::vector<double> raster = {1, 2, 3, 4, 5, 6, 7, 8};
    if (!require(writer.saveRasterBscan(raster, 0, 4, 2, DataFormat::Float64),
                 "raster float64 save")) return false;
    if (!require(writer.saveRasterBscan(raster, 1, 4, 2, DataFormat::Float32),
                 "raster float32 save")) return false;
    if (!require(writer.saveRasterBscan(raster, 2, 4, 2, DataFormat::Float16),
                 "raster float16 save")) return false;

    // A Float64 request must store genuine 64-bit precision. Routing the data
    // through float on the way to disk used to cap the mantissa at 24 bits and
    // silently produce float32 values in an 8-byte container. 1/3 is not
    // representable in float, so it exposes that: narrowing to float changes the
    // stored bit pattern.
    {
        const double third = 1.0 / 3.0;
        const std::vector<double> precise = {third, third, third, third,
                                             third, third, third, third};
        if (!require(writer.saveRasterBscan(precise, 3, 8, 1, DataFormat::Float64),
                     "raster high-precision float64 save")) return false;
        QFile f(QDir(root).absoluteFilePath(
            QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).front()
            + "/3.dat"));
        if (!require(f.open(QIODevice::ReadOnly), "float64 row opens")) return false;
        const QByteArray bytes = f.readAll();
        if (!require(bytes.size() == 8 * sizeof(double), "float64 row size")) return false;
        double first = 0.0;
        std::memcpy(&first, bytes.constData(), sizeof(double));
        const double asFloat32 = static_cast<double>(static_cast<float>(third));
        if (!require(first == third,
                     "float64 keeps full double precision (not narrowed to float32)"))
            return false;
        if (!require(first != asFloat32,
                     "float64 payload differs from its float32 rounding"))
            return false;
    }

    // Raster final C-scan PNG
    const std::vector<float> cscan = {0.0f, 1.0f, 2.0f, 3.0f};
    if (!require(writer.saveRasterCscanPng(cscan, 2, 2, 0, 0, 0, true),
                 "raster c-scan png save")) return false;

    const int16_t raw[] = {0, 2, 4, 6, 0, 4, 8, 12};
    std::vector<MapInfo> mapping(2);
    mapping[0] = {0, 0, 0, 0, 1.0f, 0.0f, 0.0f, 0.0f};
    mapping[1] = {1, 1, 1, 1, 1.0f, 0.0f, 0.0f, 0.0f};
    if (!require(writer.saveTrajectoryFrame(raw, 2, 4, mapping, 2, 1, 0,
                                            DataFormat::Float32, 0, 0, true,
                                            true, true, true, false,
                                            /*depthMap=*/nullptr, DepthConfig(),
                                            /*windowDepth=*/4,
                                            PcdSegMode::Off, 0.25f, 1.05),
                 "trajectory frame save (npy+mip+pcd)")) return false;

    QDir base(root);
    const QStringList sessions = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (!require(sessions.size() == 1, "one session directory")) return false;
    QDir session(base.absoluteFilePath(sessions.front()));

    const std::vector<double> signedRaster = {-1.0, 1.0, -2.5, 2.5, -0.0, 0.0};
    if (!writer.saveRasterBscan(signedRaster, 3, 3, 2, DataFormat::Float16)) return false;
    QFile halfFile(session.absoluteFilePath("3.dat"));
    if (!halfFile.open(QIODevice::ReadOnly)) return false;
    const auto halfBytes = halfFile.readAll();
    const uint16_t halfExpected[] = {0xbc00, 0x3c00, 0xc100, 0x4100, 0x8000, 0};
    if (!require(halfBytes.size() == sizeof(halfExpected), "signed half payload size")) return false;
    for (size_t i = 0; i < 6; ++i) {
        uint16_t actual;
        std::memcpy(&actual, halfBytes.constData() + i * 2, 2);
        if (!require(actual == halfExpected[i], "raster half preserves signs and zero")) return false;
    }

    // --- raster .dat sizes per format ---
    for (const auto &entry : session.entryList({"0.dat", "1.dat", "2.dat"}, QDir::Files)) {
        QFile f(session.absoluteFilePath(entry));
        const qint64 want = (entry == "0.dat") ? 64 : (entry == "1.dat") ? 32 : 16;
        if (!require(f.size() == want, qPrintable("raster dat size " + entry)))
            return false;
    }

    // --- raster C-scan PNG magic ---
    QFile png(session.absoluteFilePath("cscan_00000.png"));
    if (!require(png.open(QIODevice::ReadOnly) && png.read(4) == QByteArray("\x89PNG", 4),
                 "cscan png magic")) return false;

    QFile npy(session.absoluteFilePath("volume_00000.npy"));
    if (!require(npy.open(QIODevice::ReadOnly), "NPY should exist")) return false;
    const QByteArray bytes = npy.readAll();
    npy.close();
    if (!require(bytes.size() > 12, "NPY length")) return false;
    if (!require(bytes.left(6) == QByteArray("\x93NUMPY", 6), "NPY magic")) return false;

    const auto *u = reinterpret_cast<const unsigned char *>(bytes.constData());
    const int headerLength = u[8] | (u[9] << 8);
    const int dataOffset = 10 + headerLength;
    const QByteArray header = bytes.mid(10, headerLength);
    if (!require(header.endsWith('\n'), "NPY header newline")) return false;
    if (!require(header.contains("'descr': '<f4'"), "NPY float32 descr")) return false;
    if (!require(header.contains("'shape': (4, 1, 2)"), "NPY public shape")) return false;
    if (!require(bytes.size() == dataOffset + 8 * static_cast<int>(sizeof(float)),
                 "NPY payload size")) return false;

    const float expected[] = {3, 6, 1, 2, 1, 2, 3, 6};
    for (int i = 0; i < 8; ++i) {
        float actual = 0.0f;
        std::memcpy(&actual, bytes.constData() + dataOffset + i * sizeof(float), sizeof(float));
        if (!require(nearlyEqual(actual, expected[i]), "NPY depth-major payload"))
            return false;
    }

    // --- PCD point cloud ---
    QFile pcd(session.absoluteFilePath("cloud_00000.pcd"));
    if (!require(pcd.open(QIODevice::ReadOnly | QIODevice::Text), "PCD should exist"))
        return false;
    QTextStream ts(&pcd);
    bool sawFields = false, sawWidth = false, sawPoints = false, sawAscii = false;
    int dataLines = 0;
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.startsWith("FIELDS")) sawFields = line.contains("intensity");
        else if (line.startsWith("WIDTH")) sawWidth = line.contains("WIDTH 2");
        else if (line.startsWith("POINTS")) sawPoints = line.contains("POINTS 2");
        else if (line.startsWith("DATA")) sawAscii = line.contains("ascii");
        else {
            const QStringList tok = line.trimmed().split(' ', Qt::SkipEmptyParts);
            if (tok.size() == 4) ++dataLines;
        }
    }
    pcd.close();
    if (!require(sawFields && sawWidth && sawPoints && sawAscii && dataLines == 2,
                 "PCD structure (2 peak points, off-seg)")) return false;

    // --- NPY float64 round trip ---
    if (!require(writer.saveTrajectoryFrame(raw, 2, 4, mapping, 2, 1, 1,
                                            DataFormat::Float64, 0, 0, true,
                                            true, false, false, false,
                                            /*depthMap=*/nullptr, DepthConfig(),
                                            /*windowDepth=*/4,
                                            PcdSegMode::Off, 0.25f, 1.05),
                 "trajectory float64 frame save")) return false;
    QFile npy64(session.absoluteFilePath("volume_00001.npy"));
    if (!require(npy64.open(QIODevice::ReadOnly), "float64 NPY should exist")) return false;
    const QByteArray bytes64 = npy64.readAll();
    npy64.close();
    const int hdrLen64 = static_cast<unsigned char>(bytes64.at(8))
                         | (static_cast<unsigned char>(bytes64.at(9)) << 8);
    const QByteArray hdr64 = bytes64.mid(10, hdrLen64);
    if (!require(hdr64.contains("'descr': '<f8'"), "NPY float64 descr")) return false;
    if (!require(bytes64.size() == 10 + hdrLen64 + 8 * 8, "float64 NPY payload size"))
        return false;

    return true;
}

bool testConfigPerMode(const QString &root)
{
    const QString path = root + "/test.ini";
    {
        ConfigManager cfg(nullptr, path);

        // Raster profile
        cfg.setActiveMode(ScanMode::Raster);
        ScanParams raster;
        raster.scanMode = ScanMode::Raster;
        raster.nFast = 111; raster.nSlow = 222;
        raster.depth = 333; raster.windowOffset = 44;
        raster.segmentSize = 8192; raster.saveDat = true; raster.savePng = false;
        raster.dataFormat = DataFormat::Float64;
        cfg.saveScanParams(raster);
        cfg.saveSampleRate(300000000);
        cfg.saveChannelRange(0, 2000);

        // Trajectory profile
        cfg.setActiveMode(ScanMode::Trajectory);
        ScanParams traj;
        traj.scanMode = ScanMode::Trajectory;
        traj.imageWidth = 77; traj.imageHeight = 88;
        traj.depth = 444; traj.windowOffset = 55;
        traj.trajectoryBscanStart = 1234; traj.trajectoryBscanEnd = 33124;
        traj.segmentSize = 2048; traj.saveNpy = false; traj.savePcd = true; traj.saveMip = false;
        traj.dataFormat = DataFormat::Float16;
        traj.pcdSegMode = PcdSegMode::Manual; traj.pcdSegThreshold = 0.31f;
        cfg.saveScanParams(traj);
        cfg.saveSampleRate(125000000);

        // Raster must be untouched by trajectory writes
        cfg.setActiveMode(ScanMode::Raster);
        ScanParams r2 = cfg.loadScanParams();
        if (!require(r2.nFast == 111 && r2.depth == 333 && r2.segmentSize == 8192,
                     "raster params isolated")) return false;
        if (!require(r2.saveDat && !r2.savePng, "raster save toggles isolated")) return false;
        if (!require(r2.dataFormat == DataFormat::Float64, "raster data format isolated"))
            return false;
        if (!require(cfg.loadSampleRate() == 300000000, "raster DAQ isolated")) return false;
        if (!require(cfg.loadChannelRange(0) == 2000, "raster channel isolated")) return false;

        // Trajectory must be untouched by raster writes
        cfg.setActiveMode(ScanMode::Trajectory);
        ScanParams t2 = cfg.loadScanParams();
        if (!require(t2.imageWidth == 77 && t2.imageHeight == 88 && t2.depth == 444,
                     "trajectory params isolated")) return false;
        if (!require(t2.segmentSize == 2048, "trajectory segment isolated")) return false;
        if (!require(t2.trajectoryBscanStart == 1234 && t2.trajectoryBscanEnd == 33124,
                     "trajectory B-scan range persists")) return false;
        if (!require(!t2.saveNpy && t2.savePcd && !t2.saveMip,
                     "trajectory save toggles isolated")) return false;
        if (!require(t2.dataFormat == DataFormat::Float16, "trajectory format isolated"))
            return false;
        if (!require(t2.pcdSegMode == PcdSegMode::Manual &&
                     std::fabs(t2.pcdSegThreshold - 0.31f) < 1e-6f,
                     "trajectory PCD seg isolated")) return false;
        if (!require(cfg.loadSampleRate() == 125000000, "trajectory DAQ isolated"))
            return false;
    }
    return true;
}

} // namespace

// Regression: a scan must only create a session folder when the ACTIVE scan
// mode has at least one output format enabled. SaveParams carries the fields of
// both modes at once, so a stale toggle left over from the inactive mode used
// to enable saving (and create a folder) for a scan that stores nothing.
bool testNoSessionWhenNoOutputEnabled(const QString &root)
{
    // --- hasAnyOutput() must consider only the active mode's fields ---
    ScanParams s;
    s.scanMode = ScanMode::Raster;
    s.saveDat = false; s.savePng = false;      // raster: nothing selected
    s.saveNpy = true;  s.saveMip = true;       // stale trajectory toggles
    if (!require(!s.hasAnyOutput(),
                 "raster with no raster output selected has no output"))
        return false;
    if (!require(s.rasterHasOutput() == false && s.trajectoryHasOutput() == true,
                 "per-mode output predicates stay independent"))
        return false;

    s.scanMode = ScanMode::Trajectory;
    s.saveNpy = false; s.savePcd = false; s.saveMip = false; s.saveDatTraj = false;
    s.saveDat = true;                          // stale raster toggle
    if (!require(!s.hasAnyOutput(),
                 "trajectory with no trajectory output selected has no output"))
        return false;

    // Any single format of the active mode is enough to count as saving.
    s.saveMip = true;
    if (!require(s.hasAnyOutput(), "one trajectory output enables saving")) return false;
    s.scanMode = ScanMode::Raster; s.saveDat = true; s.savePng = false;
    if (!require(s.hasAnyOutput(), "one raster output enables saving")) return false;

    // --- beginSession() must not touch the filesystem while disabled ---
    const QString dataDir = root + "/nodata";
    FileWriter writer;
    writer.setEnabled(false);
    writer.setSavePath(dataDir);
    if (!require(writer.beginSession(false), "disabled beginSession reports success"))
        return false;
    if (!require(!QDir(dataDir).exists(),
                 "disabled beginSession creates no directory"))
        return false;

    // --- and must still create one when enabled ---
    writer.setEnabled(true);
    if (!require(writer.beginSession(false), "enabled beginSession succeeds")) return false;
    const QStringList sessions =
        QDir(dataDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (!require(sessions.size() == 1, "enabled beginSession creates a session"))
        return false;

    // --- end-to-end: the raster profile with only stale trajectory toggles ---
    const QString ini = root + "/session.ini";
    {
        ConfigManager cfg(nullptr, ini);
        cfg.setActiveMode(ScanMode::Raster);
        ScanParams r; r.scanMode = ScanMode::Raster;
        r.saveDat = false; r.savePng = false;     // nothing selected for raster
        r.saveNpy = true;  r.saveMip = true;      // stale trajectory toggles
        cfg.saveScanParams(r);
    }
    const QString dir2 = root + "/nodata2";
    {
        ConfigManager cfg(nullptr, ini);
        cfg.setActiveMode(ScanMode::Raster);
        ScanParams p = cfg.loadScanParams();
        p.scanMode = ScanMode::Raster;            // as onScanStart does from the combo
        FileWriter w;
        const bool anySave = p.hasAnyOutput();
        w.setEnabled(anySave);
        w.setSavePath(dir2);
        if (anySave && !w.beginSession(false)) return false;
        if (!require(!anySave, "persisted raster profile reports no output")) return false;
    }
    if (!require(!QDir(dir2).exists(),
                 "raster profile with only stale trajectory toggles creates no folder"))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Depth encoding: peak-position extraction and the depth image renderer.
//
// The renderer is what both the live C-scan and the saved depth_*.png use, so
// these checks pin down the palette, the background gate and the range clamp.
// ---------------------------------------------------------------------------
bool testDepthEncode()
{
    // --- envelope peak position ---
    {
        const double rises[]   = {0.0, 1.0, 5.0, 3.0, 2.0};
        const double falls[]   = {5.0, 3.0, 1.0, 0.0};
        const double flat[]    = {4.0, 4.0, 4.0};
        const double zeros[]   = {0.0, 0.0, 0.0};
        if (!require(alinePeakDepth(rises, 5) == 2, "peak depth takes the maximum"))
            return false;
        if (!require(alinePeakDepth(falls, 4) == 0, "peak depth handles a leading peak"))
            return false;
        // Strictly-greater comparison: the earliest of equal maxima wins, so a
        // flat A-line reports the shallowest sample rather than the deepest.
        if (!require(alinePeakDepth(flat, 3) == 0, "flat envelope -> first sample"))
            return false;
        if (!require(alinePeakDepth(zeros, 3) == 0, "all-zero envelope -> 0"))
            return false;
        if (!require(alinePeakDepth(nullptr, 0) == 0, "empty envelope -> 0"))
            return false;
    }

    // --- display range ---
    {
        DepthConfig config;
        if (!require(effectiveDepthRange(config, 600) == 599,
                     "default range is the full window")) return false;
        config.maxIndex = 200;
        if (!require(effectiveDepthRange(config, 600) == 200,
                     "explicit range wins")) return false;
        config.maxIndex = 0;
        if (!require(effectiveDepthRange(config, 1) == 1,
                     "degenerate depth of 1 sample stays encodable")) return false;
    }

    // --- palette ---
    {
        uint32_t turbo[256], jet[256];
        depthLut(0, turbo);
        depthLut(5, jet);
        if (!require((turbo[0] & 0x00FFFFFFu) != (turbo[255] & 0x00FFFFFFu),
                     "turbo spans a visible range")) return false;
        if (!require((jet[0] & 0x00FFFFFFu) != (jet[255] & 0x00FFFFFFu),
                     "jet spans a visible range")) return false;
        if (!require((turbo[0] & 0x00FFFFFFu) != (jet[0] & 0x00FFFFFFu),
                     "colormap selection changes the palette")) return false;
        // Turbo's midpoint is much lighter than either end (dark red → light →
        // dark red), which is what makes intermediate structures visible.
        auto luminance = [](uint32_t c) {
            return 0.299 * (c & 0xFF) + 0.587 * ((c >> 8) & 0xFF)
                   + 0.114 * ((c >> 16) & 0xFF);
        };
        if (!require(luminance(turbo[128]) > luminance(turbo[0]) + 60.0
                     && luminance(turbo[128]) > luminance(turbo[255]) + 60.0,
                     "turbo peaks in lightness mid-range")) return false;
        if (!require(luminance(turbo[0]) < 60.0 && luminance(turbo[255]) < 60.0,
                     "turbo ends are both dark")) return false;

        // The perceptual maps are pinned to the authors' published tables. Only
        // the endpoints are checked here, but they are exact values: a typo in
        // an anchor table (easy to make by hand) moves them.
        struct PaletteRef { int index; uint32_t first; uint32_t last; const char *name; };
        const PaletteRef refs[] = {
            {1, 0x00540144u, 0x0025E7FDu, "viridis"},
            {2, 0x0087080Du, 0x0021F9F0u, "plasma"},
            {3, 0x00040000u, 0x00BFFDFCu, "magma"},
            {4, 0x00040000u, 0x00A4FFFCu, "inferno"},
        };
        for (const PaletteRef &ref : refs) {
            uint32_t lut[256];
            depthLut(ref.index, lut);
            if (!require((lut[0] & 0x00FFFFFFu) == ref.first,
                         "perceptual palette starts at its published color"))
                return false;
            if (!require((lut[255] & 0x00FFFFFFu) == ref.last,
                         "perceptual palette ends at its published color"))
                return false;
            // Every one of them is designed to get lighter with depth, unlike
            // turbo's dark-to-dark rainbow.
            if (!require(luminance(lut[255]) > luminance(lut[0]) + 60.0,
                         "perceptual palette brightens towards the deepest sample"))
                return false;
        }

        // A plain sweep also has to be light enough to read against the black
        // background, and no two palettes may share an entry point.
        for (int a = 0; a < 7; ++a) {
            uint32_t la[256];
            depthLut(a, la);
            if (!require((la[0] & 0x00FFFFFFu) != 0 || a == 3 || a == 4,
                         "palette start is visible (magma/inferno start at black)"))
                return false;
            for (int b = a + 1; b < 7; ++b) {
                uint32_t lb[256];
                depthLut(b, lb);
                if (!require((la[128] & 0x00FFFFFFu) != (lb[128] & 0x00FFFFFFu),
                             "palettes are visually distinct mid-range"))
                    return false;
            }
        }
    }

    // --- rendering ---
    {
        const float depth[] = {0.0f, 10.0f};
        const float intensity[] = {2.0f, 2.0f};
        DepthConfig config;
        config.maxIndex = 10;
        config.minIntensityFraction = 0.0;   // no gate: test the pure palette
        QImage image;

        config.mode = DepthDisplayMode::Pure;
        encodeDepthImage(depth, intensity, 2, 1, /*windowDepth=*/11, config, image);
        uint32_t turbo[256];
        depthLut(0, turbo);
        if (!require(image.width() == 2 && image.height() == 1, "depth image size"))
            return false;
        // Compare through the raw RGB888 bytes: QImage::pixel() would convert
        // to ARGB32 and hide a channel-order mistake, which is exactly the bug
        // this check exists to catch.
        auto pixelRgb = [](const QImage &img, int x) {
            const uchar *p = img.constScanLine(0) + x * 3;
            return static_cast<uint32_t>(p[0])
                   | (static_cast<uint32_t>(p[1]) << 8)
                   | (static_cast<uint32_t>(p[2]) << 16);
        };
        if (!require(pixelRgb(image, 0) == (turbo[0] & 0x00FFFFFFu),
                     "depth 0 maps to the near end of the palette")) return false;
        if (!require(pixelRgb(image, 1) == (turbo[255] & 0x00FFFFFFu),
                     "max depth maps to the far end of the palette")) return false;

        // Values past the configured range clamp to the last color.
        const float beyond[] = {40.0f, 40.0f};
        encodeDepthImage(beyond, intensity, 2, 1, /*windowDepth=*/11, config, image);
        if (!require(pixelRgb(image, 0) == (turbo[255] & 0x00FFFFFFu),
                     "depth beyond the range clamps")) return false;

        // Modulated: brightness is intensity / frame maximum, so a non-maximal
        // pixel must come out dimmer than the same depth at full intensity.
        // Both pixels are needed: the brightest pixel is always brightness 1,
        // which makes a single-pixel check unable to detect a broken scale.
        config.mode = DepthDisplayMode::Modulated;
        const float uneven[] = {4.0f, 2.0f};   // pixel 1 is at half the maximum
        const float evenMax[] = {4.0f, 4.0f};
        auto channel = [](uint32_t packed, int shift) {
            return static_cast<int>((packed >> shift) & 0xFF);
        };
        const float deep[] = {0.0f, 10.0f};
        encodeDepthImage(deep, evenMax, 2, 1, /*windowDepth=*/11, config, image);
        const uint32_t brightColor = pixelRgb(image, 1);
        encodeDepthImage(deep, uneven, 2, 1, /*windowDepth=*/11, config, image);
        const uint32_t dimColor = pixelRgb(image, 1);
        if (!require(channel(brightColor, 0) == 144 && channel(brightColor, 8) == 12,
                     "full brightness reaches the palette color")) return false;
        const bool halved =
            std::abs(channel(dimColor, 0) - channel(brightColor, 0) / 2) <= 1
            && std::abs(channel(dimColor, 8) - channel(brightColor, 8) / 2) <= 1
            && std::abs(channel(dimColor, 16) - channel(brightColor, 16) / 2) <= 1;
        if (!require(halved, "intensity modulates brightness, not hue")) return false;

        // Gate. Threshold is a fraction of the en-face maximum, so with a 20%
        // gate and a maximum of 2.0 the cutoff sits at 0.4: 0.1 is clearly
        // background, 1.0 clearly signal (values exactly on the cutoff are
        // deliberately not used — the comparison is strict and 0.2f promoted to
        // double lands a hair above 0.2).
        // Both pixels sit at depth 10 on purpose — depth 0 is NOT black in
        // turbo (it is the dark red end), so a gated pixel could not be told
        // apart from a legitimately shallow one if the depths differed.
        config.minIntensityFraction = 0.20;
        const float both[] = {10.0f, 10.0f};
        const float gated[] = {0.1f, 1.0f};
        encodeDepthImage(both, gated, 2, 1, /*windowDepth=*/11, config, image);
        if (!require(pixelRgb(image, 0) == 0, "background gate paints black"))
            return false;
        if (!require(pixelRgb(image, 1) != 0,
                     "background gate keeps the signal pixel")) return false;

        // A frame with no signal at all renders black regardless of the gate.
        const float dark[] = {0.0f, 0.0f};
        encodeDepthImage(both, dark, 2, 1, /*windowDepth=*/11, config, image);
        if (!require(pixelRgb(image, 0) == 0 && pixelRgb(image, 1) == 0,
                     "empty frame renders black")) return false;
    }

    // --- the color scale follows the ACQUISITION WINDOW, not the data ---
    // Two properties matter, and both are easy to lose by deriving the range
    // from an image dimension or from the largest value actually present:
    //
    //   1. the same depth pixel gets different colors under different window
    //      depths (the scale belongs to the acquisition), and
    //   2. a frame whose depths only reach a small value still uses the whole
    //      palette (the scale is NOT auto-scaled to the observed maximum).
    //
    // Without (1) the raster view, the trajectory view and the color bar can
    // silently end up on three different scales; without (2) every frame's
    // colors would drift with its own deepest pixel.
    {
        const float shallow[] = {2.0f, 2.0f};   // both pixels at the same depth
        const float bright[] = {3.0f, 3.0f};
        DepthConfig config;
        config.minIntensityFraction = 0.0;
        config.mode = DepthDisplayMode::Pure;
        QImage narrow, wide;

        auto firstPixel = [](const QImage &img) {
            const uchar *p = img.constScanLine(0);
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                   | (static_cast<uint32_t>(p[2]) << 16);
        };
        uint32_t turbo[256];
        depthLut(0, turbo);

        // windowDepth 11 -> deepest index 10, so depth 2 sits at 20% of the ramp
        encodeDepthImage(shallow, bright, 2, 1, /*windowDepth=*/11, config, narrow);
        // windowDepth 100 -> deepest index 99, so the same depth 2 is near the start
        encodeDepthImage(shallow, bright, 2, 1, /*windowDepth=*/100, config, wide);
        if (!require(firstPixel(narrow) != firstPixel(wide),
                     "the same depth is colored against the window depth"))
            return false;
        if (!require(firstPixel(narrow) == (turbo[51] & 0x00FFFFFFu),
                     "depth 2 of an 11-sample window is 2/10 of the ramp"))
            return false;
        if (!require(firstPixel(wide) == (turbo[5] & 0x00FFFFFFu),
                     "depth 2 of a 100-sample window is 2/99, not the ramp maximum"))
            return false;

        // An explicit range overrides the window entirely.
        config.maxIndex = 2;
        QImage pinned;
        encodeDepthImage(shallow, bright, 2, 1, /*windowDepth=*/100, config, pinned);
        if (!require(firstPixel(pinned) == (turbo[255] & 0x00FFFFFFu),
                     "an explicit maxIndex pins the far end of the palette"))
            return false;
    }

    return true;
}

bool testChunkedWrites(const QString &root)
{
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(false)) return false;
    const QString session = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).front();
    const QString path = root + '/' + session;
    constexpr int count = 2 * 65536 + 14; // Two chunk boundaries and a short tail.
    std::vector<double> raster(count);
    std::vector<int16_t> raw(count);
    const double values[] = {1.0 / 3.0, -0.0, 0.0, -1.5, 32767, -32768, 1e-6, 1e5};
    for (int i = 0; i < count; ++i) {
        raster[i] = values[i % 8];
        raw[i] = static_cast<int16_t>((i * 17) % 65536 - 32768);
    }
    const std::vector<MapInfo> map(2, {0, 1, 2, 3, 1, 0, 0, 0});
    auto verify = [&](const QString &name, DataFormat format, bool trajectory) {
        QFile file(path + '/' + name);
        if (!file.open(QIODevice::ReadOnly)) return false;
        const auto bytes = file.readAll();
        const int elementSize = format == DataFormat::Float64 ? 8
                              : format == DataFormat::Float32 ? 4 : 2;
        if (!require(bytes.size() == count * elementSize, "chunked write length")) return false;
        for (int i = 0; i < count; ++i) {
            const double value = trajectory ? static_cast<double>(raw[i]) : raster[i];
            const float narrow = static_cast<float>(value);
            const qfloat16 half(narrow);
            const void *expected = format == DataFormat::Float64 ? static_cast<const void *>(&value)
                                 : format == DataFormat::Float32 ? static_cast<const void *>(&narrow)
                                 : static_cast<const void *>(&half);
            if (!require(std::memcmp(bytes.constData() + i * elementSize, expected, elementSize) == 0,
                         "chunked payload preserves precision, signs, order and boundaries")) return false;
        }
        return true;
    };
    for (int f = 0; f < 3; ++f) {
        const auto format = static_cast<DataFormat>(f);
        if (!writer.saveRasterBscan(raster, f, count / 2, 2, format)) return false;
        if (!writer.saveTrajectoryFrame(raw.data(), 2, count / 2, map, 2, 2, f, format,
                0, 0, false, false, false, false, true, nullptr, {}, count / 2,
                PcdSegMode::Off, 0, 0)) return false;
        if (!verify(QString("%1.dat").arg(f), format, false) ||
            !verify(QString("frame_%1.dat").arg(f, 5, 10, QChar('0')), format, true)) return false;
    }
    return require(writer.savedItemCount() == 6, "polled saved counter counts completed tasks");
}

bool testLayeredPng(const QString &root)
{
    FileWriter writer;
    writer.setEnabled(true);
    writer.setSavePath(root);
    if (!writer.beginSession(false)) return false;
    const QString path = root + '/' + QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).front();
    // Colliding footprints, different peak depths, fractional DC, uncovered
    // pixels and duplicate corner indices distinguish max(grid) from grid(max).
    const std::vector<MapInfo> map = {
        {0, 1, 4, 5, .1f, .2f, .3f, .4f},
        {0, 1, 4, 5, .4f, .3f, .2f, .1f},
        {5, 6, 9, 10, .25f, .25f, .25f, .25f},
        {5, 5, 5, 5, .1f, .2f, .3f, .4f}};
    std::vector<float> depthMap(12);
    DepthConfig config;
    config.mode = DepthDisplayMode::Modulated;
    config.minIntensityFraction = 0;
    for (int depth : {1, 7, 17, 65}) {
        std::vector<int16_t> raw(map.size() * depth);
        for (size_t i = 0; i < raw.size(); ++i)
            raw[i] = static_cast<int16_t>((i * 997 + i / depth * 37) % 1000 - 500);
        for (size_t i = 0; i < depthMap.size(); ++i) depthMap[i] = i % depth;
        for (int full = 0; full <= 1; ++full) {
            if (!writer.saveTrajectoryFrame(raw.data(), static_cast<int>(map.size()), depth,
                    map, 4, 3, full, DataFormat::Float32, 1, 0, false,
                    full != 0, true, false, false, &depthMap, config, depth,
                    PcdSegMode::Off, 0, 0)) return false;
        }
        for (const auto &prefix : {QString("mip"), QString("depth")}) {
            const QImage layered(path + '/' + prefix + "_00000.png");
            const QImage full(path + '/' + prefix + "_00001.png");
            if (!require(!layered.isNull() && layered == full,
                         "layered PNG is pixel-identical to full-volume PNG")) return false;
        }
        // Depth-only uses the same bounded path even without the intensity PNG.
        if (!writer.saveTrajectoryFrame(raw.data(), static_cast<int>(map.size()), depth,
                map, 4, 3, 2, DataFormat::Float32, 1, 0, false,
                false, false, false, false, &depthMap, config, depth,
                PcdSegMode::Off, 0, 0)) return false;
        if (!require(QImage(path + "/depth_00002.png") == QImage(path + "/depth_00001.png"),
                     "depth-only PNG matches full-volume output")) return false;
    }
    return require(!QFile::exists(path + "/volume_00000.npy") &&
                   !QFile::exists(path + "/volume_00002.npy"), "PNG-only does not write NPY");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!require(temporary.isValid(), "temporary directory")) return 1;
    if (!testTrajectoryLoader(temporary.path())) return 1;

    QDir root(temporary.path());
    if (!root.mkdir("output")) return 1;
    if (!testFileWriter(root.absoluteFilePath("output"))) return 1;
    if (!testChunkedWrites(root.absoluteFilePath("chunks"))) return 1;
    if (!testLayeredPng(root.absoluteFilePath("layered"))) return 1;
    if (!testDepthEncode()) return 1;
    if (!testConfigPerMode(temporary.path())) return 1;
    if (!testNoSessionWhenNoOutputEnabled(temporary.path())) return 1;

    initColorMap();
    if (!require(mapToIndex(0.0, 1.0, 0, false) == 0, "zero color index")) return 1;
    if (!require(mapToIndex(1.0, 1.0, 0, false) == 255, "max color index")) return 1;
    std::cout << "All core tests passed\n";
    return 0;
}
