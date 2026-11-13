#pragma once

#include <QImage>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <mutex>
#include <vector>

class BscanWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit BscanWidget(QWidget *parent = nullptr);

    void setDimensions(int depth, int nFast);
    void setColorParams(int mapping, bool clipping);
    void setColorMap(int colormap);
    void recolorCurrent();
    int currentMaxValue() const { return static_cast<int>(m_frameGlobalMax); }

public slots:
    void setBscanData(const std::vector<double> &envelope, int depth, int nFast);
    void setRawBscanData(const std::vector<double> &raw, int depth, int points);
    void resetBscan(int bscanIdx = 0);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void setData(const std::vector<double> &data, int depth, int points, bool signedSamples);
    void recolorAll();

    int m_depth = 600;
    int m_nFast = 1000;
    int m_currentBscan = -1;

    QImage m_image;
    GLuint m_texture = 0;
    int m_maxTextureSize = 1;
    std::mutex m_imageMutex;
    bool m_textureDirty = false;

    int m_viewW = 1;
    int m_viewH = 1;
    double m_frameGlobalMax = 0.0;

    int m_colorMapping = 0;
    bool m_colorClipping = true;
    int m_colorMap = 0;
    std::vector<double> m_rawEnvelope;
};
