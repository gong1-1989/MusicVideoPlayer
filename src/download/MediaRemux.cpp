/// @file MediaRemux.cpp
/// @brief 调用随程序安装的 FFmpeg 做无损转封装（-c copy）。
#include "download/MediaRemux.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/// @brief 构造转码器；真正启动进程发生在 remuxToMp4()。
MediaRemux::MediaRemux(QObject *parent)
    : QObject(parent)
{
}

/// @brief 定位可用的 ffmpeg 可执行文件。
/// @return 环境变量 MVP_FFMPEG_PATH 优先，其次程序目录常见布局，最后 PATH；
///         都没有时返回空字符串。
/// @note 只检查可执行权限，不验证版本；测试可通过环境变量注入自建 FFmpeg。
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

/// @brief 终止正在运行的 FFmpeg，并异步删除 .mvpremux 临时文件。
/// @note 取消路径不能同步等待进程退出：Windows 上删除被占用的输出文件会
///       阻塞 UI。这里断开 finished 回调后 kill，由进程退出信号负责删除临时
///       文件；调用方不会收到 finished。
void MediaRemux::cancel()
{
    if (m_process != nullptr) {
        QProcess *process = m_process;
        m_process = nullptr;
        process->disconnect(this);
        const QString tempPath = m_tempPath;
        connect(process, &QProcess::finished, process,
                [process, tempPath](int, QProcess::ExitStatus) {
                    if (!tempPath.isEmpty())
                        QFile::remove(tempPath);
                    process->deleteLater();
                });
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        } else {
            if (!tempPath.isEmpty())
                QFile::remove(tempPath);
            process->deleteLater();
        }
    }
    m_tempPath.clear();
}

/// @brief 当前环境是否找得到 FFmpeg。
bool MediaRemux::isAvailable()
{
    return !ffmpegExecutable().isEmpty();
}

/// @brief 单输入转封装：把已合并的 TS/分片产物转成 MP4。
void MediaRemux::remuxToMp4(const QString &inputPath, const QString &outputPath)
{
    remuxToMp4(inputPath, QString(), outputPath);
}

/// @brief 执行 FFmpeg 无损转封装。
/// @param videoPath 输入视频文件（通常是 HLS 合并后的 TS）。
/// @param audioPath 独立音轨路径；为空时只处理视频输入自带的音轨。
/// @param outputPath 最终 MP4 路径；实际先写同目录 `.mvpremux`。
/// @note 使用 `-c copy` 不重新编码；成功后仅在目标不存在时改名，绝不覆盖
///       已有同名 MP4。10 分钟未结束会标记超时并 kill 进程。
void MediaRemux::remuxToMp4(const QString &videoPath, const QString &audioPath,
                            const QString &outputPath)
{
    if (m_process != nullptr)
        return;

    const QString ffmpeg = ffmpegExecutable();
    if (ffmpeg.isEmpty()) {
        emit finished(false, outputPath,
                      tr("没有找到随程序安装的 FFmpeg，无法转成 MP4"));
        return;
    }

    if (!QFileInfo::exists(videoPath)) {
        emit finished(false, outputPath, tr("待转换的视频文件不存在"));
        return;
    }
    if (!audioPath.isEmpty() && !QFileInfo::exists(audioPath)) {
        emit finished(false, outputPath, tr("待合并的音频文件不存在"));
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

    QStringList arguments = {
        QStringLiteral("-y"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-i"), videoPath,
    };
    if (!audioPath.isEmpty()) {
        arguments << QStringLiteral("-i") << audioPath
                  << QStringLiteral("-map") << QStringLiteral("0:v:0?")
                  << QStringLiteral("-map") << QStringLiteral("1:a:0?");
    } else {
        arguments << QStringLiteral("-map") << QStringLiteral("0:v:0?")
                  << QStringLiteral("-map") << QStringLiteral("0:a:0?");
    }
    arguments << QStringLiteral("-c") << QStringLiteral("copy")
              << QStringLiteral("-movflags") << QStringLiteral("+faststart")
              << QStringLiteral("-f") << QStringLiteral("mp4")
              << m_tempPath;
    m_process->start(ffmpeg, arguments);

    QTimer::singleShot(10 * 60 * 1000, this, [this] {
        if (m_process == nullptr)
            return;
        // 进程已经结束但 finished 信号还没排队处理时，不能再标“超时”并 kill；
        // 否则一次正常完成的转码会在 10 分钟边界被误判为失败、删掉有效输出。
        if (m_process->state() == QProcess::NotRunning)
            return;
        m_timedOut = true;
        m_process->kill();
    });
}
