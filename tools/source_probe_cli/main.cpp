/// @file main.cpp
/// @brief 搜索源探测的外部解析器进程：接收站点地址，输出 SourceConfig JSON。
///
/// 该进程把原有 SourceProbe 探测规则从主程序 UI 中抽离出来；主程序通过
/// QProcess 调用它，只消费 stdout 的 JSON，不再直接持有探测逻辑。
#include "net/HttpClient.h"
#include "net/SourceConfig.h"
#include "net/SourceProbe.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

namespace {

void writeResult(const QJsonObject &object, int exitCode)
{
    QTextStream out(stdout);
    out << QJsonDocument(object).toJson(QJsonDocument::Compact) << Qt::endl;
    out.flush();
    QCoreApplication::exit(exitCode);
}

QJsonObject failure(const QString &error)
{
    QJsonObject object;
    object.insert(QStringLiteral("ok"), false);
    object.insert(QStringLiteral("error"), error);
    return object;
}

QJsonObject resultToJson(const SourceProbe::Result &result)
{
    QJsonObject object;
    object.insert(QStringLiteral("ok"), result.ok);
    object.insert(QStringLiteral("siteName"), result.siteName);
    object.insert(QStringLiteral("mediaType"),
                  result.mediaType == MediaType::Video ? QStringLiteral("video")
                                                       : QStringLiteral("music"));
    object.insert(QStringLiteral("validationNote"), result.validationNote);

    if (!result.error.isEmpty())
        object.insert(QStringLiteral("error"), result.error);
    if (result.ok)
        object.insert(QStringLiteral("source"),
                      sourceConfigToJson(result.config, true));

    QJsonArray samples;
    for (const QString &title : result.sampleTitles)
        samples.append(title);
    object.insert(QStringLiteral("samples"), samples);

    QJsonArray attempts;
    for (const SourceProbe::Attempt &attempt : result.attempts) {
        QJsonObject entry;
        entry.insert(QStringLiteral("url"), attempt.url);
        entry.insert(QStringLiteral("status"), attempt.status);
        entry.insert(QStringLiteral("note"), attempt.note);
        attempts.append(entry);
    }
    object.insert(QStringLiteral("attempts"), attempts);
    return object;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("source-probe-cli"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("探测站点并输出搜索源配置 JSON（供 MusicVideoPlayer 调用）"));
    parser.addHelpOption();
    const QCommandLineOption siteOption(
        {QStringLiteral("s"), QStringLiteral("site")},
        QStringLiteral("站点地址，例如 https://example.com/"), QStringLiteral("url"));
    const QCommandLineOption typeOption(
        {QStringLiteral("t"), QStringLiteral("type")},
        QStringLiteral("媒体类型：music 或 video"), QStringLiteral("type"),
        QStringLiteral("video"));
    const QCommandLineOption modeOption(
        {QStringLiteral("m"), QStringLiteral("mode")},
        QStringLiteral("调用场景：add 或 update"), QStringLiteral("mode"),
        QStringLiteral("add"));
    parser.addOption(siteOption);
    parser.addOption(typeOption);
    parser.addOption(modeOption);
    parser.process(app);

    QString siteText = parser.value(siteOption).trimmed();
    if (siteText.isEmpty()) {
        writeResult(failure(QStringLiteral("缺少 {site} 站点地址")), 2);
        return 2;
    }
    QUrl site(siteText);
    if (site.scheme().isEmpty())
        site = QUrl(QStringLiteral("https://") + siteText);
    if (site.scheme() != QLatin1String("http")
        && site.scheme() != QLatin1String("https")) {
        writeResult(failure(QStringLiteral("站点地址只支持 http/https")), 2);
        return 2;
    }

    const MediaType type = parser.value(typeOption).compare(
                               QStringLiteral("music"), Qt::CaseInsensitive) == 0
        ? MediaType::Music
        : MediaType::Video;

    HttpClient http;
    SourceProbe::Prober prober(&http);

    QObject::connect(&prober, &SourceProbe::Prober::finished, &app,
                     [&prober](const SourceProbe::Result &result) {
                         writeResult(resultToJson(result), result.ok ? 0 : 1);
                     });

    // 外层 QProcess 也有超时；这里再兜一层，避免进程因站点挂起而永远不退出。
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &app, [&prober] {
        prober.cancel();
        writeResult(failure(QStringLiteral("解析器探测超时")), 3);
    });
    guard.start(110000);

    prober.start(site, type, QStringList(), true);
    return app.exec();
}
