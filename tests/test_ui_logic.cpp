#include "media/MediaItem.h"
#include "net/HttpClient.h"
#include "ui/LyricsController.h"

#include <QListWidget>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QWidget>

namespace {

/// 歌词接口延迟 500ms 返回一行旧歌词；用来验证切走后旧请求被取消。
class DelayedLyricsServer : public QTcpServer
{
public:
    explicit DelayedLyricsServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
        body = QByteArrayLiteral("{\"lrc\":{\"lyric\":\"[00:01]旧歌词\"}}");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("responded").toBool())
                        return;
                    QByteArray pending = socket->property("pending").toByteArray()
                                         + socket->readAll();
                    if (!pending.contains("\r\n\r\n")) {
                        socket->setProperty("pending", pending);
                        return;
                    }
                    socket->setProperty("responded", true);
                    ++requestCount;

                    const QByteArray head =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n\r\n";
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(500, this, [this, guard, head] {
                        if (guard.isNull())
                            return;
                        guard->write(head);
                        guard->write(body);
                        guard->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QByteArray body;
    int requestCount = 0;
};

} // namespace

class UiLogicTest : public QObject
{
    Q_OBJECT

private slots:
    void lyricsClearCancelsPendingRequest();
};

void UiLogicTest::lyricsClearCancelsPendingRequest()
{
    DelayedLyricsServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    HttpClient http;
    QWidget panel;
    QListWidget list;
    LyricsController controller(&http, LyricsUi{&panel, &list});

    MediaItem song;
    song.type = MediaType::Music;
    song.title = QStringLiteral("测试歌曲");
    song.url = QUrl(QStringLiteral("https://example.com/song.mp3"));
    song.lyricUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/lyric")
                             .arg(server.serverPort()));

    controller.setCurrentItem(song);
    QTRY_VERIFY_WITH_TIMEOUT(server.requestCount > 0, 2000);

    // 模拟切到无歌词条目/视频：旧请求必须立刻取消，500ms 后不能回填列表。
    controller.clear();
    QTest::qWait(900);
    QCOMPARE(list.count(), 0);
}

QTEST_MAIN(UiLogicTest)

#include "test_ui_logic.moc"
