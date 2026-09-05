#include <api/networkmanager.h>
#include <services/manager/servermanager.h>
#include <services/media/mediaservice.h>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>
#include <functional>
#include <stdexcept>

namespace {

class MediaFixture {
public:
    MediaFixture() : manager(&network), media(&manager) {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected,
                                 socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket, buffer = QByteArray()]() mutable {
                    buffer += socket->readAll();
                    if (!buffer.contains("\r\n\r\n"))
                        return;
                    const QByteArray target = buffer.left(buffer.indexOf("\r\n")).split(' ').value(1);
                    const QUrl url = QUrl::fromEncoded("http://localhost" + target);
                    requests.append(url);
                    const QByteArray body = QJsonDocument(respond(url)).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\n"
                                  "Content-Type: application/json\r\nConnection: close\r\n"
                                  "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });

        ServerProfile profile;
        profile.url = QStringLiteral("http://127.0.0.1:%1/emby").arg(server.serverPort());
        profile.userId = QStringLiteral("test-user");
        profile.accessToken = QStringLiteral("test-token");
        manager.addServer(profile);
        manager.setActiveServer(profile.id);
    }

    QTcpServer server;
    NetworkManager network;
    ServerManager manager;
    MediaService media;
    QList<QUrl> requests;
    int status = 200;
    std::function<QJsonObject(const QUrl &)> respond = [](const QUrl &) {
        return QJsonObject{{"Items", QJsonArray{}}, {"TotalRecordCount", 0}};
    };
};

} // namespace

class MediaGenreFilterTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("qEmbyTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("media-genres-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    void cleanupTestCase() {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).removeRecursively();
    }

    void genresAreLibraryScopedAndPaged() {
        MediaFixture fixture;
        QVERIFY(fixture.server.isListening());
        const QString war = QString::fromUtf8("\xe6\x88\x98\xe4\xba\x89");
        const QString romance = QString::fromUtf8("\xe7\x88\xb1\xe6\x83\x85");
        fixture.respond = [&](const QUrl &url) {
            const int offset = QUrlQuery(url).queryItemValue("StartIndex").toInt();
            const QJsonArray items = offset == 0
                ? QJsonArray{
                      QJsonObject{{"Id", "war"}, {"Name", war}, {"Type", "Genre"},
                                  {"ImageTags", QJsonObject{{"Primary", "war-primary"},
                                                            {"Thumb", "war-thumb"}}},
                                  {"BackdropImageTags", QJsonArray{"war-backdrop"}}},
                      QJsonObject{{"Id", "romance"}, {"Name", romance}}}
                : QJsonArray{QJsonObject{{"Name", war}}, QJsonObject{{"Name", "Drama"}}};
            return QJsonObject{{"Items", items}, {"TotalRecordCount", 4}};
        };

        const QList<MediaItem> genres = QCoro::waitFor(
            fixture.media.getLibraryGenres(QStringLiteral("chinese-movies"), QStringLiteral("Movie")));
        QCOMPARE(genres.size(), 3);
        QCOMPARE(genres.at(0).name, war);
        QCOMPARE(genres.at(0).id, QStringLiteral("war"));
        QCOMPARE(genres.at(0).type, QStringLiteral("Genre"));
        QCOMPARE(genres.at(0).images.primaryTag, QStringLiteral("war-primary"));
        QCOMPARE(genres.at(0).images.thumbTag, QStringLiteral("war-thumb"));
        QCOMPARE(genres.at(0).images.backdropTag, QStringLiteral("war-backdrop"));
        QCOMPARE(genres.at(1).name, romance);
        QCOMPARE(genres.at(1).id, QStringLiteral("romance"));
        QCOMPARE(genres.at(1).type, QStringLiteral("Genre"));
        QVERIFY(genres.at(1).images.primaryTag.isEmpty());
        QCOMPARE(genres.at(2).name, QStringLiteral("Drama"));
        QCOMPARE(fixture.requests.size(), 2);
        for (const QUrl &url : fixture.requests) {
            QCOMPARE(url.path(), QStringLiteral("/emby/Genres"));
            const QUrlQuery query(url);
            QCOMPARE(query.queryItemValue("ParentId"), QStringLiteral("chinese-movies"));
            QCOMPARE(query.queryItemValue("UserId"), QStringLiteral("test-user"));
            QCOMPARE(query.queryItemValue("IncludeItemTypes"), QStringLiteral("Movie"));
            QCOMPARE(query.queryItemValue("Recursive"), QStringLiteral("true"));
            QCOMPARE(query.queryItemValue("EnableImages"), QStringLiteral("true"));
            QCOMPARE(query.queryItemValue("EnableImageTypes"), QStringLiteral("Primary,Backdrop,Thumb"));
            QCOMPARE(query.queryItemValue("ImageTypeLimit"), QStringLiteral("1"));
        }
        QCOMPARE(QUrlQuery(fixture.requests.last()).queryItemValue("StartIndex"), QStringLiteral("2"));
    }

    void repeatedGenrePagesStop() {
        MediaFixture fixture;
        fixture.respond = [](const QUrl &) {
            return QJsonObject{{"Items", QJsonArray{QJsonObject{{"Name", "Drama"}}}},
                               {"TotalRecordCount", 20}};
        };
        const QList<MediaItem> genres =
            QCoro::waitFor(fixture.media.getLibraryGenres(QStringLiteral("library")));
        QCOMPARE(genres.size(), 1);
        QCOMPARE(genres.first().name, QStringLiteral("Drama"));
        QCOMPARE(fixture.requests.size(), 2);
    }

    void emptyAndFailedGenreResponses() {
        MediaFixture fixture;
        QVERIFY(QCoro::waitFor(fixture.media.getLibraryGenres(QString())).isEmpty());
        QVERIFY(fixture.requests.isEmpty());
        QVERIFY(QCoro::waitFor(fixture.media.getLibraryGenres(QStringLiteral("library"))).isEmpty());

        fixture.status = 503;
        QVERIFY_EXCEPTION_THROWN(
            QCoro::waitFor(fixture.media.getLibraryGenres(QStringLiteral("library"))),
            std::runtime_error);
    }

    void genreFilterKeepsLibraryQuery_data() {
        QTest::addColumn<QString>("genre");
        QTest::newRow("chinese") << QString::fromUtf8("\xe6\x88\x98\xe4\xba\x89");
        QTest::newRow("reserved-characters") << QStringLiteral("War & Peace + Love/#50%?");
        QTest::newRow("all-genres") << QString();
    }

    void genreFilterKeepsLibraryQuery() {
        QFETCH(QString, genre);
        MediaFixture fixture;
        fixture.respond = [](const QUrl &) {
            return QJsonObject{{"Items", QJsonArray{QJsonObject{{"Id", "matching-movie"}, {"Type", "Movie"}}}},
                               {"TotalRecordCount", 53}};
        };
        const MediaQueryPage page = QCoro::waitFor(fixture.media.getLibraryItemsPage(
            QStringLiteral("chinese-movies"), QStringLiteral("PremiereDate"),
            QStringLiteral("Descending"), QStringLiteral("IsFavorite"),
            QStringLiteral("Movie"), 20, 10, true, false, genre));

        QCOMPARE(fixture.requests.size(), 1);
        const QUrl url = fixture.requests.first();
        QCOMPARE(url.path(), QStringLiteral("/emby/Users/test-user/Items"));
        const QUrlQuery query(url);
        QCOMPARE(query.queryItemValue("ParentId"), QStringLiteral("chinese-movies"));
        QCOMPARE(query.queryItemValue("IncludeItemTypes"), QStringLiteral("Movie"));
        QCOMPARE(query.queryItemValue("Recursive"), QStringLiteral("true"));
        QCOMPARE(query.queryItemValue("SortBy"), QStringLiteral("PremiereDate"));
        QCOMPARE(query.queryItemValue("SortOrder"), QStringLiteral("Descending"));
        QCOMPARE(query.queryItemValue("Filters"), QStringLiteral("IsFavorite"));
        QCOMPARE(query.queryItemValue("StartIndex"), QStringLiteral("20"));
        QCOMPARE(query.queryItemValue("Limit"), QStringLiteral("10"));
        QCOMPARE(query.hasQueryItem("Genres"), !genre.isEmpty());
        QCOMPARE(query.queryItemValue("Genres", QUrl::FullyDecoded), genre);
        if (genre.contains('+'))
            QVERIFY(url.toEncoded().contains("%2B"));
        QCOMPARE(page.totalRecordCount, 53);
        QCOMPARE(page.startIndex, 20);
        QVERIFY(page.hasMore());
        QCOMPARE(page.items.size(), 1);
        QCOMPARE(page.items.first().id, QStringLiteral("matching-movie"));
    }
};

QTEST_GUILESS_MAIN(MediaGenreFilterTest)
#include "mediagenrefiltertest.moc"
