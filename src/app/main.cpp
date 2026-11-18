#include "ui/MainWindow.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTranslator>

namespace {

QMutex g_logMutex;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QMutexLocker locker(&g_logMutex);

    static QFile *logFile = nullptr;
    if (logFile == nullptr) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        logFile = new QFile(dir + QStringLiteral("/MusicVideoPlayer.log"));
        logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }

    const char *level = "INFO";
    switch (type) {
    case QtDebugMsg:
        level = "DEBUG";
        break;
    case QtInfoMsg:
        level = "INFO";
        break;
    case QtWarningMsg:
        level = "WARN";
        break;
    case QtCriticalMsg:
        level = "ERROR";
        break;
    case QtFatalMsg:
        level = "FATAL";
        break;
    }

    QString line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    line += QStringLiteral(" [") + QLatin1String(level) + QStringLiteral("] ");
    line += message;
    if (context.file != nullptr) {
        line += QStringLiteral(" (") + QString::fromUtf8(context.file)
              + QLatin1Char(':') + QString::number(context.line) + QLatin1Char(')');
    }
    line += QLatin1Char('\n');

    if (logFile != nullptr && logFile->isOpen()) {
        logFile->write(line.toUtf8());
        // 句柄是静态的、进程退出时不会被析构，必须每行落盘，否则日志永远是空的
        logFile->flush();
    }

    fprintf(stderr, "%s", line.toLocal8Bit().constData());
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MusicVideoPlayer"));
    QCoreApplication::setApplicationName(QStringLiteral("MusicVideoPlayer"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2"));

    qInstallMessageHandler(messageHandler);
    QLoggingCategory::setFilterRules(QStringLiteral("mvp.*.debug=false"));

    // Fusion 对样式表的支持最完整，Windows 原生风格会覆盖掉部分 QSS 效果
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFile styleFile(QStringLiteral(":/style/app.qss"));
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    } else {
        qWarning() << "style sheet missing: :/style/app.qss";
    }

    QTranslator translator;
    if (translator.load(QLocale::system(), QStringLiteral("MusicVideoPlayer"),
                        QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(&translator);
    }

    MainWindow window;
    window.show();
    return app.exec();
}
