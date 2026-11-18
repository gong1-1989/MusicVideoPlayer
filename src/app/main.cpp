#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
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
        // 日志放在程序目录下的 log/，不写用户 C 盘；目录不可写时退回用户数据目录
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString localDir = QDir(appDir).filePath(QStringLiteral("log"));
        QDir().mkpath(localDir);
        const QString localPath = QDir(localDir).filePath(QStringLiteral("MusicVideoPlayer.log"));

        logFile = new QFile(localPath);
        if (!logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            delete logFile;
            logFile = nullptr;

            const QString fallbackDir =
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QDir().mkpath(fallbackDir);
            logFile = new QFile(QDir(fallbackDir).filePath(QStringLiteral("MusicVideoPlayer.log")));
            logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        }
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

    // 主题来自系统设置，样式表在资源里；加载失败保留 Fusion 默认外观
    ThemeManager::instance().applySaved();

    QTranslator translator;
    if (translator.load(QLocale::system(), QStringLiteral("MusicVideoPlayer"),
                        QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        QCoreApplication::installTranslator(&translator);
    }

    MainWindow window;
    window.show();
    return app.exec();
}
