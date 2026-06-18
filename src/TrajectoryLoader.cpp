#include "TrajectoryLoader.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

// Strip UTF-8 BOM if present
static inline void stripUtf8Bom(QString &s) {
    if (s.size() >= 1) {
        const QChar fc = s.at(0);
        if (fc.unicode() == 0xFEFF)
            s.remove(0, 1);
    }
}

// Check if a string looks like a header (contains non-numeric first token)
static inline bool isHeaderLine(const QString &line) {
    QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) return false;
    // Split on comma
    QStringList cols = trimmed.split(QLatin1Char(','));
    if (cols.isEmpty()) return false;
    // Try to parse first column as float
    bool ok = false;
    cols[0].trimmed().toFloat(&ok);
    return !ok;  // header if first column is NOT numeric
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

bool TrajectoryLoader::load(const QString &csvPath,
                            int imageW, int imageH,
                            bool swapXY,
                            std::vector<TrajectoryPoint> &outPoints,
                            std::vector<MapInfo> &outMapInfo,
                            double &outRadiusM)
{
    m_totalAlines = 0;
    m_lastError.clear();
    outPoints.clear();
    outMapInfo.clear();

    // --- Parse CSV ---
    std::vector<float> x_m, y_m;
    if (!parseCsv(csvPath, x_m, y_m)) {
        return false;
    }

    // Swap X/Y if requested
    if (swapXY) {
        x_m.swap(y_m);
        qInfo() << "TrajectoryLoader: X/Y swapped";
    }

    m_totalAlines = static_cast<int>(x_m.size());

    // --- Detect scan radius (max absolute coordinate, with margin) ---
    float maxAbs = 0.0f;
    for (size_t i = 0; i < x_m.size(); ++i) {
        maxAbs = std::max(maxAbs, std::fabs(x_m[i]));
        maxAbs = std::max(maxAbs, std::fabs(y_m[i]));
    }
    outRadiusM = static_cast<double>(maxAbs) * 1.05;  // 5% margin

    // --- Build trajectory points ---
    outPoints.resize(m_totalAlines);
    for (int i = 0; i < m_totalAlines; ++i) {
        float x = x_m[i];
        float y = y_m[i];
        outPoints[i].x_m   = x;
        outPoints[i].y_m   = y;
        outPoints[i].r_m   = std::sqrt(x*x + y*y);
        outPoints[i].theta = std::atan2(y, x);
    }

    // --- Precompute bilinear interpolation MapInfo ---
    buildMapInfo(x_m, y_m, imageW, imageH, outRadiusM, outMapInfo);

    qInfo() << "TrajectoryLoader: loaded" << m_totalAlines << "A-lines"
            << "radius" << outRadiusM << "m"
            << "grid" << imageW << "x" << imageH;

    // Diagnostic: print first 5 trajectory points and their pixel mappings
    qInfo() << "--- First 5 trajectory points ---";
    for (int i = 0; i < std::min(5, m_totalAlines); ++i) {
        const auto &pt = outPoints[i];
        const auto &mi = outMapInfo[i];
        int px0 = mi.idx00 % imageW, py0 = mi.idx00 / imageW;
        qInfo() << QString("  [%1] x=%2 y=%3 r=%4 → pixel(%5,%6) w=[%7,%8,%9,%10]")
            .arg(i)
            .arg(pt.x_m, 0, 'f', 6)
            .arg(pt.y_m, 0, 'f', 6)
            .arg(pt.r_m, 0, 'f', 6)
            .arg(px0).arg(py0)
            .arg(mi.w00, 0, 'f', 3)
            .arg(mi.w01, 0, 'f', 3)
            .arg(mi.w10, 0, 'f', 3)
            .arg(mi.w11, 0, 'f', 3);
    }
    // Print last point
    {
        int i = m_totalAlines - 1;
        const auto &pt = outPoints[i];
        const auto &mi = outMapInfo[i];
        int px0 = mi.idx00 % imageW, py0 = mi.idx00 / imageW;
        qInfo() << QString("  [%1] x=%2 y=%3 r=%4 → pixel(%5,%6)")
            .arg(i)
            .arg(pt.x_m, 0, 'f', 6)
            .arg(pt.y_m, 0, 'f', 6)
            .arg(pt.r_m, 0, 'f', 6)
            .arg(px0).arg(py0);
    }
    qInfo() << "--- End trajectory diagnostics ---";

    return true;
}

