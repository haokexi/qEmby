#include "views/media/libraryview.h"
#include "components/mediagridwidget.h"
#include "config/config_keys.h"
#include "config/configstore.h"
#include "services/manager/servermanager.h"

#include <QButtonGroup>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

namespace {

class LibraryFixture {
public:
    explicit LibraryFixture(QEmbyCore &core) {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                    [this, socket, buffer = QByteArray(), handled = false]() mutable {
                        buffer += socket->readAll();
                        if (handled || !buffer.contains("\r\n\r\n"))
                            return;
                        handled = true;
                        const QUrl url = QUrl::fromEncoded(
                            "http://localhost" + buffer.left(buffer.indexOf("\r\n")).split(' ').value(1));
                        QJsonObject response{{"Items", QJsonArray{}}, {"TotalRecordCount", 0}};
                        if (url.path().endsWith("/Items/library")) {
                            response = {{"Id", "library"}, {"Name", "Library"},
                                        {"Type", "CollectionFolder"}, {"CollectionType", collectionType}};
                        } else if (url.path() == "/Genres") {
                            response = {{"Items", QJsonArray{QJsonObject{{"Id", "drama"},
                                                                      {"Name", "Drama"}, {"Type", "Genre"}}}},
                                        {"TotalRecordCount", 1}};
                        } else if (url.path().endsWith("/Items")) {
                            itemQueries.append(QUrlQuery(url));
                        }
                        const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                      "Connection: close\r\nContent-Length: "
                                      + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    });
            }
        });
        ServerProfile profile;
        profile.url = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        profile.userId = QStringLiteral("library-test-user");
        profile.accessToken = QStringLiteral("library-test-token");
        core.serverManager()->addServer(profile);
        core.serverManager()->setActiveServer(profile.id);
        serverId = profile.id;
    }

    QTcpServer server;
    QString serverId;
    QString collectionType = QStringLiteral("movies");
    QList<QUrlQuery> itemQueries;
};

QPushButton *genreTab(LibraryView &view) {
    for (auto *button : view.findChildren<QPushButton *>("library-tab-btn")) {
        if (button->text() == QStringLiteral("Genres"))
            return button;
    }
    return nullptr;
}

} // namespace

class LibraryNavigationTest : public QObject {
    Q_OBJECT
    QEmbyCore *m_core = nullptr;

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("qEmbyTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("library-navigation-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_core = new QEmbyCore(this);
    }

    void cleanupTestCase() {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).removeRecursively();
    }

    void genreNavigationAndTabPersistence() {
        LibraryFixture fixture(*m_core);
        QVERIFY(fixture.server.isListening());
        LibraryView view(m_core);
        QCoro::waitFor(view.loadLibrary(QStringLiteral("library"), QStringLiteral("Library")));
        auto *genres = genreTab(view);
        QVERIFY(genres && !genres->isHidden());
        auto *page = view.findChild<QWidget *>("library-genres-page");
        auto *grid = page->findChild<MediaGridWidget *>();
        QVERIFY(grid);
        genres->click();
        QTRY_COMPARE(grid->itemCount(), 1);
        QVERIFY(!page->isHidden());

        MediaItem drama;
        drama.name = QStringLiteral("Drama");
        grid->itemClicked(drama);
        QTRY_VERIFY(!fixture.itemQueries.isEmpty()
                    && fixture.itemQueries.last().hasQueryItem("Genres"));
        QCOMPARE(fixture.itemQueries.last().queryItemValue("Genres"), QStringLiteral("Drama"));
        QCOMPARE(fixture.itemQueries.last().queryItemValue("ParentId"), QStringLiteral("library"));
        QVERIFY(page->isHidden());
        QVERIFY(view.handleBackNavigation());
        QVERIFY(!page->isHidden());
        QVERIFY(!view.handleBackNavigation());

        LibraryView restored(m_core);
        QCoro::waitFor(restored.loadLibrary(QStringLiteral("library"), QStringLiteral("Library")));
        QVERIFY(genreTab(restored)->isChecked());
        auto *restoredPage = restored.findChild<QWidget *>("library-genres-page");
        QVERIFY(!restoredPage->isHidden());
        QCOMPARE(restoredPage->findChild<MediaGridWidget *>()->itemCount(), 1);
    }

    void hiddenGenreTabFallsBackToAll() {
        LibraryFixture fixture(*m_core);
        fixture.collectionType = QStringLiteral("music");
        LibraryView view(m_core);
        auto *genres = genreTab(view);
        QVERIFY(genres);
        ConfigStore::instance()->set(
            ConfigKeys::forLibrary(fixture.serverId, QStringLiteral("library"), ConfigKeys::LibraryTabIndex),
            genres->group()->id(genres));

        QCoro::waitFor(view.loadLibrary(QStringLiteral("library"), QStringLiteral("Music")));
        QVERIFY(genres->isHidden());
        QVERIFY(!genres->isChecked());
        QCOMPARE(genres->group()->checkedButton()->text(), QStringLiteral("Music"));
        QVERIFY(view.findChild<QWidget *>("library-genres-page")->isHidden());
        QVERIFY(!fixture.itemQueries.isEmpty());
        QVERIFY(!fixture.itemQueries.last().hasQueryItem("Genres"));
    }
};

QTEST_MAIN(LibraryNavigationTest)
#include "librarynavigationtest.moc"
