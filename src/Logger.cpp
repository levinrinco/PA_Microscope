#include "Logger.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <cstdio>

static QFile s_logFile;
static QMutex s_mutex;
static bool s_inited = false;
static QtMessageHandler s_oldHandler = nullptr;

static void logHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    QMutexLocker lock(&s_mutex);

    QString level;
    switch (type) {
    case QtDebugMsg:    level = "D"; break;
    case QtInfoMsg:     level = "I"; break;
    case QtWarningMsg:  level = "W"; break;
    case QtCriticalMsg: level = "C"; break;
    case QtFatalMsg:    level = "F"; break;
    }

    QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString line = QString("[%1 %2] %3\n")
        .arg(ts, level, msg);

    if (s_logFile.isOpen()) {
        (void)s_logFile.write(line.toUtf8());
        s_logFile.flush();
    }

    // Also print to stderr for debugger visibility
    fprintf(stderr, "%s", qPrintable(line));

    if (s_oldHandler)
        s_oldHandler(type, ctx, msg);

    if (type == QtFatalMsg)
        abort();
}

namespace Logger {

void init(const QString &logDir)
{
    if (s_inited) return;

    QDir().mkpath(logDir);
    QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString path = logDir + "/PA_Microscope_" + ts + ".txt";
    s_logFile.setFileName(path);
    (void)s_logFile.open(QIODevice::WriteOnly | QIODevice::Append);

    s_oldHandler = qInstallMessageHandler(logHandler);
    s_inited = true;

    qInfo() << "=== Logger started ===";
    qInfo() << "Log:" << path;
    qInfo() << "Exe:" << QCoreApplication::applicationDirPath();
}

} // namespace Logger
