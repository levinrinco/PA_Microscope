#include "AlineWidget.h"
#include <QOpenGLFunctions>
#include <cmath>

AlineWidget::AlineWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
}

void AlineWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glLineWidth(1.0f);
}

void AlineWidget::resizeGL(int w, int h)
{
    m_viewW = w;
    m_viewH = h;
}

void AlineWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_viewW <= 0 || m_viewH <= 0) return;

    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (m_data.empty())
        return;

    const qreal dpr = devicePixelRatioF();
    const int fboW = static_cast<int>(m_viewW * dpr);
    const int fboH = static_cast<int>(m_viewH * dpr);
    const int margin = static_cast<int>(4 * dpr);

    // === Trace area (full height; no scale bar / ruler reserved) ===
    glViewport(margin, margin, fboW - 2 * margin, fboH - 2 * margin);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Data is already DC-centered; just find max absolute value.
    double maxAbs = 1e-9;
    for (auto v : m_data) {
        double a = std::abs(v);
        if (a > maxAbs) maxAbs = a;
    }
    const double invMax = 1.0 / maxAbs;
    const double n = static_cast<double>(m_data.size());

    // Zero reference line
    glColor3f(0.15f, 0.15f, 0.15f);
    glBegin(GL_LINES);
    glVertex2d(0.5, 0.0); glVertex2d(0.5, 1.0);
    glEnd();

    // Signal trace
    glColor3f(0.0f, 1.0f, 0.0f);
    glBegin(GL_LINE_STRIP);
    for (size_t i = 0; i < m_data.size(); ++i) {
        const double x = 0.5 + 0.45 * m_data[i] * invMax;
        const double y = 1.0 - static_cast<double>(i) / n;
        glVertex2d(x, y);
    }
    glEnd();
}

void AlineWidget::setAlineData(const std::vector<double> &envelope)
{
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_data = envelope;
    }
    update(); // schedule repaint
}
