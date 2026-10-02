#include <api/networkmanager.h>
#include <api/useragentmanager.h>
#include <services/manager/servermanager.h>

#include <QDir>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <stdexcept>

namespace {

class NetworkFixture {
public:
    NetworkFixture() : servers(&network) {
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
                        const QByteArray path = buffer.left(buffer.indexOf("\r\n")).split(' ').value(1);
                        paths.append(path);
                        for (const QByteArray &line : buffer.split('\n')) {
                            if (line.toLower().startsWith("user-agent:"))
                                userAgents.append(line.mid(line.indexOf(':') + 1).trimmed());
                        }
                        if (path == "/stall")
                            return;

                        QByteArray body = "{\"ok\":true}";
                        QByteArray extraHeaders;
                        int status = 200;
                        if (path == "/same-host" || path == "/cross-host") {
                            status = 302;
                            const QString target = path == "/same-host"
                                ? url(QStringLiteral("/ok"))
                                : QStringLiteral("http://localhost:%1/ok").arg(server.serverPort());
                            extraHeaders = "Location: " + target.toUtf8() + "\r\n";
                        } else if (path == "/large") {
                            body = "{\"data\":\"" + QByteArray(1024, 'x') + "\"}";
                        }
                        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\n"
                                      "Content-Type: application/json\r\nConnection: close\r\n"
                                      "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                                      + extraHeaders + "\r\n" + body);
                        socket->disconnectFromHost();
                    });
            }
        });
        ServerProfile profile;
        profile.url = url(QString());
        profile.userId = QStringLiteral("network-test-user");
        profile.accessToken = QStringLiteral("network-test-token");
        profile.useGlobalUserAgent = false;
        profile.userAgent.enabled = true;
        profile.userAgent.value = QStringLiteral("qEmby-network-test");
        servers.addServer(profile);
        servers.setActiveServer(profile.id);
        UserAgentManager::instance()->attachServerManager(&servers);
    }

    ~NetworkFixture() {
        UserAgentManager::instance()->attachServerManager(nullptr);
    }

    QString url(const QString &path) const {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path);
    }

    NetworkRequestOptions options() const {
        NetworkRequestOptions result;
        result.restrictRedirectsToSameHost = true;
        result.maximumResponseBytes = 256;
        result.timeoutMs = 1000;
        result.userAgentServerId = servers.activeProfile().id;
        return result;
    }

    QTcpServer server;
    NetworkManager network;
    ServerManager servers;
    QList<QByteArray> paths;
    QList<QByteArray> userAgents;
};

} // namespace

class NetworkOptionsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("qEmbyTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("network-options-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    void cleanupTestCase() {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).removeRecursively();
    }

    void apiUserAgentKeepsRedirectRestriction() {
        NetworkFixture fixture;
        QVERIFY(fixture.server.isListening());
        const QJsonObject result = QCoro::waitFor(fixture.servers.activeClient()->get("/same-host"));
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(fixture.paths, (QList<QByteArray>{"/same-host", "/ok"}));
        QCOMPARE(fixture.userAgents, (QList<QByteArray>{"qEmby-network-test", "qEmby-network-test"}));

        fixture.paths.clear();
        QVERIFY_EXCEPTION_THROWN(
            QCoro::waitFor(fixture.servers.activeClient()->get("/cross-host")), std::runtime_error);
        QCOMPARE(fixture.paths, (QList<QByteArray>{"/cross-host"}));
    }

    void batchKeepsLimitsAndPerRequestErrors() {
        NetworkFixture fixture;
        const auto options = fixture.options();
        const QList<NetworkJsonGetRequest> requests{
            {fixture.url("/ok"), {}, options},
            {fixture.url("/large"), {}, options},
            {fixture.url("/cross-host"), {}, options},
            {fixture.url("/same-host"), {}, options},
        };
        const QList<NetworkJsonResult> results = QCoro::waitFor(fixture.network.getBatch(requests));
        QCOMPARE(results.size(), 4);
        QVERIFY(results[0].succeeded());
        QVERIFY(results[0].object.value("ok").toBool());
        QVERIFY(!results[1].succeeded());
        QVERIFY(!results[2].succeeded());
        QVERIFY(results[3].succeeded());
        QCOMPARE(fixture.paths.count("/ok"), 2);
        QCOMPARE(fixture.userAgents.size(), fixture.paths.size());
        for (const QByteArray &userAgent : fixture.userAgents)
            QCOMPARE(userAgent, QByteArray("qEmby-network-test"));
    }

    void timeoutStillAppliesWithRedirectRestriction() {
        NetworkFixture fixture;
        auto options = fixture.options();
        options.timeoutMs = 100;
        QVERIFY_EXCEPTION_THROWN(
            QCoro::waitFor(fixture.network.get(fixture.url("/stall"), {}, options)), std::runtime_error);
        QCOMPARE(fixture.paths, (QList<QByteArray>{"/stall"}));
        QCOMPARE(fixture.userAgents, (QList<QByteArray>{"qEmby-network-test"}));
    }
};

QTEST_GUILESS_MAIN(NetworkOptionsTest)
#include "networkoptionstest.moc"
