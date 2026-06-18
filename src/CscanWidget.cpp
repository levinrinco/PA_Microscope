#include "CscanWidget.h"
#include <QOpenGLFunctions>
#include <cstdint>
#include <cmath>

extern uint32_t hotLUT[256];
extern int mapToIndex(double value, double maxVal, int mapping, bool clipping);

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
    m_globalMipMax = 0.0f;
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
                float angle = 2.0f * 3.14159265f * i / N_SEG;
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

void CscanWidget::recolorAll()
{
    for (int y = 0; y < m_h; ++y) {
        int rowBase = y * m_w;
        for (int x = 0; x < m_w; ++x) {
            int idx = mapToIndex(m_rawMip[rowBase + x], m_globalMipMax,
                                 m_colorMapping, m_colorClipping);
            uint32_t c = hotLUT[idx];
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

void CscanWidget::recolorCurrent()
{
    {
        std::lock_guard<std::mutex> lock(m_mipMutex);
        recolorAll();
        m_textureDirty = true;
    }
    update();
}
