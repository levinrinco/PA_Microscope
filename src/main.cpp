#include <QApplication>
#include <QSurfaceFormat>
#include <QMessageBox>
#include <QAbstractSpinBox>
#include <QEvent>
#include "MainWindow.h"
#include "ColorBar.h"
#include "Logger.h"

namespace {

// All spin boxes (and their internal line edits) ignore mouse-wheel events so
// scrolling over a control never accidentally changes a value. Keyboard
// (arrows / typed input) still works as usual.
class SpinBoxWheelFilter : public QObject {
public:
    explicit SpinBoxWheelFilter(QObject *parent) : QObject(parent) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Wheel) {
            if (qobject_cast<QAbstractSpinBox *>(watched) ||
                qobject_cast<QAbstractSpinBox *>(watched->parent()))
                return true;   // consume the event
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

int main(int argc, char *argv[])
{
    // Use OpenGL 2.1 compatibility profile for legacy glBegin/glEnd
    QSurfaceFormat fmt;
    fmt.setVersion(2, 1);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    // applicationName is an ASCII identifier: the desktop shell (GNOME) shows
    // it in the top bar / notifications, and a non-ASCII value gets mangled to
    // Latin-1 there (UTF-8 read as Latin-1). The Chinese display name is the
    // window title (set in MainWindow.ui), which is correctly UTF-8.
    app.setApplicationName("PA_Microscope");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("PAM_Lab");

    app.installEventFilter(new SpinBoxWheelFilter(&app));

    Logger::init(QCoreApplication::applicationDirPath() + "/logs");
    initColorMap();

    qInfo() << "App start, Qt" << QT_VERSION_STR;

    MainWindow w;
    w.resize(1400, 900);
    w.show();

    return app.exec();
}
