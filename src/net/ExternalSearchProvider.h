#ifndef EXTERNALSEARCHPROVIDER_H
#define EXTERNALSEARCHPROVIDER_H

#include "net/SearchProviderBase.h"
#include "net/SourceConfig.h"

#include <QObject>
#include <QProcess>
#include <QString>

class QTimer;

/// 外部解析器源：把关键词交给一个外部进程（脚本/小工具），由它自己算签名、抓页面，
/// 最后在 stdout 吐 JSON。这样进程崩溃不会带崩播放器，算法升级只换一个文件，
/// 也不需要和主程序对齐 Qt/编译器版本。
///
/// stdout 支持两种形态：
///   [ {title, artist, durationMs, streamUrl, webUrl, lyricUrl}, ... ]
///   { "items": [...], "filtered": 3 }
class ExternalSearchProvider : public SearchProviderBase
{
    Q_OBJECT

public:
    explicit ExternalSearchProvider(const SourceConfig &config, QObject *parent = nullptr);

    void search(const QString &keyword, int limit) override;
    void abort() override;

private:
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onFailedToStart();
    void onTimeout();
    void finish(const QString &error);
    void parseOutput(const QByteArray &output);

    SourceConfig m_config;
    QProcess *m_process = nullptr;
    QTimer *m_timer = nullptr;

    int m_filtered = 0;
    bool m_done = false;
};

#endif // EXTERNALSEARCHPROVIDER_H
