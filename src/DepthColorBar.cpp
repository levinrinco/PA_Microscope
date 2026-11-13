#include "DepthColorBar.h"
#include "DepthEncode.h"
#include <QPainter>

DepthColorBar::DepthColorBar(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(preferredHeight());
    setMinimumWidth(40);
    setToolTip(QObject::tr("深度编码色标：颜色 = A-line 包络峰值所在深度"
                           "（采样点序号，相对信号窗口起点）"));
}

void DepthColorBar::setRange(int maxIndex)
{
    if (maxIndex == m_maxIndex) return;
    m_maxIndex = maxIndex;
    update();
}

void DepthColorBar::setColorMap(int colorMap)
{
    if (colorMap == m_colorMap) return;
    m_colorMap = colorMap;
    update();
}

void DepthColorBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const int m = 3;               // horizontal margin
    const int barY = 2, barH = 10;
    const int barX = m, barW = width() - 2 * m;
    if (barW < 10) return;

    uint32_t lut[256];
    depthLut(m_colorMap, lut);
    for (int i = 0; i < 256; ++i) {
        const uint32_t c = lut[i];
        int x0 = barX + (i * barW) / 256;
        int x1 = barX + ((i + 1) * barW) / 256;
        if (x1 <= x0) x1 = x0 + 1;
        p.fillRect(x0, barY, x1 - x0, barH,
                   QColor(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF));
    }
    p.setPen(QColor(100, 100, 100));
    p.drawRect(barX, barY, barW, barH);

    // Labels tight against the bar edges, same style as the intensity bar.
    p.setPen(palette().color(QPalette::WindowText));
    const int textY = barY + barH + 12;
    const QString maxText = m_maxIndex > 0 ? QString::number(m_maxIndex)
                                           : QStringLiteral("depth");
    const QString unit = QObject::tr(" 采样点");
    p.drawText(barX, textY, "0");
    const QString rightText = maxText + unit;
    const int tw = p.fontMetrics().horizontalAdvance(rightText);
    p.drawText(width() - m - tw, textY, rightText);
}
