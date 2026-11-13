#include "CscanWidget.h"
#include "ColorMap.h"
#include <QOpenGLFunctions>
#include <algorithm>
#include <cstdint>
#include <cmath>

CscanWidget::CscanWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
}

void CscanWidget::setDimensions(int w, int h)
{
    m_w = w;
    m_h = h;
    m_image = QImage(w, h, QImage::Format_RGB888);  // width, height
    m_image.fill(0);
    m_rawMip.assign(static_cast<size_t>(w) * h, 0.0f);
    m_depthPlane.assign(static_cast<size_t>(w) * h, 0.0f);
    m_globalMipMax = 0.0f;
    m_committedRows = 0;
    m_textureDirty = true;
}

void CscanWidget::setScanRadius(double radiusM, double imageSizeM)
{
    m_scanRadiusM = radiusM;
    m_imageSizeM = imageSizeM;
    m_hasCircle = (radiusM > 0.0 && imageSizeM > 0.0);
}

void CscanWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_TEXTURE_2D);

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void CscanWidget::resizeGL(int w, int h)
{
    m_viewW = w;
    m_viewH = h;
}

void CscanWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_image.isNull() || m_viewW <= 0 || m_viewH <= 0) return;

    qreal dpr = devicePixelRatioF();
    int fboW = static_cast<int>(m_viewW * dpr);
    int fboH = static_cast<int>(m_viewH * dpr);
    glViewport(0, 0, fboW, fboH);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Upload texture if dirty
    {
        std::lock_guard<std::mutex> lock(m_mipMutex);
        if (m_textureDirty) {
            glBindTexture(GL_TEXTURE_2D, m_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                         m_image.width(), m_image.height(), 0,
                         GL_RGB, GL_UNSIGNED_BYTE, m_image.bits());
            m_textureDirty = false;
        }
    }

    // Compute quad to preserve aspect ratio
    float iw = static_cast<float>(m_image.width());
    float ih = static_cast<float>(m_image.height());
    float va = static_cast<float>(m_viewW) / static_cast<float>(m_viewH);
    float ia = iw / ih;
    float qw = 1.0f, qh = 1.0f;
    if (ia > va) qh = va / ia;
    else         qw = ia / va;
    float x0 = (1.0f - qw) * 0.5f;
    float y0 = (1.0f - qh) * 0.5f;

    // Draw MIP texture
    glColor3f(1.0f, 1.0f, 1.0f);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x0,      y0);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x0 + qw, y0);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x0 + qw, y0 + qh);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x0,      y0 + qh);
    glEnd();

    // Draw circle overlay (scan boundary)
    if (m_hasCircle && m_scanRadiusM > 0 && m_imageSizeM > 0) {
        double radiusNorm = m_scanRadiusM / (m_imageSizeM * 0.5);
        if (radiusNorm > 0 && radiusNorm <= 1.0) {
            float cx = x0 + qw * 0.5f;
            float cy = y0 + qh * 0.5f;
            float rx = static_cast<float>(radiusNorm) * qw * 0.5f;
            float ry = static_cast<float>(radiusNorm) * qh * 0.5f;

            glDisable(GL_TEXTURE_2D);
            glColor4f(0.0f, 1.0f, 0.0f, 0.6f);
            glLineWidth(1.5f);
            glBegin(GL_LINE_LOOP);
            const int N_SEG = 128;
            for (int i = 0; i < N_SEG; ++i) {
                float angle = 2.0f * 3.14159265f * static_cast<float>(i)
                              / static_cast<float>(N_SEG);
                glVertex2f(cx + rx * std::cos(angle), cy + ry * std::sin(angle));
            }
            glEnd();
            glEnable(GL_TEXTURE_2D);
        }
    }
}

void CscanWidget::updateMip(const std::vector<float> &mipData, int w, int h)
{
    if (w != m_w || h != m_h) {
        setDimensions(w, h);
    }
    if (static_cast<int>(mipData.size()) < m_w * m_h) return;

    {
        std::lock_guard<std::mutex> lock(m_mipMutex);

        // Store raw data + find global max
        float newMax = 0.0f;
        for (int i = 0; i < m_w * m_h; ++i) {
            m_rawMip[i] = mipData[i];
            if (mipData[i] > newMax) newMax = mipData[i];
        }
        m_globalMipMax = newMax;

        // Render entire image
        recolorAll();
        m_textureDirty = true;
    }
    update();
}

void CscanWidget::updateRasterFrame(const std::vector<float> &intensity,
                                   const std::vector<float> &depth, int w, int h)
{
    if (w <= 0 || h <= 0 || intensity.size() != static_cast<size_t>(w) * h ||
        depth.size() != intensity.size()) return;
    if (w != m_w || h != m_h) setDimensions(w, h);
    {
        std::lock_guard<std::mutex> lock(m_mipMutex);
        m_rawMip = intensity;
        m_depthPlane = depth;
        m_committedRows = w; // Full snapshot, including zeroed unacquired columns.
        m_globalMipMax = *std::max_element(m_rawMip.begin(), m_rawMip.end());
        if (m_depthDisplay) renderDepthLocked();
        else recolorAll();
        m_textureDirty = true;
    }
    update();
}

