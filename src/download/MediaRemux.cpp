#include "download/MediaRemux.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

MediaRemux::MediaRemux(QObject *parent)
    : QObject(parent)
{
}

QString MediaRemux::ffmpegExecutable()
{
    const QString override = qEnvironmentVariable("MVP_FFMPEG_PATH");
    if (!override.isEmpty() && QFileInfo(override).isExecutable())
        return QFileInfo(override).absoluteFilePath();

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/ffmpeg/ffmpeg.exe"),
        appDir + QStringLiteral("/ffmpeg/bin/ffmpeg.exe"),
        appDir + QStringLiteral("/tools/ffmpeg/ffmpeg.exe"),
        appDir + QStringLiteral("/ffmpeg.exe"),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo(candidate).isExecutable())
            return QFileInfo(candidate).absoluteFilePath();
    }

    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

void MediaRemux::cancel()
{
    if (!m_tempPath.isEmpty())
        QFile::remove(m_tempPath);
    m_tempPath.clear();
    if (m_process == nullptr)
        return;
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    process->kill();
    process->deleteLater();
}

bool MediaRemux::isAvailable()
{
    return !ffmpegExecutable().isEmpty();
}

void MediaRemux::remuxToMp4(const QString &inputPath, const QString &outputPath)
{
    if (m_process != nullptr)
        return;

    const QString ffmpeg = ffmpegExecutable();
    if (ffmpeg.isEmpty()) {
        emit finished(false, outputPath,
                      tr("没有找到随程序安装的 FFmpeg，无法转成 MP4"));
        return;
    }

    if (!QFileInfo::exists(inputPath)) {
        emit finished(false, outputPath, tr("待转换的 TS 文件不存在"));
        return;
    }

    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    // 先写临时文件，成功后只在目标不存在时改名，绝不覆盖已有的同名 MP4。
    m_tempPath = outputPath + QStringLiteral(".mvpremux");
    QFile::remove(m_tempPath);

    m_timedOut = false;
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setWorkingDirectory(QFileInfo(ffmpeg).absolutePath());

#ifdef Q_OS_WIN
    // GUI 程序拉起控制台子进程时不要闪黑框。
    m_process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif

    connect(m_process, &QProcess::errorOccurred, this,
            [this, outputPath, tempPath = m_tempPath](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || m_process == nullptr)
                    return;
                const QString detail = m_process->errorString();
                m_process->deleteLater();
                m_process = nullptr;
                QFile::remove(tempPath);
                m_tempPath.clear();
                emit finished(false, outputPath,
                              tr("FFmpeg 启动失败：%1").arg(detail));
            });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, outputPath, tempPath = m_tempPath](int exitCode,
                                                            QProcess::ExitStatus status) {
                if (m_process == nullptr)
                    return;
                const QString log =
                    QString::fromUtf8(m_process->readAll()).trimmed();
                const bool normal = status == QProcess::NormalExit && exitCode == 0;
                const bool outputOk = QFileInfo(tempPath).exists()
                                      && QFileInfo(tempPath).size() > 0;
                bool success = normal && outputOk && !m_timedOut;

                QString error;
                if (m_timedOut)
                    error = tr("FFmpeg 转封装超时");
                else if (!normal)
                    error = tr("FFmpeg 异常退出，代码 %1").arg(exitCode);
                else if (!outputOk)
                    error = tr("FFmpeg 没有生成有效的 MP4 文件");
                else if (!log.isEmpty())
                    error = log.right(2000);

                if (success && QFileInfo::exists(outputPath)) {
                    success = false;
                    error = tr("目标 MP4 已存在，未覆盖：%1").arg(outputPath);
                } else if (success && !QFile::rename(tempPath, outputPath)) {
                    success = false;
                    error = tr("无法把临时 MP4 改名到 %1").arg(outputPath);
                }
                if (!success)
                    QFile::remove(tempPath);

                m_process->deleteLater();
                m_process = nullptr;
                m_tempPath.clear();
                emit finished(success, outputPath, error);
            });

    const QStringList arguments = {
        QStringLiteral("-y"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-i"), inputPath,
        QStringLiteral("-map"), QStringLiteral("0:v?"),
        QStringLiteral("-map"), QStringLiteral("0:a?"),
        QStringLiteral("-c"), QStringLiteral("copy"),
        QStringLiteral("-movflags"), QStringLiteral("+faststart"),
        QStringLiteral("-f"), QStringLiteral("mp4"),
        m_tempPath,
    };
    m_process->start(ffmpeg, arguments);

    QTimer::singleShot(10 * 60 * 1000, this, [this] {
        if (m_process == nullptr)
            return;
        m_timedOut = true;
        m_process->kill();
    });
}
