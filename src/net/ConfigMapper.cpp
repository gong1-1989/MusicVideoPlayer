/// @file ConfigMapper.cpp
/// @brief 配置驱动搜索源的 JSON 映射纯函数实现。
#include "net/ConfigMapper.h"

#include <QJsonArray>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStringList>

Q_LOGGING_CATEGORY(lcMapper, "mvp.net.mapper")

namespace {
// 规则来自可编辑配置/远端 JSON，any/all 可以无限互相嵌套；限制深度，
// 避免恶意或误配规则把递归栈打爆。
constexpr int kMaxRuleDepth = 64;
} // namespace

namespace ConfigMapper {

/// @brief 把 JSON 布尔/数字/字符串统一转成 double。
/// @return 无法转换或类型不支持时返回 0.0。
double toNumber(const QJsonValue &value)
{
    if (value.isDouble())
        return value.toDouble();
    if (value.isBool())
        return value.toBool() ? 1.0 : 0.0;
    if (value.isString())
        return value.toString().toDouble();
    return 0.0;
}

/// @brief 按 a.b[0].c 形式从 JSON 取值。
/// @param root 根节点。
/// @param path 路径表达式；空路径返回根节点。
/// @return 命中值；对象/数组结构不符或下标越界返回 Undefined。
/// @note 只支持点号与数字下标，不支持通配符/负下标，避免配置规则过于任意。
QJsonValue valueAtPath(const QJsonValue &root, const QString &path)
{
    if (path.trimmed().isEmpty())
        return root;

    QJsonValue current = root;
    QString rest = path.trimmed();

    while (!rest.isEmpty()) {
        if (!rest.startsWith(QLatin1Char('['))) {
            int dot = rest.indexOf(QLatin1Char('.'));
            int bracket = rest.indexOf(QLatin1Char('['));
            int stop = dot;
            if (bracket >= 0 && (stop < 0 || bracket < stop))
                stop = bracket;
            const QString key = stop < 0 ? rest : rest.left(stop);
            if (!current.isObject())
                return QJsonValue();
            current = current.toObject().value(key);
            if (stop < 0)
                return current;
            rest = rest.mid(stop);
        }
        if (rest.startsWith(QLatin1Char('['))) {
            int close = rest.indexOf(QLatin1Char(']'));
            if (close < 0)
                return QJsonValue();
            bool ok = false;
            const int index = rest.mid(1, close - 1).toInt(&ok);
            if (!ok || !current.isArray())
                return QJsonValue();
            const QJsonArray array = current.toArray();
            if (index < 0 || index >= array.size())
                return QJsonValue();
            current = array.at(index);
            rest = rest.mid(close + 1);
        }
        if (rest.startsWith(QLatin1Char('.')))
            rest = rest.mid(1);
    }
    return current;
}

/// @brief 把 {name} 占位符替换为 vars 中的值。
/// @param text 模板文本。
/// @param vars 变量名到值的映射。
/// @return 替换后的文本；未知占位符保持原样。
QString renderTemplate(QString text, const QHash<QString, QString> &vars)
{
    for (auto it = vars.cbegin(); it != vars.cend(); ++it) {
        text.replace(QStringLiteral("{") + it.key() + QStringLiteral("}"), it.value());
    }
    return text;
}

/// @brief 递归匹配过滤规则。
/// @param item 当前待匹配的结果节点。
/// @param rule 规则对象：any/all 组合或 {path,op,value} 叶子。
/// @param depth 当前递归深度，超过 kMaxRuleDepth 按不匹配处理。
/// @return 规则为空或结构不支持时返回 false；all 要求全部子规则通过，
///         any 要求至少一条通过。
bool ruleMatchesAtDepth(const QJsonValue &item, const QJsonObject &rule, int depth)
{
    if (rule.isEmpty())
        return false;
    if (depth > kMaxRuleDepth) {
        qCWarning(lcMapper) << "规则嵌套超过" << kMaxRuleDepth << "层，按不匹配处理";
        return false;
    }

    if (rule.contains(QStringLiteral("any"))) {
        const QJsonArray any = rule.value(QStringLiteral("any")).toArray();
        for (const QJsonValue &entry : any) {
            if (entry.isObject() && ruleMatchesAtDepth(item, entry.toObject(), depth + 1))
                return true;
        }
        return false;
    }
    if (rule.contains(QStringLiteral("all"))) {
        const QJsonArray all = rule.value(QStringLiteral("all")).toArray();
        for (const QJsonValue &entry : all) {
            if (!entry.isObject()
                || !ruleMatchesAtDepth(item, entry.toObject(), depth + 1))
                return false;
        }
        return true;
    }

    const QString path = rule.value(QStringLiteral("path")).toString();
    const QString op = rule.value(QStringLiteral("op")).toString(QStringLiteral("eq"));
    const QJsonValue expected = rule.value(QStringLiteral("value"));
    const QJsonValue actual = valueAtPath(item, path);

    if (op == QLatin1String("empty"))
        return actual.isUndefined() || actual.isNull()
               || (actual.isString() && actual.toString().isEmpty());
    if (op == QLatin1String("notEmpty"))
        return !(actual.isUndefined() || actual.isNull()
                 || (actual.isString() && actual.toString().isEmpty()));

    if (op == QLatin1String("eq") || op == QLatin1String("ne")) {
        bool equal = false;
        // bool 不能和数字混算：true == false == 0 会把 audius 的
        // “is_streamable eq false” 误判成全部命中，20 条结果全被过滤。
        if (expected.isBool() || actual.isBool()) {
            equal = expected.isBool() && actual.isBool()
                    && expected.toBool() == actual.toBool();
        } else if (expected.isDouble() || actual.isDouble()) {
            equal = toNumber(actual) == toNumber(expected);
        } else {
            equal = actual.toString() == expected.toString();
        }
        return op == QLatin1String("eq") ? equal : !equal;
    }
    if (op == QLatin1String("gt"))
        return toNumber(actual) > toNumber(expected);
    if (op == QLatin1String("gte"))
        return toNumber(actual) >= toNumber(expected);
    if (op == QLatin1String("lt"))
        return toNumber(actual) < toNumber(expected);
    if (op == QLatin1String("lte"))
        return toNumber(actual) <= toNumber(expected);
    if (op == QLatin1String("contains"))
        return actual.toString().contains(expected.toString(), Qt::CaseInsensitive);

    qCWarning(lcMapper) << "未知的过滤操作符：" << op;
    return false;
}

/// @brief 从根节点开始匹配过滤规则。
bool ruleMatches(const QJsonValue &item, const QJsonObject &rule)
{
    return ruleMatchesAtDepth(item, rule, 0);
}

/// @brief 按字段配置从节点取值。
/// @param node 单条结果节点。
/// @param fields 字段名到“JSON 路径或 @模板”的映射。
/// @param key 要读取的字段名。
/// @param vars 模板变量。
/// @return 取到的字符串；字段未配置、路径不存在或模板为空时返回空。
QString mapString(const QJsonValue &node, const QHash<QString, QString> &fields,
                  const QString &key, const QHash<QString, QString> &vars)
{
    const QString spec = fields.value(key);
    if (spec.trimmed().isEmpty())
        return QString();
    if (spec.startsWith(QLatin1Char('@')))
        return renderTemplate(spec.mid(1), vars);
    return valueAtPath(node, spec).toVariant().toString();
}

/// @brief 清理接口文本：去 HTML 标签、解码常见实体并压缩空白。
QString cleanText(const QString &raw)
{
    QString text = raw;
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    return text.simplified();
}

/// @brief 年份归一：只接受 1888..2999 的四位数字，否则保留原文本。
QString normalizeYear(const QString &raw)
{
    const QString text = cleanText(raw);
    const QString head = text.left(4);
    bool ok = false;
    const int value = head.toInt(&ok);
    if (ok && value >= 1888 && value <= 2999)
        return head;
    return text;
}

/// @brief 归一化远程数据里的媒体地址。
/// @param raw 原始地址，可以是协议相对地址或站点相对路径。
/// @param base 用于解析相对路径的站点/接口地址。
/// @return 仅 http/https 的绝对地址；其他协议（file/UNC/smb 等）返回空。
QUrl normalizeUrl(const QString &raw, const QUrl &base)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return QUrl();
    QUrl url = text.startsWith(QStringLiteral("//")) ? QUrl(QStringLiteral("https:") + text)
                                                      : QUrl(text);
    if (url.isRelative() && !url.isEmpty() && base.isValid())
        url = base.resolved(url);
    // 配置源返回的是远程数据，不能让远程 JSON 直接指定 file/UNC/smb 等本地资源。
    // 允许的相对地址已经按 base 解析成 http(s)；这里再做一次白名单兜底。
    if (!url.isValid()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
        return QUrl();
    }
    return url;
}

