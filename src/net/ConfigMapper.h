#ifndef CONFIGMAPPER_H
#define CONFIGMAPPER_H

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QUrl>
#include <QVector>

/// 配置驱动搜索源的纯数据层：
/// JSON 路径取值、模板渲染、字段映射、过滤规则、HTML 清理、地址归一、剧集拆分。
/// 这里不碰网络和 QObject，便于单元测试，也让 ConfigSearchProvider 只保留请求编排。
namespace ConfigMapper {

struct EpisodePart
{
    QString name;
    QString url;
};

/// 支持 a.b[0].c 形式的路径取值；空路径返回根节点。
QJsonValue valueAtPath(const QJsonValue &root, const QString &path);

/// 把 {name} 占位符替换为 vars 中的值。
QString renderTemplate(QString text, const QHash<QString, QString> &vars);

/// JSON 布尔/数字/字符串统一转数字，供时长等字段与数值比较使用。
double toNumber(const QJsonValue &value);

/// 过滤规则匹配：支持 any/all 组合，以及 empty/notEmpty/eq/ne/gt/gte/lt/lte/contains。
bool ruleMatches(const QJsonValue &item, const QJsonObject &rule);

/// 按 fields 里的路径或 @模板 取值。
QString mapString(const QJsonValue &node, const QHash<QString, QString> &fields,
                  const QString &key, const QHash<QString, QString> &vars);

/// 去掉接口返回文本里的 HTML 标签与常见实体。
QString cleanText(const QString &raw);

/// 年份归一：只有 1888–2999 的四位数字才保留，否则原样返回。
QString normalizeYear(const QString &raw);

/// 协议相对地址补 https、相对地址按 base 解析；空值返回空 QUrl。
QUrl normalizeUrl(const QString &raw, const QUrl &base = QUrl());

/// JSON 模板转义：引号、反斜杠、换行、制表符。
QString jsonEscape(const QString &raw);

/// 采集接口的 "名称$地址#名称$地址$$$备用源" 拆分，只取第一组播放源。
QVector<EpisodePart> parseEpisodes(const QString &raw);

} // namespace ConfigMapper

#endif // CONFIGMAPPER_H
