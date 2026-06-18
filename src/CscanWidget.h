#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QImage>
#include <vector>
#include <mutex>

class CscanWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit CscanWidget(QWidget *parent = nullptr);
    void setDimensions(int w, int h);
    void setScanRadius(double radiusM, double imageSizeM);
    void setColorParams(int mapping, bool clipping);
    void recolorCurrent();
    int currentMaxValue() const { return static_cast<int>(m_globalMipMax); }

public slots:
    /// Full-image MIP update (replaces raster commitRow)
    void updateMip(const std::vector<float> &mipData, int w, int h);
    /// Receive pre-colored QImage from pipeline (UITZ-style inline MIP)
    void updateMipImage(const QImage &image, float maxValue = 0.0f);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void recolorAll();

    int m_w = 500, m_h = 500;

    QImage m_image;
    GLuint m_texture = 0;
    std::mutex m_mipMutex;
    bool m_textureDirty = false;

    std::vector<float> m_rawMip;
    float m_globalMipMax = 0.0f;

    int m_viewW = 1, m_viewH = 1;
    int  m_colorMapping = 0;
    bool m_colorClipping = true;

    // Circle overlay
    double m_scanRadiusM = 0.0;
    double m_imageSizeM = 0.0;
    bool m_hasCircle = false;
};
