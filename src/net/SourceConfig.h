#ifndef SOURCECONFIG_H
#define SOURCECONFIG_H

#include "media/MediaItem.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/// 一个搜索源的完整描述。内置源与用户自定义源共用同一结构，
/// 区别只有 builtin 标记：内置源可停用、可复制为新源，但不能删除。
struct SourceConfig
{
    QString id;
    QString name;
    bool enabled = true;
    bool builtin = false;
    MediaType mediaType = MediaType::Music;

    /// config   = 纯配置驱动（JSON 接口）
    /// builtin  = 编进程序的抓取实现（如网页型影视源，只解析站点页面）
    /// external = 外部解析器进程（自己写脚本算签名/爬取，stdout 吐 JSON）
    QString kind = QStringLiteral("config");
    QString provider;                       // kind=builtin 时用，例如 maccms_web

    QString method = QStringLiteral("GET"); // GET / POST
    QString urlTemplate;                    // 支持 {keyword} {limit}
    QString bodyTemplate;                   // POST 用，占位符同上
    bool bodyIsJson = false;
    QHash<QString, QString> headers;
    QString cookie;

    QString command;                        // kind=external：可执行文件
    QString arguments;                      // 支持 {keyword} {limit}
    /// 0 表示按源类型/条数自动分配预算；大于 0 时所有源类型都按该值限时。
    int timeoutMs = 0;

    QString itemsPath;                      // 例如 result.songs，空表示根就是数组
    // 字段映射：id / title / artist / durationMs / streamUrl / lyricUrl / webUrl
    //          posterUrl / year / area / genre / description
    // 值是 JSON 路径；以 @ 开头表示模板（@https://host/play?id={id}）
    QHash<QString, QString> fields;

    /// 播放地址的形态：
    /// direct    = 一个字段就是一条直链
    /// episodes  = 一个字段里装了整部剧的剧集（采集接口的 "名称$地址#名称$地址" 格式），
    ///             会按剧集拆成多条结果
    QString streamKind = QStringLiteral("direct");
    QJsonObject skipIf;                     // 规则树，空表示不过滤

    // ---------------------------------------------------------- 二次解析
    // 有些接口搜索结果里没有直链，要拿 id 再请求一次详情/播放接口才给流地址。
    // 配置了 detailUrlTemplate 就走这条路：搜索命中 → 详情请求 → 取直链。
    QString detailUrlTemplate;              // 支持 {id} {keyword} {title} {artist}
    QString detailItemsPath;                // 详情 JSON 里流地址所在的路径，空表示根
    QHash<QString, QString> detailFields;   // 通常只需要 streamUrl（@ 模板同 fields）

    bool needsDetailFetch() const
    {
        return !detailUrlTemplate.trimmed().isEmpty();
    }

    bool isValid() const;
    /// 配置不完整时给出具体原因（界面标红和悬停提示用），有效则返回空
    QString validationError() const;
    QString typeLabel() const;
    QString kindLabel() const;
};

QJsonObject sourceConfigToJson(const SourceConfig &config);
SourceConfig sourceConfigFromJson(const QJsonObject &object, bool *ok = nullptr);

#endif // SOURCECONFIG_H
