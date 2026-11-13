#pragma once

#include <QWidget>

// A thin vertical depth ruler that lives between the A-line and B-scan
// widgets. Depth 0 is at the TOP and increases downward, matching the
// B-scan depth axis. Always visible (not dependent on widget width).
class DepthRulerWidget : public QWidget {
    Q_OBJECT
public:
    explicit DepthRulerWidget(QWidget *parent = nullptr);

    void setDepth(int depth);
    int  depth() const { return m_depth; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int m_depth = 600;
};
