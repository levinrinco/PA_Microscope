#include "AlineWidget.h"
#include <QOpenGLFunctions>
#include <QPainter>
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

    qreal dpr = devicePixelRatioF();
    int fboW = static_cast<int>(m_viewW * dpr);
    int fboH = static_cast<int>(m_viewH * dpr);
    int margin = static_cast<int>(4 * dpr);

    // Reserve bottom 36 logical px for scale bar
    int scaleBarH = static_cast<int>(36 * dpr);
    int traceH = fboH - 2 * margin - scaleBarH;

    // === Trace area ===
    glViewport(margin, margin + scaleBarH, fboW - 2*margin, traceH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Data from pipeline is already DC-centered; just find maxAbs
    double maxAbs = 1e-9;
    for (auto v : m_data) {
        double a = std::abs(v);
        if (a > maxAbs) maxAbs = a;
    }
    double invMax = 1.0 / maxAbs;
    double n = static_cast<double>(m_data.size());

    // Draw zero reference line
    glColor3f(0.15f, 0.15f, 0.15f);
    glBegin(GL_LINES);
    glVertex2d(0.5, 0.0); glVertex2d(0.5, 1.0);
    glEnd();

    // Draw signal trace
    glColor3f(0.0f, 1.0f, 0.0f);
    glBegin(GL_LINE_STRIP);
    for (size_t i = 0; i < m_data.size(); ++i) {
        double x = 0.5 + 0.45 * m_data[i] * invMax;
        double y = 1.0 - static_cast<double>(i) / n;
        glVertex2d(x, y);
    }
    glEnd();

    // === Scale bar at bottom ===
    glViewport(margin, margin, fboW - 2*margin, scaleBarH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Horizontal axis line
    glColor3f(0.4f, 0.4f, 0.4f);
    glBegin(GL_LINES);
    glVertex2d(0.05, 0.5); glVertex2d(0.95, 0.5);
    glEnd();

    // Tick marks at -max, 0, +max (mapped to x=0.05, x=0.5, x=0.95)
    double tickVals[3] = {-maxAbs, 0.0, maxAbs};
    double tickPos[3]  = {0.05, 0.5, 0.95};
    glBegin(GL_LINES);
    for (int i = 0; i < 3; ++i) {
        glVertex2d(tickPos[i], 0.2); glVertex2d(tickPos[i], 0.8);
    }
    glEnd();

    // Text labels via QPainter overlay
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font("Consolas", 9);
    font.setPixelSize(static_cast<int>(10 * dpr));
    painter.setFont(font);
    painter.setPen(QColor(180, 180, 180));

    int scaleBarY = fboH - margin - scaleBarH;
    // Tick labels aligned to logical pixel positions within the scale bar
    for (int i = 0; i < 3; ++i) {
        int labelX = static_cast<int>(margin + tickPos[i] * (fboW - 2*margin));
        int labelY = scaleBarY + scaleBarH / 2 + static_cast<int>(4 * dpr);
        painter.drawText(QRect(labelX - 40, labelY, 80, static_cast<int>(14*dpr)),
                         Qt::AlignHCenter | Qt::AlignTop,
                         QString::number(static_cast<int>(tickVals[i])));
    }
    painter.end();
}

void AlineWidget::setAlineData(const std::vector<double> &envelope)
{
    {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_data = envelope;
    }
    update(); // schedule repaint
}