void CscanWidget::commitRow(int slowIdx, const std::vector<float> &mipRow)
{
    // Transposed raster: slowIdx is the SLOW axis (a horizontal column in the
    // displayed image); mipRow[y] holds the FAST-axis peak values (vertical).
    // So each committed "slow row" becomes a vertical column at x = slowIdx,
    // giving a C-scan whose fast axis runs top-to-bottom.
    if (slowIdx < 0 || slowIdx >= m_w ||
        static_cast<int>(mipRow.size()) < m_h)
        return;

    {
        std::lock_guard<std::mutex> lock(m_mipMutex);
        float rowMax = 0.0f;
        for (int y = 0; y < m_h; ++y) {
            const float v = mipRow[y];
            m_rawMip[y * m_w + slowIdx] = v;
            if (v > rowMax) rowMax = v;
        }

        const bool maxChanged = rowMax > m_globalMipMax;
        if (maxChanged) m_globalMipMax = rowMax;
        m_committedRows = std::max(m_committedRows, slowIdx + 1);

        if (m_depthDisplay) {
            // Depth rendering repaints the whole plane anyway, so the
            // incremental intensity fast path does not apply.
            renderDepthLocked();
        } else if (maxChanged) {
            recolorAll();
        } else {
            const uint32_t *lut = colorLut(m_colorMap);
            for (int y = 0; y < m_h; ++y) {
                uchar *line = m_image.scanLine(y);
                const int idx = mapToIndex(m_rawMip[y * m_w + slowIdx], m_globalMipMax,
                                           m_colorMapping, m_colorClipping);
                const uint32_t color = lut[idx];
                line[slowIdx * 3 + 0] = color & 0xFF;
                line[slowIdx * 3 + 1] = (color >> 8) & 0xFF;
                line[slowIdx * 3 + 2] = (color >> 16) & 0xFF;
            }
        }
        m_textureDirty = true;
    }
    update();
}

void CscanWidget::commitDepthRow(int slowIdx, const std::vector<float> &depthRow)
{
    if (slowIdx < 0 || slowIdx >= m_w ||
        static_cast<int>(depthRow.size()) < m_h ||
        static_cast<int>(m_depthPlane.size()) < m_w * m_h)
        return;

    {
        std::lock_guard<std::mutex> lock(m_mipMutex);
        for (int y = 0; y < m_h; ++y)
            m_depthPlane[y * m_w + slowIdx] = depthRow[y];
        if (m_depthDisplay) renderDepthLocked();
        m_textureDirty = true;
    }
    update();
}

// ===== Depth rendering =====

void CscanWidget::setDepthDisplay(bool on)
{
    if (m_depthDisplay == on) return;
    m_depthDisplay = on;
    recolorCurrent();   // repaint from whichever plane is now active
}

void CscanWidget::setDepthConfig(const DepthConfig &config)
{
    m_depthConfig = config;
    if (m_depthDisplay) recolorCurrent();
}

void CscanWidget::setDepthRange(int maxIndex)
{
    if (m_depthConfig.maxIndex == maxIndex) return;
    m_depthConfig.maxIndex = maxIndex;
    if (m_depthDisplay) recolorCurrent();
}

void CscanWidget::setWindowDepth(int depth)
{
    m_windowDepth = depth;
    m_depthConfig.fallbackDepth = depth;
}

void CscanWidget::renderDepthLocked()
{
    // m_windowDepth is the signal window in samples — the depth axis the color
    // scale is built on, independent of this widget's own dimensions.
    encodeDepthImage(m_depthPlane.data(), m_rawMip.data(), m_w, m_h,
                     m_windowDepth, m_depthConfig, m_image);
}

void CscanWidget::recolorAll()
{
    const uint32_t *lut = colorLut(m_colorMap);
    for (int y = 0; y < m_h; ++y) {
        int rowBase = y * m_w;
        for (int x = 0; x < m_w; ++x) {
            int idx = mapToIndex(m_rawMip[rowBase + x], m_globalMipMax,
                                 m_colorMapping, m_colorClipping);
            uint32_t c = lut[idx];
            // scanLine(y) = row y, + x*3 = column x (no transpose)
            uchar *p = m_image.scanLine(y) + x * 3;
            p[0] = c & 0xFF;
            p[1] = (c >> 8) & 0xFF;
            p[2] = (c >> 16) & 0xFF;
        }
    }
}

void CscanWidget::updateMipImage(const QImage &image, float maxValue)
{
    if (image.isNull()) return;
    {
        std::lock_guard<std::mutex> lock(m_mipMutex);
        m_image = image;
        m_globalMipMax = maxValue;
        m_textureDirty = true;
    }
    update();
}

void CscanWidget::setColorParams(int mapping, bool clipping)
{
    if (m_colorMapping == mapping && m_colorClipping == clipping) return;
    m_colorMapping = mapping;
    m_colorClipping = clipping;
    recolorCurrent();
}

void CscanWidget::setColorMap(int colormap)
{
    if (m_colorMap == colormap) return;
    m_colorMap = colormap;
    recolorCurrent();
}

void CscanWidget::recolorCurrent()
{
    {
        std::lock_guard<std::mutex> lock(m_mipMutex);
        // Depth rendering needs a depth plane to paint from. Trajectory frames
        // are colored by MainWindow (updateMipImage), so this raster path only
        // takes over once the pipeline has actually committed depth rows —
        // otherwise a settings change during startup would blank the view.
        if (m_depthDisplay
            && static_cast<int>(m_depthPlane.size()) >= m_w * m_h
            && m_committedRows > 0) {
            renderDepthLocked();
        } else {
            recolorAll();
        }
        m_textureDirty = true;
    }
    update();
}
