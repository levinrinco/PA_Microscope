#pragma once
#include <QWidget>
#include <cstdint>

extern uint32_t hotLUT[256];
void initColorMap();

class ColorBar : public QWidget {
    Q_OBJECT
public:
    explicit ColorBar(QWidget *parent = nullptr);
    void setMaxValue(int maxVal);
    void setInputRange(int rangeMv);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int m_maxVal = 0;
    int m_rangeMv = 1000;  // default ±1V
    static constexpr double kAdcFullScale = 8192.0;  // 14-bit
};
