#include "DepthRulerWidget.h"
#include <QPainter>
#include <QPen>
#include <QFont>

DepthRulerWidget::DepthRulerWidget(QWidget *parent)
    : QWidget(parent)
{
    // A fixed, thin column that sits between the A-line and B-scan widgets.
    setFixedWidth(46);
}

void DepthRulerWidget::setDepth(int depth)
{
    if (depth != m_depth) {
        m_depth = depth;
        update();
    }
}

void DepthRulerWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QFont f("Consolas", 8);
    f.setPixelSize(9);
    p.setFont(f);
    p.setPen(QColor(170, 170, 170));

    const int axisX  = width() - 12;   // vertical axis line near the right edge
    // The B-scan image fills its widget top-to-bottom (depth axis), so this
    // ruler spans the full height too: 0 at the very top, depth at the bottom.
    const int top    = 0;
    const int bottom = height() - 1;

    // Axis line spanning the full height (matches the B-scan image fill).
    p.drawLine(axisX, top, axisX, bottom);

    // Fixed sample-depth interval: 0, 100, 200, ... within the visible range.
    if (m_depth <= 0) return;
    constexpr int tickStep = 100;
    const int labelH = 14;
    for (int i = 0; i <= m_depth / tickStep; ++i) {
        const int value = i * tickStep;
        // Named `frac`, not `f`: the QFont above is already `f`, and MSVC /W4
        // reports the shadowing as C4456.
        const double frac = static_cast<double>(value) / m_depth;
        const int y      = top + static_cast<int>(frac * (bottom - top));
        p.drawLine(axisX - 3, y, axisX, y);

        // Right-aligned label, clamped so the top/bottom labels stay inside.
        int labelY = y - labelH / 2;
        if (labelY < 0) labelY = 0;
        if (labelY + labelH > height()) labelY = height() - labelH;
        p.drawText(QRect(0, labelY, axisX - 8, labelH),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(value));
    }
}
