#ifndef SOURCEPROBE_H
#define SOURCEPROBE_H

#include "media/MediaItem.h"
#include "net/SourceConfig.h"

#include <QElapsedTimer>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

class HttpClient;
class QNetworkReply;
class ConfigSearchProvider;

/// 网站探测：用户只给一个网址，程序试几条常见接口路径，
/// 从返回的 JSON 里反推「结果路径 + 字段对应 + 播放地址形态」，再试搜一次做校验。
/// 命中不了就如实失败，不猜、不半成品落库。
namespace SourceProbe {

struct Attempt
{
    QString url;
    int status = 0;
    QString note;
};

/// 内置的常见接口路径（界面上只读展示，探测时作为兜底候选）
QStringList defaultPaths(MediaType mediaType);

struct Result
{
    bool ok = false;
    MediaType mediaType = MediaType::Music;   ///< 识别出来的类型
    MediaType hintType = MediaType::Music;    ///< 用户选的类型（用来判断要不要自动切换）
    bool autoDetected = false;                ///< 用户没选，或识别结果与所选不同
    QString siteName;            ///< 站点标题（取不到时用 host）
    QUrl siteUrl;                ///< 重定向后的站点地址
    SourceConfig config;         ///< 建议配置（enabled=false，等用户确认后写入）
    QStringList sampleTitles;    ///< 试搜到的前几条标题
    int sampleCount = 0;
    QString error;               ///< 失败原因
    QVector<Attempt> attempts;   ///< 探测记录，失败时展示给用户
};

class Prober : public QObject
{
    Q_OBJECT

public:
    explicit Prober(HttpClient *http, QObject *parent = nullptr);
    ~Prober() override;

    /// hintType 是用户的选择；autoDetect=true 时会把另一种类型的候选路径也试一遍，
    /// 并按返回数据自动判定到底是影视还是音乐（选错也会纠正）。
    void start(const QUrl &site, MediaType hintType,
               const QStringList &extraPaths = QStringList(), bool autoDetect = false);
    void cancel();

signals:
    void finished(const SourceProbe::Result &result);

private:
    void fetchHome();
    void probeNextCandidate();
    void analyze(const QUrl &url, const QJsonValue &root, int status);
    void validate(const SourceConfig &config, const QString &keyword);
    void fail(const QString &error);
    /// 把失败归类成一句用户能看懂、能据此行动的原因
    QString diagnoseFailure() const;
    void done(const Result &result);

    bool budgetLeft() const;
    void recordAttempt(const QString &url, int status, const QString &note);

    HttpClient *m_http;
    MediaType m_type = MediaType::Music;
    bool m_auto = false;
    QUrl m_site;
    QUrl m_origin;          ///< 用户输入的域名，候选接口优先拼在它上面
    QUrl m_redirectOrigin;  ///< 首页跳转后的域名，仅作兜底
    QString m_siteName;

    QStringList m_candidates;
    QVector<MediaType> m_candidateTypes;
    QStringList m_extraPaths;
    int m_candidateIndex = 0;

    // 失败归类：用来把“为什么没探到接口”说清楚，而不是笼统一句话
    bool m_homeOk = false;      ///< 首页是否打得开
    int m_connFail = 0;         ///< 连不上（DNS/超时/被断连）
    int m_denied = 0;           ///< 403/401：拒绝程序访问
    int m_notFound = 0;         ///< 404：路径不存在
    int m_notJson = 0;          ///< 200 但返回网页而非 JSON

    QElapsedTimer m_clock;
    QPointer<QNetworkReply> m_activeReply;
    QPointer<ConfigSearchProvider> m_validator;

    Result m_result;
    bool m_finished = false;
};

} // namespace SourceProbe

Q_DECLARE_METATYPE(SourceProbe::Result)

#endif // SOURCEPROBE_H
