#include "BscanWidget.h"
#include "CscanWidget.h"
#include "DepthEncode.h"
#include <QApplication>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setVersion(2, 1);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    CscanWidget cscan;
    cscan.resize(600, 400);
    const std::vector<float> intensity = {1, 2, 3, 4, 5, 6};
    const std::vector<float> depths = {0, 1, 2, 3, 4, 5};
    cscan.updateRasterFrame(intensity, depths, 3, 2);
    if (cscan.currentMaxValue() != 6) return 1;
    cscan.updateRasterFrame({1}, depths, 3, 2);
    if (cscan.currentMaxValue() != 6) return 1;
    DepthConfig config;
    config.mode = DepthDisplayMode::Pure;
    config.maxIndex = 5;
    config.minIntensityFraction = 0;
    cscan.setWindowDepth(6);
    cscan.setDepthConfig(config);
    cscan.setDepthDisplay(true);
    BscanWidget view;
    view.resize(800, 400);
    view.setRawBscanData({-3, 0, 3, -4, 0, 4}, 3, 2);
    if (view.currentMaxValue() != 4) return 1;
    view.setRawBscanData({1}, 3, 2); // Invalid payload must not replace the image.
    if (view.currentMaxValue() != 4) return 1;
    view.setBscanData({7, 2, 1, 0}, 4, 1); // Raster still uses the same renderer.
    if (view.currentMaxValue() != 7) return 1;

    // Wider than the texture limit on many GPUs, with an odd RGB row stride.
    std::vector<double> raw(33124 * 3);
    for (int i = 0; i < 33124; ++i) {
        raw[i * 3] = -100;
        raw[i * 3 + 1] = 0;
        raw[i * 3 + 2] = 100;
    }
    view.setRawBscanData(raw, 3, 33124);
    if (view.currentMaxValue() != 100) return 1;
    if (qEnvironmentVariableIsSet("PA_TEST_OPENGL")) {
        cscan.show();
        app.processEvents();
        if (!cscan.isValid()) return 1;
        const QImage actual = cscan.grabFramebuffer();
        QImage expected;
        encodeDepthImage(depths.data(), intensity.data(), 3, 2, 6, config, expected);
        if (actual.isNull() || expected.isNull()) return 1;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x) {
                const QColor a = actual.pixelColor((2 * x + 1) * actual.width() / 6,
                                                  (2 * y + 1) * actual.height() / 4);
                const QColor e = expected.pixelColor(x, y);
                if (std::abs(a.red() - e.red()) > 3 || std::abs(a.green() - e.green()) > 3 ||
                    std::abs(a.blue() - e.blue()) > 3) {
                    std::cerr << "Polled raster C-scan depth/orientation mismatch at " << x << ',' << y
                              << ": actual=" << a.red() << ',' << a.green() << ',' << a.blue()
                              << " expected=" << e.red() << ',' << e.green() << ',' << e.blue()
                              << " framebuffer=" << actual.width() << 'x' << actual.height() << '\n';
                    return 1;
                }
            }
        cscan.hide();
        view.show();
        app.processEvents();
        if (!view.isValid()) {
            std::cerr << "No valid OpenGL context\n";
            return 1;
        }
        const QImage image = view.grabFramebuffer();
        if (image.isNull()) return 1;
        view.makeCurrent();
        if (view.context()->functions()->glGetError() != GL_NO_ERROR) return 1;
        view.doneCurrent();
        const QColor top = image.pixelColor(image.width() / 2, 0);
        const QColor middle = image.pixelColor(image.width() / 2, image.height() / 2);
        if (top.red() + top.green() + top.blue() < 100 ||
            middle.red() + middle.green() + middle.blue() > 20) {
            std::cerr << "Wide B-scan did not render with the expected depth orientation\n";
            return 1;
        }
    }
    cscan.updateRasterFrame(std::vector<float>(6, 0), std::vector<float>(6, 0), 3, 2);
    if (cscan.currentMaxValue() != 0) return 1;
    view.resetBscan();
    return view.currentMaxValue() == 0 ? 0 : 1;
}
