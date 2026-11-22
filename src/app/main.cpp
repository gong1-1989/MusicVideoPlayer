#include "ui/MainWindow.h"
#include "ui/ThemeManager.h"

#include "storage/StoragePaths.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QStyleFactory>
#include <QThread>
#include <QTimer>

namespace {

QMutex g_logMutex;
QFile *g_logFile = nullptr;
QString g_logPath;
int g_writesSinceCheck = 0;
qint64 g_unflushedBytes = 0;
qint64 g_lastOpenAttemptMs = 0;
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
    const QString stale = backup + QStringLiteral(".old");
    // 先把旧备份挪到临时名，避免“先删旧备份再 rename”在第二步失败时
    // 连最后一份可用日志也丢掉；current→.1 失败时再把旧 .1 放回去。
    QFile::remove(stale);
    const bool hadBackup = QFile::exists(backup);
    if (hadBackup && !QFile::rename(backup, stale))
        return false;
    if (!QFile::rename(path, backup)) {
        if (hadBackup)
            QFile::rename(stale, backup);
        return false;
    }
    if (hadBackup)
        QFile::remove(stale);
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
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            // 日志路径不可写时不要每条日志都尝试打开；5 秒重试一次即可。
            if (now - g_lastOpenAttemptMs >= 5000) {
                g_lastOpenAttemptMs = now;

                // 日志只放程序目录下的 log/，不再回退用户 AppData
                const QString appDir = QCoreApplication::applicationDirPath();
                const QString localDir = QDir(appDir).filePath(QStringLiteral("log"));
                QDir().mkpath(localDir);
                const QString localPath =
                    QDir(localDir).filePath(QStringLiteral("MusicVideoPlayer.log"));

                if (QFileInfo(localPath).size() >= kMaxLogBytes)
                    rotateLog(localPath);

                g_logFile = new QFile(localPath);
                if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Append
                                     | QIODevice::Text)) {
                    fprintf(stderr, "日志文件打开失败：%s\n", qPrintable(localPath));
                    delete g_logFile;
                    g_logFile = nullptr;
                } else {
                    g_logPath = localPath;
                }
                g_lastFlush.start();
            }
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
                    if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Append
                                         | QIODevice::Text)) {
                        // 重新打开失败时不能留下“句柄存在但已关闭”的假状态：
                        // 否则后续写入全部进入 isOpen()==false 分支，且再也不会重试。
                        fprintf(stderr, "日志轮转后打开失败：%s\n", qPrintable(g_logPath));
                        delete g_logFile;
                        g_logFile = nullptr;
                        g_logPath.clear();
                        g_unflushedBytes = 0;
                        g_lastOpenAttemptMs = QDateTime::currentMSecsSinceEpoch();
                    }
                }
            }

            if (g_logFile == nullptr || !g_logFile->isOpen())
                return;

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

/// 同一 Data 目录只允许一个写者；不同便携目录/测试目录互不干扰。
QString activationServerName()
{
    const QByteArray digest =
        QCryptographicHash::hash(StoragePaths::dataDir().toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    return QStringLiteral("MusicVideoPlayer-activate-") + QString::fromLatin1(digest);
}

bool notifyRunningInstance()
{
    QLocalSocket socket;
    for (int attempt = 0; attempt < 4; ++attempt) {
        socket.connectToServer(activationServerName());
        if (socket.waitForConnected(500)) {
            socket.write("activate");
            socket.flush();
            socket.waitForBytesWritten(500);
            socket.disconnectFromServer();
            return true;
        }
        qWarning() << "唤醒已有实例失败（第" << attempt + 1 << "次）："
                   << socket.errorString() << "服务名：" << activationServerName();
        socket.abort();
        QThread::msleep(150);
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MusicVideoPlayer"));
    QCoreApplication::setApplicationName(QStringLiteral("MusicVideoPlayer"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2"));

    QLoggingCategory::setFilterRules(QStringLiteral("mvp.*.debug=false"));

    if (!QDir().mkpath(StoragePaths::dataDir())) {
        fprintf(stderr, "无法创建数据目录，程序退出：%s\n",
                qPrintable(StoragePaths::dataDir()));
        return 1;
    }

    // 单实例锁必须在任何窗口/存储初始化之前拿到；两个实例各自全量读、全量写
    // playlists.json/downloads.json/sources.json，后退出者会覆盖先退出者的修改。
    QLockFile instanceLock(StoragePaths::dataFile(QStringLiteral("MusicVideoPlayer.lock")));
    if (!instanceLock.tryLock(0)) {
        // 第二实例不安装日志 handler，避免和第一实例并发写同一个日志文件。
        fprintf(stderr, "单实例锁已被占用，数据目录：%s\n",
                qPrintable(StoragePaths::dataDir()));
        if (!notifyRunningInstance()
            && QGuiApplication::platformName() != QLatin1String("offscreen")) {
            QMessageBox::information(
                nullptr, QObject::tr("MusicVideoPlayer 已在运行"),
                QObject::tr("检测到另一个实例正在使用同一数据目录。\n\n"
                            "请从任务栏切换到已运行的窗口；如果窗口没有出现，"
                            "请先结束旧的 MusicVideoPlayer 进程再启动。"));
        }
        return 0;
    }

    qInstallMessageHandler(messageHandler);
    qAddPostRoutine(flushLogFile);
    qInfo() << "单实例锁已获取，数据目录：" << StoragePaths::dataDir()
            << "激活服务名：" << activationServerName();

    // 即使没有新日志，也按秒把 INFO/DEBUG 缓冲落盘，方便现场实时看日志。
    QTimer logFlushTimer;
    QObject::connect(&logFlushTimer, &QTimer::timeout, &app, [] { flushLogFile(); });
    logFlushTimer.start(1000);

    // Fusion 对样式表的支持最完整，Windows 原生风格会覆盖掉部分 QSS 效果
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // 主题来自系统设置，样式表在资源里；加载失败保留 Fusion 默认外观
    ThemeManager::instance().applySaved();

    // 第二实例带锁失败后通过本地套接字唤醒已有窗口；服务端先于窗口构造监听，
    // 避免主窗口加载慢时第二实例误判“无法唤醒”而弹出模态提示。
    MainWindow *mainWindow = nullptr;
    QLocalServer activationServer;
    QLocalServer::removeServer(activationServerName());
    if (activationServer.listen(activationServerName()))
        qInfo() << "单实例激活服务已监听：" << activationServerName();
    else
        qWarning() << "单实例激活服务启动失败：" << activationServer.errorString()
                   << "服务名：" << activationServerName();
    QObject::connect(&activationServer, &QLocalServer::newConnection, &app, [&] {
        while (activationServer.hasPendingConnections()) {
            QLocalSocket *socket = activationServer.nextPendingConnection();
            if (socket == nullptr)
                break;
            socket->readAll();
            socket->disconnectFromServer();
            socket->deleteLater();
            if (mainWindow != nullptr) {
                mainWindow->showNormal();
                mainWindow->raise();
                mainWindow->activateWindow();
            }
        }
    });

    MainWindow window;
    mainWindow = &window;
    window.show();
    return app.exec();
}
