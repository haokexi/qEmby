#include "views/media/playerview.h"
#include "views/user/dashboardview.h"
#include "components/horizontallistviewgallery.h"
#include "components/playermediaswitcherpanel.h"
#include "components/mpvhttpstreamrelay.h"
#include "config/config_keys.h"
#include "config/configstore.h"
#include "models/media/playerlaunchcontext.h"
#include "qembycore.h"
#include "services/manager/servermanager.h"
#include "services/media/mediaservice.h"

#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QEnterEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListWidget>
#include <QOpenGLContext>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

namespace {

// A local Emby fixture. All media requests initially fail with 429; recovery
// serves a small silent WAV so libmpv can genuinely load and seek the stream.
class PlaybackFixture {
public:
    explicit PlaybackFixture(QEmbyCore &appCore) : core(appCore) {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                    [this, socket, buffer = QByteArray(), handled = false]() mutable {
                        if (handled)
                            return;
                        buffer += socket->readAll();
                        const int headerEnd = buffer.indexOf("\r\n\r\n");
                        if (headerEnd < 0)
                            return;
                        int bodySize = 0;
                        for (const QByteArray &line : buffer.left(headerEnd).split('\n')) {
                            if (line.toLower().startsWith("content-length:"))
                                bodySize = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                        }
                        if (buffer.size() < headerEnd + 4 + bodySize)
                            return;
                        handled = true;
                        const QUrl url = QUrl::fromEncoded(
                            "http://localhost" + buffer.left(buffer.indexOf("\r\n")).split(' ').value(1));
                        const QJsonObject payload = QJsonDocument::fromJson(buffer.mid(headerEnd + 4)).object();
                        respond(socket, url, payload, buffer.left(headerEnd));
                    });
            }
        });

        ServerProfile profile;
        profile.url = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        profile.userId = QStringLiteral("test-user");
        profile.accessToken = QStringLiteral("test-token");
        profile.useGlobalUserAgent = false;
        profile.userAgent.enabled = true;
        profile.userAgent.value = QStringLiteral("qEmby-playback-test");
        core.serverManager()->addServer(profile);
        core.serverManager()->setActiveServer(profile.id);

        QDataStream wave(&media, QIODevice::WriteOnly);
        wave.setByteOrder(QDataStream::LittleEndian);
        constexpr int sampleBytes = 8000 * 2 * 60;
        wave.writeRawData("RIFF", 4);
        wave << quint32(36 + sampleBytes);
        wave.writeRawData("WAVEfmt ", 8);
        wave << quint32(16) << quint16(1) << quint16(1) << quint32(8000)
             << quint32(16000) << quint16(2) << quint16(16);
        wave.writeRawData("data", 4);
        wave << quint32(sampleBytes);
        media.append(QByteArray(sampleBytes, '\0'));
    }

    QJsonArray sources() const {
        return {QJsonObject{{"Id", "source-a"}, {"Container", "strm"}},
                QJsonObject{{"Id", "source-b"}, {"Container", "strm"}}};
    }

    QJsonObject item(const QString &id) const {
        if (items.contains(id))
            return items.value(id);
        return {{"Id", id}, {"Name", id}, {"Type", "Movie"}, {"MediaSources", sources()},
                {"RunTimeTicks", unknownDuration ? 0 : 600000000},
                {"UserData", QJsonObject{{"PlaybackPositionTicks", 20000000}}}};
    }

    void start(PlayerView &view) {
        PlayerLaunchContext context;
        context.mediaItem = MediaItem::fromJson(item(QStringLiteral("test-media")));
        context.selectedSource = context.mediaItem.mediaSources.first();
        view.playMedia(context.mediaItem.id, context.mediaItem.name,
                       core.mediaService()->getStreamUrl(context.mediaItem.id, context.selectedSource),
                       20000000, QVariant::fromValue(context));
    }

    void respond(QTcpSocket *socket, const QUrl &url, const QJsonObject &payload,
                 const QByteArray &headers) {
        QByteArray body;
        QByteArray contentType = "application/json";
        QByteArray extraHeaders;
        int status = 200;
        if (url.path().endsWith("/stream")) {
            streamRequests.append(url);
            for (const QByteArray &line : headers.split('\n')) {
                if (line.toLower().startsWith("user-agent:"))
                    streamUserAgents.append(line.mid(line.indexOf(':') + 1).trimmed());
            }
            if (rateLimited) {
                status = 429;
                body = "Too Many Requests";
            } else {
                contentType = "audio/wav";
                qint64 offset = 0;
                for (const QByteArray &line : headers.split('\n')) {
                    if (line.toLower().startsWith("range: bytes="))
                        offset = line.mid(13).split('-').first().toLongLong();
                }
                offset = qBound<qint64>(0, offset, media.size() - 1);
                status = 206;
                body = media.mid(offset);
                extraHeaders = "Accept-Ranges: bytes\r\nContent-Range: bytes "
                    + QByteArray::number(offset) + "-" + QByteArray::number(media.size() - 1)
                    + "/" + QByteArray::number(media.size()) + "\r\n";
            }
        } else {
            QJsonObject result;
            if (url.path().endsWith("/PlaybackInfo")) {
                const QString itemId = url.path().section('/', -2, -2);
                result = {{"PlaySessionId", QStringLiteral("session-%1").arg(++sessionCount)},
                          {"MediaSources", playbackSources.value(
                              itemId, item(itemId).value("MediaSources").toArray())}};
            } else if (url.path() == "/Sessions/Playing") {
                starts.append(payload);
            } else if (url.path() == "/Sessions/Playing/Progress") {
                progress.append(payload);
            } else if (url.path() == "/Sessions/Playing/Stopped") {
                stops.append(payload);
                if (holdStopResponse) {
                    pendingStop = socket;
                    return;
                }
            } else if (url.path().endsWith("/Seasons")) {
                result = {{"Items", QJsonArray{QJsonObject{{"Id", "test-season"}, {"Type", "Season"}}}}};
            } else if (url.path().endsWith("/Episodes")) {
                result = {{"Items", episodes}, {"TotalRecordCount", episodes.size()}};
            } else if (url.path().endsWith("/Items/Resume")) {
                ++resumeRequests;
                if (unknownDuration) {
                    result = {{"Items", resumeItems}, {"TotalRecordCount", resumeItems.size()}};
                } else {
                    result = {{"Items", QJsonArray{item("test-media"), item("next-media")}},
                              {"TotalRecordCount", 2}};
                }
            } else if (url.path().contains("/Images/")) {
                status = 404;
            } else if (url.path().contains("/Items/")) {
                result = item(url.path().section('/', -1));
            }
            body = QJsonDocument(result).toJson(QJsonDocument::Compact);
        }
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\n"
                      "Connection: close\r\nContent-Type: " + contentType + "\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                      + extraHeaders + "\r\n" + body);
        socket->disconnectFromHost();
    }

    QTcpServer server;
    QEmbyCore &core;
    bool rateLimited = true;
    bool unknownDuration = false;
    bool holdStopResponse = false;
    QPointer<QTcpSocket> pendingStop;
    QHash<QString, QJsonObject> items;
    QHash<QString, QJsonArray> playbackSources;
    QJsonArray episodes;
    QJsonArray resumeItems;
    int resumeRequests = 0;
    int sessionCount = 0;
    QList<QUrl> streamRequests;
    QList<QByteArray> streamUserAgents;
    QList<QJsonObject> starts;
    QList<QJsonObject> progress;
    QList<QJsonObject> stops;
    QByteArray media;
};

