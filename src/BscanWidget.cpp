#include "BscanWidget.h"
#include "ColorMap.h"
#include <QOpenGLFunctions>
#include <QPainter>
#include <algorithm>
#include <cmath>

BscanWidget::BscanWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    initColorMap();
}

void BscanWidget::setDimensions(int depth, int nFast)
{
    if (depth <= 0 || nFast <= 0) return;
    std::lock_guard<std::mutex> lock(m_imageMutex);
    m_depth = depth;
    m_nFast = nFast;
    m_image = QImage(nFast, depth, QImage::Format_RGB888);
    m_image.fill(0);
    m_rawEnvelope.clear();
    m_frameGlobalMax = 0.0;
    m_textureDirty = true;
}

void BscanWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void BscanWidget::resizeGL(int w, int h)
{
    m_viewW = w;
    m_viewH = h;
}

void BscanWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_image.isNull() || m_viewW <= 0 || m_viewH <= 0) return;

    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, static_cast<int>(m_viewW * dpr),
               static_cast<int>(m_viewH * dpr));
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    {
        std::lock_guard<std::mutex> lock(m_imageMutex);
        if (m_textureDirty) {
            glBindTexture(GL_TEXTURE_2D, m_texture);
            // A full spiral can exceed the GPU's texture-width limit. Keep all
            // samples for recoloring, scaling only the uploaded display image.
            const QImage textureImage = (m_image.width() > m_maxTextureSize ||
                                         m_image.height() > m_maxTextureSize)
                ? m_image.scaled(std::min(m_image.width(), m_maxTextureSize),
                                 std::min(m_image.height(), m_maxTextureSize),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_RGB888)
                : m_image;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                         textureImage.width(), textureImage.height(), 0,
                         GL_RGB, GL_UNSIGNED_BYTE, textureImage.constBits());
            m_textureDirty = false;
        }
    }

    // Fill the whole widget: the depth axis always spans the full height, so
    // the external DepthRulerWidget (also full height) stays aligned with the
    // B-scan image regardless of the configured acquisition depth.
    const float quadW = 1.0f, quadH = 1.0f;
    const float x0 = 0.0f, y0 = 0.0f;

    glColor3f(1.0f, 1.0f, 1.0f);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glBegin(GL_QUADS);
    // Image row 0 holds depth 0 — the earliest samples of the A-line — and must
    // appear at the TOP of the widget, matching the depth ruler on the left and
    // the A-line widget. Under glOrtho(0,1,0,1) the ortho y=0 edge is the
    // BOTTOM of the viewport, so texture v=0 (row 0) is bound to the top edge
    // and v=1 to the bottom edge.
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x0,         y0);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x0 + quadW, y0);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x0 + quadW, y0 + quadH);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x0,         y0 + quadH);
    glEnd();
}

void BscanWidget::setBscanData(const std::vector<double> &envelope,
                               int depth, int nFast)
{
    setData(envelope, depth, nFast, false);
}

void BscanWidget::setRawBscanData(const std::vector<double> &raw, int depth, int points)
{
    setData(raw, depth, points, true);
}

void BscanWidget::setData(const std::vector<double> &envelope, int depth,
                          int nFast, bool signedSamples)
{
    if (depth <= 0 || nFast <= 0) return;
    const size_t total = static_cast<size_t>(depth) * nFast;
    if (envelope.size() < total) return;
    if (depth != m_depth || nFast != m_nFast) setDimensions(depth, nFast);

    {
        std::lock_guard<std::mutex> lock(m_imageMutex);
        m_rawEnvelope.assign(envelope.begin(), envelope.begin() + total);
        m_frameGlobalMax = 0.0;
        for (double &value : m_rawEnvelope) {
            if (signedSamples) value = std::abs(value);
            if (value > m_frameGlobalMax) m_frameGlobalMax = value;
        }
        recolorAll();
        m_textureDirty = true;
    }
    update();
}

void BscanWidget::recolorAll()
{
    if (m_rawEnvelope.size() < static_cast<size_t>(m_depth) * m_nFast) return;
    const uint32_t *lut = colorLut(m_colorMap);
    for (int col = 0; col < m_nFast; ++col) {
        const size_t base = static_cast<size_t>(col) * m_depth;
        for (int row = 0; row < m_depth; ++row) {
            const int index = mapToIndex(m_rawEnvelope[base + row],
                                         m_frameGlobalMax,
                                         m_colorMapping,
                                         m_colorClipping);
            const uint32_t color = lut[index];
            uchar *pixel = m_image.scanLine(row) + col * 3;
            pixel[0] = color & 0xFF;
            pixel[1] = (color >> 8) & 0xFF;
            pixel[2] = (color >> 16) & 0xFF;
        }
    }
}

void BscanWidget::setColorParams(int mapping, bool clipping)
{
    if (m_colorMapping == mapping && m_colorClipping == clipping) return;
    m_colorMapping = mapping;
    m_colorClipping = clipping;
    recolorCurrent();
}

void BscanWidget::setColorMap(int colormap)
{
    if (m_colorMap == colormap) return;
    m_colorMap = colormap;
    recolorCurrent();
}

void BscanWidget::recolorCurrent()
{
    {
        std::lock_guard<std::mutex> lock(m_imageMutex);
        recolorAll();
        m_textureDirty = true;
    }
    update();
}

void BscanWidget::resetBscan(int bscanIdx)
{
    {
        std::lock_guard<std::mutex> lock(m_imageMutex);
        m_image.fill(0);
        m_rawEnvelope.clear();
        m_frameGlobalMax = 0.0;
        m_textureDirty = true;
    }
    m_currentBscan = bscanIdx;
    update();
}
