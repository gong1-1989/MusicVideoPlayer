#ifndef MACCMSWEBPROVIDER_H
#define MACCMSWEBPROVIDER_H

#include "net/SearchProviderBase.h"
#include "net/SourceConfig.h"

#include <QString>
#include <QStringList>
#include <QSharedPointer>
#include <QUrl>
#include <QVector>

#include <functional>

class HttpClient;

/// 内置网页型影视源：面向只提供网页、没有开放接口的采集站（苹果CMS 系为主）。
/// 站点之间的结构差异全部收敛在"模板"上——搜索入口、详情路径、播放路径——
/// 所以同一份实现能同时覆盖 /video/<id>.html 系和 /voddetail/<id>.html 系。
///
/// 搜索入口按四种策略依次降级；搜索命中必须回到详情页验真（大量站点对未知
/// 地址直接回首页，会造成"命中上百条"的假象）；播放页只认三种形态：
/// 明文直链（可播）、地址加密（明确说明原因）、第三方解析（明确说明原因）。
class MaccmsWebProvider : public SearchProviderBase
{
    Q_OBJECT

public:
    MaccmsWebProvider(const SourceConfig &config, HttpClient *http, QObject *parent = nullptr);

    void search(const QString &keyword, int limit) override;
    void abort() override;

    /// 探测阶段只验证“搜索页确实按关键词返回影片链接”，
    /// 命中候选后立即结束，不抓详情页和播放页。
    void setValidationMode(bool enabled) { m_validationMode = enabled; }

private:
    void finishCandidateValidation();
    struct Candidate
    {
        QString title;
        QUrl detailUrl;
        QString poster;
        QString year;
        QString area;
        QString genre;
        QString description;
        /// 影视站 vod_remarks（HD、1080P、更新至…）归一化后的品质标签
        QString quality;
        QStringList playLines; // 详情页里每条线路的第一集（按线路顺序）
    };

    /// 一个候选的完整抓取链：详情页 → 若干播放页 → 一条 MediaItem。
    /// 并发抓取时每个 job 保存自己的线路进度；provider 不保存“当前候选”，
    /// 避免多个 job 共享可变游标互相踩。
    struct Job
    {
        int index = 0;
        Candidate candidate;
        int lineIndex = 0;
        QString firstFailure;
    };

    void fetchRoot();
    void trySuggest();
    void trySearch(int strategy);
    void afterSearch(const QString &html, int strategy);
    void nextStrategyOrFinish();
    void startWorkers();
    void processJob(const QSharedPointer<Job> &job);
    void processPlay(const QSharedPointer<Job> &job);
    void finishJob(const QSharedPointer<Job> &job, bool ok, const MediaItem &item = MediaItem(),
                   const QString &reason = QString());
    void finish(const QString &error);

    QUrl siteUrl(const QString &relative) const;
    QHash<QString, QString> requestHeaders() const;
    QHash<QString, QString> mediaHeaders() const;
    void requestHtml(const QUrl &url, const std::function<void(bool, const QString &)> &callback);

    const SourceConfig m_config;
    HttpClient *m_http = nullptr;

    QString m_keyword;
    QString m_keywordEncoded;
    int m_limit = 10;
    int m_rootLength = 0;
    QString m_root; // 站点根，保证以 / 结尾

    QStringList m_searchErrors; // 每个搜索策略的失败原因，整源无结果时汇总上报
    QVector<Candidate> m_candidates;
    /// 并发抓取时按候选序号暂存，最后统一排序，避免结果顺序由“谁先抓完”决定
    QVector<QPair<int, MediaItem>> m_slots;
    QVector<MediaItem> m_items;
    QString m_firstFailure; // 第一条条目的失败原因，用于整源提示

    int m_filtered = 0;
    int m_nextCursor = 0;
    int m_active = 0;
    bool m_done = false;
    bool m_suggestUsed = false;      // 本次结果来自 ajax/suggest
    bool m_htmlSearchTried = false;  // 已经试过 HTML 搜索链（防止 suggest 失败后反复回退）
    bool m_validationMode = false;   // 探测用：只验搜索命中，不抓详情/播放
};

#endif // MACCMSWEBPROVIDER_H