/// @brief 转义 JSON 模板中的引号、反斜杠、换行和制表符。
QString jsonEscape(const QString &raw)
{
    QString text = raw;
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    text.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    text.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return text;
}

/// @brief 拆分采集接口的 “名称$地址#名称$地址$$$备用线路” 文本。
/// @param raw 原始剧集字段。
/// @return 主线路剧集列表；每个 EpisodePart 的 altUrls 保存备用地址。
/// @note `$$$` 第一组是主线路，后面的组按“同名剧集优先、否则同序号”归并，
///       避免多 CDN/多清晰度线路被截断丢失。
QVector<EpisodePart> parseEpisodes(const QString &raw)
{
    QVector<EpisodePart> parts;
    const QString body = raw.trimmed();
    if (body.isEmpty())
        return parts;

    // `$$$` 是采集接口的多线路分隔符：第一组是主线路，后面是备用线路。
    // 备用线路按“同名剧集优先、否则同序号”归到对应 EpisodePart，播放时再决定主/备。
    const QStringList sources = body.split(QStringLiteral("$$$"), Qt::SkipEmptyParts);
    for (int sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        const QStringList groups = sources.at(sourceIndex).split(QLatin1Char('#'), Qt::SkipEmptyParts);
        for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            const QString trimmed = groups.at(groupIndex).trimmed();
            if (trimmed.isEmpty())
                continue;
            const int dollar = trimmed.indexOf(QLatin1Char('$'));
            const QString name = dollar > 0 ? trimmed.left(dollar).trimmed() : QString();
            const QString url = dollar >= 0 ? trimmed.mid(dollar + 1).trimmed() : trimmed;
            if (url.isEmpty())
                continue;

            if (sourceIndex == 0) {
                parts.append(EpisodePart{name, url, QStringList()});
                continue;
            }

            int target = -1;
            if (!name.isEmpty()) {
                for (int i = 0; i < parts.size(); ++i) {
                    if (parts.at(i).name == name) {
                        target = i;
                        break;
                    }
                }
            }
            if (target < 0 && groupIndex < parts.size())
                target = groupIndex;
            if (target >= 0 && parts.at(target).url != url
                && !parts.at(target).altUrls.contains(url)) {
                parts[target].altUrls.append(url);
            }
        }
    }
    return parts;
}

} // namespace ConfigMapper
