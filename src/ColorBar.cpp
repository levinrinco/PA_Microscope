#include "ColorBar.h"
#include <QPainter>

ColorBar::ColorBar(QWidget *parent) : QWidget(parent)
{
    initColorMap();
    setFixedHeight(28);
    setMinimumWidth(40);
}

void ColorBar::setMaxValue(int maxVal)
{
    m_maxVal = maxVal;
    update();
}

void ColorBar::setInputRange(int rangeMv)
{
    if (rangeMv <= 0 || rangeMv == m_rangeMv) return;
    m_rangeMv = rangeMv;
    update();
}

void ColorBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    int m = 3;  // horizontal margin
    int barY = 2, barH = 10;
    int barX = m, barW = width() - 2 * m;
    if (barW < 10) return;

    // Gradient
    for (int i = 0; i < 256; ++i) {
        uint32_t c = hotLUT[i];
        int x0 = barX + (i * barW) / 256;
        int x1 = barX + ((i + 1) * barW) / 256;
        if (x1 <= x0) x1 = x0 + 1;
        p.fillRect(x0, barY, x1 - x0, barH,
                   QColor(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF));
    }
    p.setPen(QColor(100, 100, 100));
    p.drawRect(barX, barY, barW, barH);

    // Labels directly below the gradient, tight against bar edges
    p.setPen(palette().color(QPalette::WindowText));
    int textY = barY + barH + 12;

    p.drawText(barX, textY, "0");

    // Convert raw counts to mV using input range
    double mvPerCount = m_rangeMv / kAdcFullScale;
    double mvVal = m_maxVal * mvPerCount;

    QString rText;
    if (mvVal < 1.0)        rText = QString::number(mvVal, 'f', 3) + " mV";
    else if (mvVal < 10.0)  rText = QString::number(mvVal, 'f', 2) + " mV";
    else if (mvVal < 100.0) rText = QString::number(mvVal, 'f', 1) + " mV";
    else                    rText = QString::number(static_cast<int>(mvVal)) + " mV";
    int tw = p.fontMetrics().horizontalAdvance(rText);
    p.drawText(width() - m - tw, textY, rText);
}
