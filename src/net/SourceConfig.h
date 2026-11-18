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

    QString method = QStringLiteral("GET"); // GET / POST
    QString urlTemplate;                    // 支持 {keyword} {limit}
    QString bodyTemplate;                   // POST 用，占位符同上
    bool bodyIsJson = false;
    QHash<QString, QString> headers;
    QString cookie;

    QString itemsPath;                      // 例如 result.songs，空表示根就是数组
    // 字段映射：id / title / artist / durationMs / streamUrl / lyricUrl / webUrl
    // 值是 JSON 路径；以 @ 开头表示模板（@https://host/play?id={id}）
    QHash<QString, QString> fields;
    QJsonObject skipIf;                     // 规则树，空表示不过滤

    bool isValid() const;
    QString typeLabel() const;
};

QJsonObject sourceConfigToJson(const SourceConfig &config);
SourceConfig sourceConfigFromJson(const QJsonObject &object, bool *ok = nullptr);

#endif // SOURCECONFIG_H
