#pragma once

#include <QObject>
#include <QDateTime>
#include <QString>
#include <vector>
#include "Types.h"

class FileWriter : public QObject {
    Q_OBJECT
public:
    explicit FileWriter(QObject *parent = nullptr);
    ~FileWriter();

    void setSavePath(const QString &dir);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    /// Call at scan start — creates session folder. Subsequent frames go under it.
    void beginSession();

    /// Grid raw A-lines → 3D volume (.npy) + MIP (.png)
    bool saveGriddedVolume(const int16_t *rawAlines,
                           int alineCount, int depth,
                           const std::vector<MapInfo> &mapInfo,
                           int imageW, int imageH,
                           int frameIdx = 0);

signals:
    void errorOccurred(const QString &message);

private:
    QString m_savePath;
    QString m_sessionDir;    // current session subfolder (cleared at endSession)
    bool m_enabled = false;

    void gridVolume(const int16_t *rawAlines,
                    int alineCount, int depth,
                    const std::vector<MapInfo> &mapInfo,
                    int imageW, int imageH,
                    std::vector<float> &volumeOut,
                    std::vector<float> &mipOut);

    bool saveMipPng(const QString &path, const std::vector<float> &mip,
                    int imageW, int imageH);

    bool saveVolumeNpy(const QString &path, const std::vector<float> &volume,
                       int imageW, int imageH, int depth);
};
