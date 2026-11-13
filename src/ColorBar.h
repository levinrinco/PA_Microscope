#pragma once
#include <QWidget>
#include "ColorMap.h"

class ColorBar : public QWidget {
    Q_OBJECT
public:
    explicit ColorBar(QWidget *parent = nullptr);
    void setMaxValue(int maxVal);
    void setInputRange(int rangeMv);
    void setColorMap(int colormap);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int m_maxVal = 0;
    int m_rangeMv = 1000;  // default ±1V
    int m_colorMap = 0;    // 0 = hot, 1 = grayscale
    static constexpr double kAdcFullScale = 8192.0;  // 14-bit
};
