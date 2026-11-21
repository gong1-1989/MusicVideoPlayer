#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTimer>
#include <QTranslator>

namespace {

QMutex g_logMutex;
QFile *g_logFile = nullptr;
QString g_logPath;
int g_writesSinceCheck = 0;
qint64 g_unflushedBytes = 0;
QElapsedTimer g_lastFlush;
constexpr qint64 kMaxLogBytes = 8LL * 1024 * 1024;
constexpr int kRotateCheckEvery = 512;
/// INFO/DEBUG 最多攒这么久或这么多字节再落盘；WARN/ERROR 仍然立即 flush。
constexpr int kFlushIntervalMs = 250;
constexpr qint64 kFlushBytes = 64LL * 1024;

/// 日志只保留当前文件 + 一个 .1 备份，避免长期运行把磁盘写满。
/// 轮转失败时保持原文件继续追加，不因为日志问题影响主流程。
bool rotateLog(const QString &path)
{
    const QString backup = path + QStringLiteral(".1");
    QFile::remove(backup);
    if (!QFile::rename(path, backup))
        return false;
    return true;
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const char *level = "INFO";
    bool urgent = false;
    switch (type) {
    case QtDebugMsg:
        level = "DEBUG";
        break;
    case QtInfoMsg:
        level = "INFO";
        break;
    case QtWarningMsg:
        level = "WARN";
        urgent = true;
        break;
    case QtCriticalMsg:
        level = "ERROR";
        urgent = true;
        break;
    case QtFatalMsg:
        level = "FATAL";
        urgent = true;
        break;
    }

    // 格式化在锁外完成，锁只保护文件句柄和轮转状态，网络线程不必排队拼字符串。
    QString line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    line += QStringLiteral(" [") + QLatin1String(level) + QStringLiteral("] ");
    line += message;
    if (context.file != nullptr) {
        line += QStringLiteral(" (") + QString::fromUtf8(context.file)
              + QLatin1Char(':') + QString::number(context.line) + QLatin1Char(')');
    }
    line += QLatin1Char('\n');
    const QByteArray utf8 = line.toUtf8();

    {
        QMutexLocker locker(&g_logMutex);

        if (g_logFile == nullptr) {
            // 日志放在程序目录下的 log/，不写用户 C 盘；目录不可写时退回用户数据目录
            const QString appDir = QCoreApplication::applicationDirPath();
            const QString localDir = QDir(appDir).filePath(QStringLiteral("log"));
            QDir().mkpath(localDir);
            const QString localPath =
                QDir(localDir).filePath(QStringLiteral("MusicVideoPlayer.log"));

            if (QFileInfo(localPath).size() >= kMaxLogBytes)
                rotateLog(localPath);

            g_logFile = new QFile(localPath);
            if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                delete g_logFile;
                g_logFile = nullptr;

                const QString fallbackDir =
                    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                QDir().mkpath(fallbackDir);
                const QString fallbackPath =
                    QDir(fallbackDir).filePath(QStringLiteral("MusicVideoPlayer.log"));
                if (QFileInfo(fallbackPath).size() >= kMaxLogBytes)
                    rotateLog(fallbackPath);
                g_logFile = new QFile(fallbackPath);
                if (g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
                    g_logPath = fallbackPath;
            } else {
                g_logPath = localPath;
            }
            g_lastFlush.start();
        }

        if (g_logFile != nullptr && g_logFile->isOpen() && !g_logPath.isEmpty()) {
            g_logFile->write(utf8);
            g_unflushedBytes += utf8.size();
            ++g_writesSinceCheck;

            // 长会话里也要检查轮转；先 flush 再按真实文件大小判断，避免缓冲把大小算小。
            bool rotateCheck = g_writesSinceCheck >= kRotateCheckEvery;
            if (rotateCheck) {
                g_writesSinceCheck = 0;
                if (g_logFile->size() + g_unflushedBytes >= kMaxLogBytes) {
                    g_logFile->flush();
                    g_unflushedBytes = 0;
                    g_logFile->close();
                    // 轮转失败就继续用原文件追加，不丢日志
                    rotateLog(g_logPath);
                    g_logFile->setFileName(g_logPath);
                    g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
                }
            }

            // WARN 及以上立即落盘；INFO/DEBUG 按时间或字节批量 flush，避免每个分片回调同步磁盘写。
            if (urgent || g_unflushedBytes >= kFlushBytes
                || g_lastFlush.elapsed() >= kFlushIntervalMs) {
                g_logFile->flush();
                g_unflushedBytes = 0;
                g_lastFlush.restart();
            }
        }
    }

    fprintf(stderr, "%s", line.toLocal8Bit().constData());
}

/// 退出前把 INFO/DEBUG 缓冲落盘；句柄是静态的、进程退出时不会被析构。
void flushLogFile()
{
    QMutexLocker locker(&g_logMutex);
    if (g_logFile != nullptr && g_logFile->isOpen()) {
        g_logFile->flush();
        g_unflushedBytes = 0;
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MusicVideoPlayer"));
    QCoreApplication::setApplicationName(QStringLiteral("MusicVideoPlayer"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2"));

    qInstallMessageHandler(messageHandler);
    qAddPostRoutine(flushLogFile);
    QLoggingCategory::setFilterRules(QStringLiteral("mvp.*.debug=false"));

    // 即使没有新日志，也按秒把 INFO/DEBUG 缓冲落盘，方便现场实时看日志。
    QTimer logFlushTimer;
    QObject::connect(&logFlushTimer, &QTimer::timeout, &app, [] { flushLogFile(); });
    logFlushTimer.start(1000);

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