QGraphicsOpacityEffect *hudOpacity(PlayerView &view, const char *name) {
    auto *hud = view.findChild<QWidget *>(QLatin1String(name));
    return hud ? qobject_cast<QGraphicsOpacityEffect *>(hud->graphicsEffect()) : nullptr;
}

} // namespace

class PlaybackFailureTest : public QObject {
    Q_OBJECT
    QEmbyCore *m_core = nullptr;

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("qEmbyTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("playback-failure-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
        ConfigStore::instance()->set(ConfigKeys::PlayerDanmakuEnabled, false);
        ConfigStore::instance()->set(ConfigKeys::PlayerHwDec, QStringLiteral("no"));
        ConfigStore::instance()->set(ConfigKeys::PlayerUseMpvConf, false);
        ConfigStore::instance()->set(ConfigKeys::PlayerVolumeMuted, true);
        // Production has one application core; its proxy factory is a singleton.
        m_core = new QEmbyCore(this);
    }

    void cleanupTestCase() {
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).removeRecursively();
    }

    void errorSignalKeepsControlsUsable() {
        PlayerView view(m_core);
        auto *mpv = view.findChild<MpvWidget *>();
        auto *topOpacity = hudOpacity(view, "playerTopHUD");
        auto *bottomOpacity = hudOpacity(view, "playerBottomHUD");
        QVERIFY(mpv && topOpacity && bottomOpacity);
        topOpacity->setOpacity(0);
        bottomOpacity->setOpacity(0);
        view.setCursor(Qt::BlankCursor);
        mpv->controller()->endOfFile(QStringLiteral("error"));

        auto *panel = view.findChild<QWidget *>("playerPlaybackErrorPanel");
        QVERIFY(panel && !panel->isHidden());
        QTRY_COMPARE(topOpacity->opacity(), 1.0);
        QCOMPARE(bottomOpacity->opacity(), 1.0);
        QCOMPARE(view.cursor().shape(), Qt::ArrowCursor);
        QVERIFY(!view.isMediaPlaying());
        // Late MPV properties must not resurrect playback or its loading overlay.
        mpv->controller()->playbackStateChanged(false);
        mpv->controller()->propertyChanged("paused-for-cache", true);
        QVERIFY(!view.isMediaPlaying());
        QVERIFY(view.findChild<LoadingOverlay *>()->isHidden());
        QVERIFY(QMetaObject::invokeMethod(&view, "hideControls"));
        QCOMPARE(topOpacity->opacity(), 1.0);

        QSignalSpy back(&view, &PlayerView::navigateBack);
        view.findChild<QPushButton *>("playerPlaybackBackButton")->click();
        QCOMPARE(back.count(), 1);
        QVERIFY(panel->isHidden());
    }

    void rateLimitRecovery_data() {
        QTest::addColumn<bool>("keyboardRetry");
        QTest::newRow("retry-button") << false;
        QTest::newRow("space-key") << true;
    }

    void episodeSwitchKeepsSourcePosition_data() {
        QTest::addColumn<int>("selectedIndex");
        QTest::addColumn<int>("nextSourceCount");
        QTest::addColumn<bool>("sameSeries");
        QTest::addColumn<bool>("autoAdvance");
        QTest::addColumn<bool>("rememberedNextSource");
        QTest::addColumn<bool>("reorderedPlaybackInfo");
        QTest::addColumn<int>("expectedIndex");
        QTest::addColumn<bool>("sourceOnlyLaunch");
        QTest::newRow("fourth-source") << 3 << 4 << true << false << false << false << 3 << false;
        QTest::newRow("second-source") << 1 << 4 << true << false << false << false << 1 << false;
        QTest::newRow("continuous-playback") << 3 << 4 << true << true << false << false << 3 << false;
        QTest::newRow("position-overrides-old-selection") << 3 << 4 << true << false << true << false << 3 << false;
        QTest::newRow("fewer-sources-use-rules") << 3 << 2 << true << false << false << false << 0 << false;
        QTest::newRow("fewer-sources-use-remembered") << 3 << 2 << true << false << true << false << 1 << false;
        QTest::newRow("different-series") << 3 << 4 << false << false << false << false << 0 << false;
        QTest::newRow("metadata-refresh-keeps-position") << 3 << 4 << true << false << false << true << 3 << false;
        QTest::newRow("detail-page-launch") << 3 << 4 << true << true << false << false << 3 << true;
    }

    void episodeSwitchKeepsSourcePosition() {
        QFETCH(int, selectedIndex);
        QFETCH(int, nextSourceCount);
        QFETCH(bool, sameSeries);
        QFETCH(bool, autoAdvance);
        QFETCH(bool, rememberedNextSource);
        QFETCH(bool, reorderedPlaybackInfo);
        QFETCH(int, expectedIndex);
        QFETCH(bool, sourceOnlyLaunch);
        QOpenGLContext gl;
        if (!gl.create())
            QSKIP("The libmpv integration test needs OpenGL and a display.");

        PlaybackFixture fixture(*m_core);
        fixture.rateLimited = false;
        auto episode = [&fixture](const QString &id, int number, int sourceCount,
                                  const QString &seriesId) {
            QJsonObject result = fixture.item(id);
            result["Type"] = "Episode";
            result["SeriesId"] = seriesId;
            result["ParentIndexNumber"] = 2;
            result["IndexNumber"] = number;
            QJsonArray sources;
            for (int i = 0; i < sourceCount; ++i) {
                sources.append(QJsonObject{
                    {"Id", QStringLiteral("%1-source-%2").arg(id).arg(i)},
                    {"Name", QStringLiteral("%1 version %2").arg(id).arg(i)},
                    {"Container", "strm"}, {"Size", 10000 - i * 1000}});
            }
            result["MediaSources"] = sources;
            return result;
        };
        const auto current = episode("episode-3", 3, 4, "test-series");
        const auto next = episode("episode-4", 4, nextSourceCount,
                                  sameSeries ? "test-series" : "other-series");
        fixture.items.insert("episode-3", current);
        fixture.items.insert("episode-4", next);
        fixture.episodes = QJsonArray{current, next};
        if (reorderedPlaybackInfo) {
            QJsonArray reordered;
            const auto sources = current.value("MediaSources").toArray();
            for (int i = sources.size() - 1; i >= 0; --i)
                reordered.append(sources.at(i));
            fixture.playbackSources.insert("episode-3", reordered);
        }
        const auto nextSources = next.value("MediaSources").toArray();
        const QString expectedSourceId = nextSources.at(expectedIndex).toObject().value("Id").toString();
        ConfigStore::instance()->set(ConfigKeys::PlayerPreferredVersion, QStringLiteral("size-desc"));
        ConfigStore::instance()->set(ConfigKeys::PlayerContinuousPlay, true);
        if (rememberedNextSource) {
            ConfigStore::instance()->set(ConfigKeys::forServerMedia(
                m_core->serverManager()->activeProfile().id, "episode-4",
                ConfigKeys::PlayerSelectedMediaSource), nextSources.at(1).toObject().value("Id").toString());
        }

        PlayerView view(m_core);
        view.setWindowFlag(Qt::Tool);
        view.setAttribute(Qt::WA_ShowWithoutActivating);
        view.resize(1000, 700);
        view.show();
        auto *mpv = view.findChild<MpvWidget *>();
        QTRY_VERIFY(mpv->isValid());
        QSignalSpy loaded(mpv->controller(), &MpvController::fileLoaded);
        PlayerLaunchContext context;
        context.mediaItem = MediaItem::fromJson(current);
        context.selectedSource = context.mediaItem.mediaSources.at(selectedIndex);
        view.playMedia(context.mediaItem.id, context.mediaItem.name,
                       m_core->mediaService()->getStreamUrl(context.mediaItem.id, context.selectedSource),
                       0, sourceOnlyLaunch ? QVariant::fromValue(context.selectedSource)
                                           : QVariant::fromValue(context));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QCOMPARE(fixture.starts.first().value("MediaSourceId").toString(), context.selectedSource.id);

        if (autoAdvance)
            mpv->controller()->endOfFile(QStringLiteral("eof"));
        else
            view.findChild<PlayerMediaSwitcherPanel *>()->playRequested("episode-4", "episode-4", 0);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 10000);
        QCOMPARE(fixture.starts.last().value("ItemId").toString(), QStringLiteral("episode-4"));
        QCOMPARE(fixture.starts.last().value("MediaSourceId").toString(), expectedSourceId);
        QCOMPARE(QUrlQuery(fixture.streamRequests.last()).queryItemValue("mediaSourceId"), expectedSourceId);
        view.prepareForStackLeave();
        QTRY_COMPARE(fixture.stops.size(), 2);
        ConfigStore::instance()->set(ConfigKeys::PlayerPreferredVersion, QString());
    }

    void unknownStrmDurationSurvivesStopAndRefreshesDashboard() {
        QOpenGLContext gl;
        if (!gl.create())
            QSKIP("The libmpv integration test needs OpenGL and a display.");

        PlaybackFixture fixture(*m_core);
        fixture.rateLimited = false;
        fixture.unknownDuration = true;
        fixture.holdStopResponse = true;
        const QString serverId = m_core->serverManager()->activeProfile().id;
        for (const char *key : {ConfigKeys::ShowLatestAdded, ConfigKeys::ShowRecommended,
                               ConfigKeys::ShowCompletedWatching, ConfigKeys::ShowMediaLibraries,
                               ConfigKeys::ShowEachLibrary})
            ConfigStore::instance()->set(ConfigKeys::forServer(serverId, key), false);

        DashboardView dashboard(m_core);
        PlayerView view(m_core);
        view.resize(1000, 700);
        view.show();
        auto *mpv = view.findChild<MpvWidget *>();
        QTRY_VERIFY(mpv->isValid());
        QSignalSpy loaded(mpv->controller(), &MpvController::fileLoaded);
        fixture.start(view);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QTRY_VERIFY(!fixture.progress.isEmpty()
                    && fixture.progress.last().value("RunTimeTicks").toInteger() == 600000000);
        view.pausePlayback();
        QTRY_VERIFY(!view.isMediaPlaying());
        QVERIFY(QMetaObject::invokeMethod(&view, "onSliderMoved", Q_ARG(int, 20)));
        QTRY_VERIFY(mpv->controller()->getProperty("time-pos").toDouble() >= 19.0);
        // MPV can clear duration during teardown; the final report still needs it.
        mpv->durationChanged(0.0);

        QSignalSpy stopped(m_core->mediaService(), &MediaService::playbackStopped);
        view.prepareForStackLeave();
        view.hide();
        QTRY_VERIFY(fixture.pendingStop);
        QCOMPARE(stopped.count(), 0);
        QCOMPARE(fixture.progress.last().value("RunTimeTicks").toInteger(), 600000000);
        QVERIFY(fixture.stops.last().value("PositionTicks").toInteger() >= 190000000);

        dashboard.resize(1000, 700);
        dashboard.show();
        QTRY_VERIFY(fixture.resumeRequests > 0);
        const auto galleries = dashboard.findChildren<HorizontalListViewGallery *>();
        const auto containsResume = [&] {
            for (const auto *gallery : galleries) {
                for (const MediaItem &entry : gallery->items()) {
                    if (entry.id == QStringLiteral("test-media"))
                        return true;
                }
            }
            return false;
        };
        QVERIFY(!containsResume());
        const int initialRequests = fixture.resumeRequests;

        // Emulate the server committing progress after the first home-page fetch.
        auto resumed = fixture.item(QStringLiteral("test-media"));
        resumed["RunTimeTicks"] = fixture.progress.last().value("RunTimeTicks");
        resumed["UserData"] = QJsonObject{{"PlaybackPositionTicks", fixture.stops.last().value("PositionTicks")},
                                          {"Played", false}, {"PlayedPercentage", 33.3}};
        fixture.resumeItems = QJsonArray{resumed};
        fixture.pendingStop->write("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
        fixture.pendingStop->disconnectFromHost();
        QTRY_COMPARE(stopped.count(), 1);
        QCOMPARE(stopped.first().at(0).toString(), serverId);
        QTRY_VERIFY(fixture.resumeRequests > initialRequests);
        QTRY_VERIFY(containsResume());
    }

    void windowRestoreKeepsSessionAndRelay_data() {
        QTest::addColumn<bool>("resumePlaying");
        QTest::newRow("restore-playing") << true;
        QTest::newRow("restore-paused") << false;
    }

    void windowRestoreKeepsSessionAndRelay() {
        QFETCH(bool, resumePlaying);
        QOpenGLContext gl;
        if (!gl.create())
            QSKIP("The libmpv integration test needs OpenGL and a display.");

        PlaybackFixture fixture(*m_core);
        fixture.rateLimited = false;
        PlayerView view(&fixture.core);
        view.setWindowFlag(Qt::Tool);
        view.setAttribute(Qt::WA_ShowWithoutActivating);
        view.resize(1000, 700);
        view.show();
        auto *mpv = view.findChild<MpvWidget *>();
        auto *relay = mpv->findChild<MpvHttpStreamRelay *>();
        QVERIFY(relay);
        QTRY_VERIFY(mpv->isValid());
        QSignalSpy loaded(mpv->controller(), &MpvController::fileLoaded);
        fixture.start(view);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QTRY_VERIFY(mpv->controller()->getProperty("time-pos").toDouble() >= 1.0);
        view.pausePlayback();
        QTRY_VERIFY(!view.isMediaPlaying());
        const double position = mpv->controller()->getProperty("time-pos").toDouble();
        const QString session = fixture.starts.first().value("PlaySessionId").toString();
        QVERIFY(!session.isEmpty());
        const qsizetype previousRequests = fixture.streamRequests.size();

        view.restoreAfterWindowShow(resumePlaying);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 10000);
        QTRY_COMPARE(mpv->controller()->getProperty("pause").toBool(), !resumePlaying);
        QTRY_VERIFY(mpv->controller()->getProperty("time-pos").toDouble() >= position - 0.5);
        QVERIFY(fixture.streamRequests.size() > previousRequests);
        QVERIFY(relay->hasStartedSuccessfulMediaResponse());
        QCOMPARE(fixture.starts.size(), 1);
        QVERIFY(fixture.stops.isEmpty());
        for (const QUrl &request : fixture.streamRequests)
            QCOMPARE(QUrlQuery(request).queryItemValue("PlaySessionId"), session);
        QCOMPARE(fixture.streamUserAgents.size(), fixture.streamRequests.size());
        for (const QByteArray &userAgent : fixture.streamUserAgents)
            QCOMPARE(userAgent, QByteArray("qEmby-playback-test"));

        // Reusing the player must report the old session once and load the new one.
        fixture.start(view);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 3, 10000);
        QTRY_COMPARE(fixture.stops.size(), 1);
        QCOMPARE(fixture.starts.size(), 2);
        QVERIFY(fixture.starts.last().value("PlaySessionId").toString() != session);
        view.prepareForStackLeave();
        QTRY_COMPARE(fixture.stops.size(), 2);
    }

    void rateLimitRecovery() {
        QFETCH(bool, keyboardRetry);
        QOpenGLContext gl;
        if (!gl.create())
            QSKIP("The libmpv integration test needs OpenGL and a display (for example Xvfb).");

        PlaybackFixture fixture(*m_core);
        QVERIFY(fixture.server.isListening());
        PlayerView view(&fixture.core);
        view.setWindowFlag(Qt::Tool);
        view.setAttribute(Qt::WA_ShowWithoutActivating);
        view.resize(1000, 700);
        view.show();
        auto *mpv = view.findChild<MpvWidget *>();
        QTRY_VERIFY(mpv->isValid());
        QSignalSpy loaded(mpv->controller(), &MpvController::fileLoaded);
        fixture.start(view);
        QVERIFY(QMetaObject::invokeMethod(&view, "hideControls"));
        auto *topOpacity = hudOpacity(view, "playerTopHUD");
        QVERIFY(topOpacity);
        QTRY_COMPARE(topOpacity->opacity(), 0.0);

        auto *panel = view.findChild<QWidget *>("playerPlaybackErrorPanel");
        QTRY_VERIFY_WITH_TIMEOUT(panel && panel->isVisible(), 20000);
        QTRY_COMPARE(topOpacity->opacity(), 1.0);
        QCOMPARE(fixture.streamRequests.size(), 8);
        QCOMPARE(fixture.starts.size(), 2);
        QTRY_COMPARE(fixture.stops.size(), 2);
        QVERIFY(!view.isMediaPlaying());
        QVERIFY(view.findChild<QLabel *>("playerPlaybackErrorLabel")->text().contains("limiting"));
        QVERIFY(view.findChild<LoadingOverlay *>()->isHidden());

        ConfigStore::instance()->set(ConfigKeys::PlayerMediaSwitcherMode, QStringLiteral("hud"));
        QTest::mouseClick(view.findChild<QPushButton *>("playerMediaSwitchBtn"), Qt::LeftButton);
        QVERIFY(view.findChild<PlayerMediaSwitcherPanel *>()->isVisible());
        ConfigStore::instance()->set(ConfigKeys::PlayerMediaSwitcherMode, QStringLiteral("sidebar"));
        auto *list = view.findChild<QListWidget *>();
        QVERIFY(list);
        // Enter the existing sidebar trigger through the same event filter as a mouse.
        auto *trigger = view.findChild<QWidget *>("playerRightTrigger");
        QVERIFY(trigger);
        QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), trigger->mapToGlobal(QPoint(1, 1)));
        QCoreApplication::sendEvent(trigger, &enter);
        QTRY_VERIFY(list->isVisible());

        fixture.rateLimited = false;
        if (keyboardRetry)
            QTest::keyClick(&view, Qt::Key_Space);
        else
            QTest::mouseClick(view.findChild<QPushButton *>("playerPlaybackRetryButton"), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QVERIFY(panel->isHidden());
        QCOMPARE(fixture.starts.size(), 3);
        QCOMPARE(fixture.starts.last().value("PositionTicks").toInteger(), 20000000);
        QVERIFY(fixture.streamRequests.size() > 8);
        QVERIFY(QUrlQuery(fixture.streamRequests.last()).queryItemValue("PlaySessionId") !=
                fixture.stops.last().value("PlaySessionId").toString());

        // A later non-429 failure must also allow switching media and going back.
        mpv->controller()->endOfFile(QStringLiteral("error"));
        QTRY_VERIFY(panel->isVisible());
        QTRY_COMPARE(fixture.stops.size(), 3);
        QVERIFY(view.findChild<QLabel *>("playerPlaybackErrorLabel")->text().contains("Playback failed"));
        auto *switcher = view.findChild<PlayerMediaSwitcherPanel *>();
        switcher->playRequested("next-media", "next-media", 0);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 10000);
        QCOMPARE(fixture.starts.last().value("ItemId").toString(), QStringLiteral("next-media"));
        QCOMPARE(fixture.stops.size(), 3);

        mpv->controller()->endOfFile(QStringLiteral("error"));
        QTRY_COMPARE(fixture.stops.size(), 4);
        QSignalSpy back(&view, &PlayerView::navigateBack);
        QTest::mouseClick(view.findChild<QPushButton *>("playerPlaybackBackButton"), Qt::LeftButton);
        QCOMPARE(back.count(), 1);
        view.prepareForStackLeave();
        QTest::qWait(100);
        QCOMPARE(fixture.stops.size(), 4);
        QVERIFY(panel->isHidden());
    }
};

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
        && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    PlaybackFailureTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "playbackfailuretest.moc"