// ---------------------------------------------------------------------------
// parseCsv — simplified 3-column format: index, x_m, y_m
// ---------------------------------------------------------------------------

bool TrajectoryLoader::parseCsv(const QString &path,
                                std::vector<float> &x_m,
                                std::vector<float> &y_m)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QString("Cannot open CSV: %1").arg(path);
        qWarning() << m_lastError;
        return false;
    }

    QTextStream stream(&file);
    x_m.clear();
    y_m.clear();

    int lineNo = 0;
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        ++lineNo;

        // Strip BOM on first line
        if (lineNo == 1)
            stripUtf8Bom(line);

        // Skip empty lines
        if (line.trimmed().isEmpty())
            continue;

        // Skip header line (first non-numeric line)
        if (lineNo == 1 && isHeaderLine(line))
            continue;

        // Split on comma
        QStringList cols = line.split(QLatin1Char(','));
        if (cols.size() < 3) {
            // Try tab-separated
            cols = line.split(QLatin1Char('\t'));
            if (cols.size() < 3) {
                qDebug() << "TrajectoryLoader: skipping line" << lineNo
                         << "(need 3 columns, got" << cols.size() << ")";
                continue;
            }
        }

        bool ok = false;
        float x = cols[1].trimmed().toFloat(&ok);
        if (!ok) {
            qDebug() << "TrajectoryLoader: skipping line" << lineNo
                     << "(bad x_m value)";
            continue;
        }
        float y = cols[2].trimmed().toFloat(&ok);
        if (!ok) {
            qDebug() << "TrajectoryLoader: skipping line" << lineNo
                     << "(bad y_m value)";
            continue;
        }

        x_m.push_back(x);
        y_m.push_back(y);
    }

    file.close();

    if (x_m.empty()) {
        m_lastError = QString("CSV contains no valid data: %1").arg(path);
        qWarning() << m_lastError;
        return false;
    }

    qInfo() << "TrajectoryLoader: parsed" << x_m.size() << "points from CSV";
    return true;
}

// ---------------------------------------------------------------------------
// buildMapInfo — ported from CODE TEST precomputeMapInfo()
// Maps x_m, y_m (meters) to image pixel grid with bilinear weights
// ---------------------------------------------------------------------------

void TrajectoryLoader::buildMapInfo(const std::vector<float> &x_m,
                                    const std::vector<float> &y_m,
                                    int W_img, int H_img,
                                    double R_m_in,
                                    std::vector<MapInfo> &out)
{
    const int N = static_cast<int>(x_m.size());
    out.resize(N);

    // Guard against zero-radius trajectory
    double R = R_m_in;
    if (R <= 0.0) {
        qWarning() << "buildMapInfo: R is zero, using fallback radius 1.0";
        R = 1.0;
    }

    for (int i = 0; i < N; ++i) {
        // Map x_m / y_m from [-R, +R] to pixel coordinate [0, W-1] / [0, H-1]
        float fx = (x_m[i] / static_cast<float>(R) + 1.0f) * 0.5f * (W_img - 1);
        float fy = (y_m[i] / static_cast<float>(R) + 1.0f) * 0.5f * (H_img - 1);

        // Clamp to valid pixel range
        fx = clampf(fx, 0.0f, static_cast<float>(W_img - 1));
        fy = clampf(fy, 0.0f, static_cast<float>(H_img - 1));

        // Bilinear interpolation weights
        int x0 = static_cast<int>(std::floor(fx));
        int y0 = static_cast<int>(std::floor(fy));
        int x1 = std::min(x0 + 1, W_img - 1);
        int y1 = std::min(y0 + 1, H_img - 1);

        float dx = fx - static_cast<float>(x0);
        float dy = fy - static_cast<float>(y0);

        MapInfo &m = out[i];
        m.idx00 = y0 * W_img + x0;
        m.idx01 = y0 * W_img + x1;
        m.idx10 = y1 * W_img + x0;
        m.idx11 = y1 * W_img + x1;

        m.w00 = (1.0f - dx) * (1.0f - dy);
        m.w01 =        dx  * (1.0f - dy);
        m.w10 = (1.0f - dx) *        dy;
        m.w11 =        dx  *        dy;
    }
}
