#pragma once

#include <QWidget>
#include <cstdint>

/// Horizontal color bar for depth-encoded images: the gradient is the depth
/// colormap and the labels are depth indices in samples, 0 at the left edge.
/// Deliberately separate from ColorBar (which labels mV intensity) so a depth
/// image is never mistaken for an intensity one.
class DepthColorBar : public QWidget {
    Q_OBJECT
public:
    explicit DepthColorBar(QWidget *parent = nullptr);

    /// Far end of the bar. Values are sample indices relative to the signal
    /// window start, matching the depth ruler next to the B-scan.
    void setRange(int maxIndex);
    void setColorMap(int colorMap);

    /// Height reserved for one label line, so callers can stack bars without
    /// guessing the geometry.
    static int preferredHeight() { return 28; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int m_maxIndex = 0;
    int m_colorMap = 0;
};
