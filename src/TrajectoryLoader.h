#pragma once

#include <QString>
#include <vector>
#include "Types.h"

/// Loads spiral trajectory from CSV and precomputes bilinear interpolation mapping.
class TrajectoryLoader {
public:
    /// Load a spiral trajectory CSV file.
    /// CSV format: index, x_m, y_m  (header row optional, auto-detected)
    /// @param csvPath   Path to the trajectory CSV file
    /// @param imageW    Output grid pixel width
    /// @param imageH    Output grid pixel height
    /// @param swapXY    If true, swap x_m and y_m columns
    /// @param outPoints [out] Parsed trajectory points (x_m, y_m, r_m, theta)
    /// @param outMapInfo [out] Bilinear interpolation map per A-line
    /// @param outRadiusM [out] Detected scan radius (max |x|, |y|)
    /// @return true on success
    bool load(const QString &csvPath,
              int imageW, int imageH,
              bool swapXY,
              std::vector<TrajectoryPoint> &outPoints,
              std::vector<MapInfo> &outMapInfo,
              double &outRadiusM);

    /// Total A-lines from last successful load
    int totalAlines() const { return m_totalAlines; }

    /// Error message from last failed load
    QString lastError() const { return m_lastError; }

private:
    int m_totalAlines = 0;
    QString m_lastError;

    /// Parse CSV: extract x_m, y_m columns. Returns true on success.
    bool parseCsv(const QString &path,
                  std::vector<float> &x_m,
                  std::vector<float> &y_m);

    /// Precompute bilinear interpolation MapInfo.
    /// Ported from CODE TEST trajectory_utils.cpp — math preserved.
    static void buildMapInfo(const std::vector<float> &x_m,
                             const std::vector<float> &y_m,
                             int W, int H, double R,
                             std::vector<MapInfo> &out);
};
