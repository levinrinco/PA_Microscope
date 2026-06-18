#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <vector>
#include <mutex>

class AlineWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit AlineWidget(QWidget *parent = nullptr);

public slots:
    void setAlineData(const std::vector<double> &envelope);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    std::vector<double> m_data;
    std::mutex m_dataMutex;
    int m_viewW = 1;
    int m_viewH = 1;
};
