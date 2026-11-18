#include "lyrics/LrcParser.h"

#include <QString>
#include <QtTest>

class TestLrcParser : public QObject
{
    Q_OBJECT

private slots:
    void emptyInput();
    void parsesBasicLines();
    void supportsMultipleTimestampsPerLine();
    void normalizesFractionDigits();
    void appliesOffsetTag();
    void ignoresMetadataAndPlainText();
    void sortsOutOfOrderInput();
    void indexLookup();
};

void TestLrcParser::emptyInput()
{
    QVERIFY(LrcParser::parse(QString()).isEmpty());
    QVERIFY(LrcParser::parseBytes(QByteArray()).isEmpty());
}

void TestLrcParser::parsesBasicLines()
{
    const QString lrc = QStringLiteral("[00:01.00]第一句\n[00:03.50]第二句\n");
    const QVector<LrcLine> lines = LrcParser::parse(lrc);

    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0).timeMs, qint64(1000));
    QCOMPARE(lines.at(0).text, QStringLiteral("第一句"));
    QCOMPARE(lines.at(1).timeMs, qint64(3500));
    QCOMPARE(lines.at(1).text, QStringLiteral("第二句"));
}

void TestLrcParser::supportsMultipleTimestampsPerLine()
{
    const QString lrc = QStringLiteral("[00:01.00][00:05.00]重复一句");
    const QVector<LrcLine> lines = LrcParser::parse(lrc);

    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0).timeMs, qint64(1000));
    QCOMPARE(lines.at(1).timeMs, qint64(5000));
    QCOMPARE(lines.at(0).text, QStringLiteral("重复一句"));
    QCOMPARE(lines.at(1).text, QStringLiteral("重复一句"));
}

void TestLrcParser::normalizesFractionDigits()
{
    const QVector<LrcLine> lines = LrcParser::parse(
        QStringLiteral("[00:01.5]十分之一秒\n[00:01.05]百分之一秒\n[00:01.005]毫秒\n"));

    QCOMPARE(lines.size(), 3);
    // 解析结果按时间升序排列，因此 .005 < .05 < .5
    QCOMPARE(lines.at(0).timeMs, qint64(1005));
    QCOMPARE(lines.at(1).timeMs, qint64(1050));
    QCOMPARE(lines.at(2).timeMs, qint64(1500));
}

void TestLrcParser::appliesOffsetTag()
{
    const QVector<LrcLine> lines = LrcParser::parse(
        QStringLiteral("[offset:-500]\n[00:02.00]偏移后的歌词\n"));

    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.at(0).timeMs, qint64(1500));
}

void TestLrcParser::ignoresMetadataAndPlainText()
{
    const QVector<LrcLine> lines = LrcParser::parse(
        QStringLiteral("[ti:标题]\n[ar:歌手]\n[al:专辑]\n没有时间戳的说明\n[00:01.00]正文\n"));

    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.at(0).text, QStringLiteral("正文"));
}

void TestLrcParser::sortsOutOfOrderInput()
{
    const QVector<LrcLine> lines =
        LrcParser::parse(QStringLiteral("[00:03.00]第三\n[00:01.00]第一\n[00:02.00]第二\n"));

    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines.at(0).text, QStringLiteral("第一"));
    QCOMPARE(lines.at(1).text, QStringLiteral("第二"));
    QCOMPARE(lines.at(2).text, QStringLiteral("第三"));
}

void TestLrcParser::indexLookup()
{
    QVector<LrcLine> lines;
    lines.append(LrcLine{0, QStringLiteral("a")});
    lines.append(LrcLine{1000, QStringLiteral("b")});
    lines.append(LrcLine{2000, QStringLiteral("c")});

    QCOMPARE(LrcParser::indexAt(lines, -1), -1);
    QCOMPARE(LrcParser::indexAt(lines, 0), 0);
    QCOMPARE(LrcParser::indexAt(lines, 999), 0);
    QCOMPARE(LrcParser::indexAt(lines, 1000), 1);
    QCOMPARE(LrcParser::indexAt(lines, 1500), 1);
    QCOMPARE(LrcParser::indexAt(lines, 99999), 2);
}

QTEST_APPLESS_MAIN(TestLrcParser)

#include "tst_lrcparser.moc"
