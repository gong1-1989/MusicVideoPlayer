#ifndef HTMLTEXT_H
#define HTMLTEXT_H

#include <QString>

/// HTML 文本清洗策略。
///
/// 不同站点对实体转义的层数和 `&nbsp;` 的处理并不一致，合并实现时必须把
/// 这些差异显式保留下来，否则会改变片名/播放地址的解析结果。
struct HtmlTextPolicy
{
    int maxPasses = 1;              ///< 实体解码轮数；双层转义站点用 2
    bool decodeNbsp = false;        ///< 是否把 `&nbsp;` 还原成普通空格
    bool decodeNumeric = true;      ///< 是否还原 `&#39;` / `&#x4E07;` 等数字实体
    bool decodeApostrophe = true;   ///< 是否处理 `&apos;`
};

namespace HtmlText {

/// 按策略解码常见 HTML 实体，并可选还原数字实体。
QString decodeEntities(const QString &raw, const HtmlTextPolicy &policy = {});

/// 去掉 HTML 标签、按策略解码实体，并压缩连续空白。
QString stripTags(const QString &raw, const HtmlTextPolicy &policy = {});

} // namespace HtmlText

#endif // HTMLTEXT_H
