#include "util/HtmlText.h"

#include <QRegularExpression>

namespace HtmlText {

QString decodeEntities(const QString &raw, const HtmlTextPolicy &policy)
{
    if (raw.isEmpty())
        return raw;

    static const QRegularExpression numeric(
        QStringLiteral("&#(?:[xX]([0-9a-fA-F]{1,6})|([0-9]{1,7}));"));
    QString text = raw;
    const int passes = qMax(1, policy.maxPasses);
    for (int pass = 0; pass < passes; ++pass) {
        const QString before = text;
        text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
        if (policy.decodeApostrophe)
            text.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
        text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
        text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
        if (policy.decodeNbsp)
            text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
        text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));

        if (policy.decodeNumeric) {
            QString decoded;
            int last = 0;
            QRegularExpressionMatchIterator it = numeric.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                bool ok = false;
                const uint code = match.captured(1).isEmpty()
                                      ? match.captured(2).toUInt(&ok, 10)
                                      : match.captured(1).toUInt(&ok, 16);
                const bool valid = ok && code > 0 && code <= 0x10FFFF
                                   && !(code >= 0xD800 && code <= 0xDFFF);
                decoded += text.mid(last, match.capturedStart() - last);
                if (valid) {
                    const char32_t codePoint = static_cast<char32_t>(code);
                    decoded += QString::fromUcs4(&codePoint, 1);
                } else {
                    decoded += match.captured(0);
                }
                last = match.capturedEnd();
            }
            decoded += text.mid(last);
            text = decoded;
        }
        if (text == before)
            break;
    }
    return text;
}

QString stripTags(const QString &raw, const HtmlTextPolicy &policy)
{
    QString text = raw;
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    return decodeEntities(text, policy).simplified();
}

} // namespace HtmlText
