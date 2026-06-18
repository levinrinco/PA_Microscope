#include <QApplication>
#include <QSurfaceFormat>
#include <QMessageBox>
#include "MainWindow.h"
#include "ColorBar.h"
#include "Logger.h"

int main(int argc, char *argv[])
{
    // Use OpenGL 2.1 compatibility profile for legacy glBegin/glEnd
    QSurfaceFormat fmt;
    fmt.setVersion(2, 1);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("光声显微镜数据采集控制软件");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("PAM_Lab");

    Logger::init(QCoreApplication::applicationDirPath() + "/logs");
    initColorMap();

    qInfo() << "App start, Qt" << QT_VERSION_STR;

    MainWindow w;
    w.resize(1400, 900);
    w.show();

    return app.exec();
}
