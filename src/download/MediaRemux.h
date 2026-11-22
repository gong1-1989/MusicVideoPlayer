#ifndef MEDIAREMUX_H
#define MEDIAREMUX_H

#include <QObject>
#include <QString>

class QProcess;

/// 调用随程序安装的 FFmpeg，把 HLS 合并后的 TS 无损转封装成 MP4。
/// 只做 `-c copy`，不重新编码；失败时调用方保留原 TS。
class MediaRemux : public QObject
{
    Q_OBJECT

public:
    explicit MediaRemux(QObject *parent = nullptr);

    /// 优先使用程序目录 ffmpeg/ffmpeg.exe，其次 PATH；测试可用 MVP_FFMPEG_PATH 覆盖。
    static QString ffmpegExecutable();
    static bool isAvailable();

    void remuxToMp4(const QString &inputPath, const QString &outputPath);
    /// 用户移除任务/程序清理时终止正在运行的 FFmpeg，不再发 finished
    void cancel();
    /// 当前 `.mvpremux` 临时路径；用于调用方在取消后补清残留。
    QString tempFilePath() const { return m_tempPath; }

signals:
    void finished(bool success, const QString &outputPath, const QString &error);

private:
    QProcess *m_process = nullptr;
    QString m_tempPath;
    bool m_timedOut = false;
};

#endif // MEDIAREMUX_H
