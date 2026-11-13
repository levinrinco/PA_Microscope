#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QImage>
#include <vector>
#include <mutex>
#include "DepthEncode.h"

class CscanWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit CscanWidget(QWidget *parent = nullptr);
    void setDimensions(int w, int h);
    void setScanRadius(double radiusM, double imageSizeM);
    void setColorParams(int mapping, bool clipping);
    void setColorMap(int colormap);
    void recolorCurrent();
    int currentMaxValue() const { return static_cast<int>(m_globalMipMax); }

    // ---- Depth encoding (raster live view) ----
    /// Enable/disable depth rendering of the accumulated raster C-scan. When
    /// enabled the image is painted from the depth plane instead of intensity.
    void setDepthDisplay(bool on);
    /// Depth settings; also refreshes the image when depth rendering is active.
    void setDepthConfig(const DepthConfig &config);
    /// Depth index mapped to the far end of the colormap (<= 0 = full window).
    void setDepthRange(int maxIndex);
    /// Deepest sample of the current window, i.e. the last valid depth index.
    void setWindowDepth(int depth);
    bool depthDisplay() const { return m_depthDisplay; }

public slots:
    /// Append/replace one raster "slow row". For raster the image is
    /// width = nSlow (slow axis, horizontal), height = nFast (fast axis,
    /// vertical); each committed slow row becomes a vertical column so the
    /// fast axis runs top-to-bottom.
    void commitRow(int slowIdx, const std::vector<float> &mipRow);
    /// Depth twin of commitRow: one envelope-peak depth index per fast-axis
    /// position of the same slow row.
    void commitDepthRow(int slowIdx, const std::vector<float> &depthRow);
    /// Full-image MIP update (replaces raster commitRow)
    void updateMip(const std::vector<float> &mipData, int w, int h);
    void updateRasterFrame(const std::vector<float> &intensity,
                           const std::vector<float> &depth, int w, int h);
    /// Receive pre-colored QImage from pipeline (UITZ-style inline MIP)
    void updateMipImage(const QImage &image, float maxValue = 0.0f);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void recolorAll();
    /// Paint the whole image from the depth plane (must hold m_mipMutex).
    void renderDepthLocked();

    int m_w = 500, m_h = 500;

    QImage m_image;
    GLuint m_texture = 0;
    std::mutex m_mipMutex;
    bool m_textureDirty = false;

    std::vector<float> m_rawMip;
    float m_globalMipMax = 0.0f;
    int m_committedRows = 0;

    // Raster depth plane, same (fast, slow) layout and indexing as m_rawMip.
    std::vector<float> m_depthPlane;
    bool m_depthDisplay = false;
    DepthConfig m_depthConfig;
    // Signal window in samples. This is the depth axis used for the color
    // scale; it is deliberately NOT m_h (the widget height), which for a
    // raster C-scan is the fast-axis point count instead.
    int m_windowDepth = 0;

    int m_viewW = 1, m_viewH = 1;
    int  m_colorMapping = 0;
    bool m_colorClipping = true;
    int  m_colorMap = 0;

    // Circle overlay
    double m_scanRadiusM = 0.0;
    double m_imageSizeM = 0.0;
    bool m_hasCircle = false;
};
