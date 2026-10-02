#include "mpvhttpstreamrelay.h"

#include "../utils/logredactionutils.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include "api/useragentmanager.h"
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

namespace
{

constexpr qint64 kReplyReadBufferBytes = 4 * 1024 * 1024;
constexpr qint64 kSocketQueuedBytesHighWater = 4 * 1024 * 1024;
constexpr qint64 kRelayPumpChunkBytes = 256 * 1024;
constexpr int kUpstreamHandoffDelayMs = 400;
constexpr int kRateLimitRetryBaseDelayMs = 500;
constexpr int kRateLimitRetryMaxDelayMs = 5000;
constexpr int kMaxRateLimitRetries = 3;

bool canWriteToSocket(QTcpSocket *socket)
{
    return socket && socket->isOpen() && socket->isWritable() && socket->state() != QAbstractSocket::UnconnectedState;
}

} 

MpvHttpStreamRelay::MpvHttpStreamRelay(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this)), m_network(new QNetworkAccessManager(this)),
      m_speedTimer(new QTimer(this))
{
    m_lifecycleClock.start();
    connect(m_server, &QTcpServer::newConnection, this, [this]() { onNewConnection(); });
    m_speedTimer->setInterval(1000);
    connect(m_speedTimer, &QTimer::timeout, this,
            [this]()
            {
                const qint64 speed = m_bytesRelayedSinceLastTick;
                m_bytesRelayedSinceLastTick = 0;
                Q_EMIT upstreamSpeedChanged(speed);
            });
}

MpvHttpStreamRelay::~MpvHttpStreamRelay()
{
    stop();
}

QUrl MpvHttpStreamRelay::prepare(const QUrl &targetUrl, const QString &serverId, const QNetworkProxy &proxy)
{
    stop();
    if (!targetUrl.isValid() || targetUrl.scheme().isEmpty())
    {
        return {};
    }

    if (!m_server->isListening() && !m_server->listen(QHostAddress::LocalHost, 0))
    {
        qWarning() << "[MpvHttpStreamRelay] failed to listen"
                   << "| error:" << m_server->errorString();
        return {};
    }

    m_targetUrl = targetUrl;
    m_serverId = serverId;
    m_streamToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_network->setProxy(proxy);
    m_bytesRelayedSinceLastTick = 0;
    m_speedTimer->start();

    QUrl localUrl;
    localUrl.setScheme(QStringLiteral("http"));
    localUrl.setHost(QStringLiteral("127.0.0.1"));
    localUrl.setPort(m_server->serverPort());
    localUrl.setPath(QStringLiteral("/%1/stream").arg(m_streamToken));

    qInfo() << "[MpvHttpStreamRelay] prepared"
            << "| target:" << LogRedactionUtils::url(m_targetUrl) << "| local:" << localUrl.toString(QUrl::FullyEncoded)
            << "| serverId:" << (m_serverId.isEmpty() ? QStringLiteral("<none>") : m_serverId)
            << "| proxyType:" << proxy.type();

    return localUrl;
}

void MpvHttpStreamRelay::stop()
{
    ++m_relayGeneration;
    const auto sockets = m_connections.keys();
    for (QTcpSocket *socket : sockets)
    {
        closeConnection(socket);
    }
    if (m_server->isListening())
    {
        m_server->close();
    }
    if (m_speedTimer->isActive())
    {
        m_speedTimer->stop();
        m_bytesRelayedSinceLastTick = 0;
        Q_EMIT upstreamSpeedChanged(0);
    }
    m_connections.clear();
    m_activeSocket = nullptr;
    m_network->clearConnectionCache();
    m_targetUrl.clear();
    m_resolvedTargetUrl.clear();
    m_serverId.clear();
    m_streamToken.clear();
    m_terminalHttpStatus = 0;
    m_hasStartedSuccessfulMediaResponse = false;
}

void MpvHttpStreamRelay::onNewConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection())
    {
        m_connections.insert(socket, ConnectionState{});
        QPointer<QTcpSocket> safeSocket(socket);
        connect(socket, &QTcpSocket::readyRead, this,
                [this, safeSocket]()
                {
                    if (safeSocket)
                    {
                        onSocketReadyRead(safeSocket.data());
                    }
                });
        connect(socket, &QTcpSocket::bytesWritten, this,
                [this, safeSocket](qint64)
                {
                    if (safeSocket)
                    {
                        pumpReplyToSocket(safeSocket.data());
                    }
                });
        connect(socket, &QTcpSocket::disconnected, this,
                [this, safeSocket]()
                {
                    if (safeSocket)
                    {
                        onSocketDisconnected(safeSocket.data());
                    }
                });
    }
}

void MpvHttpStreamRelay::onSocketReadyRead(QTcpSocket *socket)
{
    if (!socket || !socket->isOpen())
    {
        closeConnection(socket);
        return;
    }

    auto it = m_connections.find(socket);
    if (it == m_connections.end() || it->requestReady || it->reply)
    {
        return;
    }

    it->buffer += socket->readAll();
    const int headerEnd = it->buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
    {
        if (it->buffer.size() > 64 * 1024)
        {
            writeError(socket, 431, "Request header too large");
        }
        return;
    }

    const QByteArray requestData = it->buffer.left(headerEnd + 4);
    processRequest(socket, requestData);
}

void MpvHttpStreamRelay::onSocketDisconnected(QTcpSocket *socket)
{
    closeConnection(socket);
}

void MpvHttpStreamRelay::processRequest(QTcpSocket *socket, const QByteArray &requestData)
{
    if (m_targetUrl.isEmpty() || m_streamToken.isEmpty())
    {
        writeError(socket, 503, "Relay target is not ready");
        return;
    }

    const QList<QByteArray> lines = requestData.split('\n');
    if (lines.isEmpty())
    {
        writeError(socket, 400, "Bad request");
        return;
    }

    const QList<QByteArray> requestParts = lines.first().trimmed().split(' ');
    if (requestParts.size() < 2)
    {
        writeError(socket, 400, "Bad request");
        return;
    }

    const QByteArray method = requestParts.at(0).toUpper();
    const QByteArray path = requestParts.at(1);
    const QByteArray expectedPrefix = QByteArray("/") + m_streamToken.toUtf8() + QByteArray("/");
    if (!(method == "GET" || method == "HEAD") || !path.startsWith(expectedPrefix))
    {
        writeError(socket, 404, "Not found");
        return;
    }

    // A previous response can already have final metadata while its signal is
    // still queued. Capture it before choosing the target for this Range.
    const auto currentSockets = m_connections.keys();
    for (QTcpSocket *currentSocket : currentSockets)
    {
        const auto current = m_connections.constFind(currentSocket);
        if (current != m_connections.constEnd() && current->reply)
        {
            rememberResolvedTarget(current->reply,
                                   current->relayGeneration);
        }
    }

    const bool usingResolvedTarget = !m_resolvedTargetUrl.isEmpty();
    QNetworkRequest request(usingResolvedTarget ? m_resolvedTargetUrl
                                                : m_targetUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setRawHeader(QByteArrayLiteral("Connection"),
                         QByteArrayLiteral("close"));

    for (int i = 1; i < lines.size(); ++i)
    {
        QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
        {
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon <= 0)
        {
            continue;
        }

        QByteArray name = line.left(colon).trimmed();
        const QByteArray value = line.mid(colon + 1).trimmed();
        const QByteArray lowerName = name.toLower();
        if (lowerName == "host" || isHopByHopHeader(lowerName))
        {
            continue;
        }
        if (lowerName == "range" || lowerName == "user-agent" || lowerName == "accept" || lowerName == "icy-metadata")
        {
            request.setRawHeader(name, value);
        }
    }

    auto it = m_connections.find(socket);
    if (it == m_connections.end())
    {
        return;
    }
    UserAgentManager::instance()->applyToRequest(request, m_serverId);

    it->request = request;
    it->headOnly = method == "HEAD";
    it->requestReady = true;
    it->usingResolvedTarget = usingResolvedTarget;
    it->relayGeneration = m_relayGeneration;
    it->requestGeneration = ++m_latestRequestGeneration;
    const quint64 relayGeneration = it->relayGeneration;
    const quint64 requestGeneration = it->requestGeneration;

    const auto existingSockets = m_connections.keys();
    for (QTcpSocket *existingSocket : existingSockets)
    {
        if (existingSocket != socket)
        {
            qDebug() << "[MpvHttpStreamRelay] superseding previous upstream "
                        "request"
                     << "| newRange:" << request.rawHeader("Range");
            closeConnection(existingSocket);
        }
    }

    qDebug() << "[MpvHttpStreamRelay] queued request"
             << "| method:" << method << "| target:" << LogRedactionUtils::url(m_targetUrl)
             << "| range:" << request.rawHeader("Range")
             << "| relayGeneration:" << relayGeneration
             << "| requestGeneration:" << requestGeneration;

    scheduleUpstreamStart(socket);
}

void MpvHttpStreamRelay::scheduleUpstreamStart(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || !it->requestReady || it->reply)
    {
        return;
    }

    const quint64 relayGeneration = it->relayGeneration;
    const quint64 requestGeneration = it->requestGeneration;
    const qint64 remainingMs = qMax<qint64>(0, m_upstreamNotBeforeMs - m_lifecycleClock.elapsed());
    if (remainingMs == 0)
    {
        startUpstream(socket, relayGeneration, requestGeneration);
        return;
    }

    qDebug() << "[MpvHttpStreamRelay] delaying upstream request"
             << "| delayMs:" << remainingMs
             << "| range:" << it->request.rawHeader("Range")
             << "| relayGeneration:" << relayGeneration
             << "| requestGeneration:" << requestGeneration;

    QPointer<QTcpSocket> safeSocket(socket);
    QTimer::singleShot(static_cast<int>(remainingMs), this,
                       [this, safeSocket, relayGeneration, requestGeneration]()
                       {
                           if (safeSocket)
                           {
                               startUpstream(safeSocket.data(), relayGeneration,
                                             requestGeneration);
                           }
                       });
}

void MpvHttpStreamRelay::startUpstream(QTcpSocket *socket,
                                       quint64 relayGeneration,
                                       quint64 requestGeneration)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || !it->requestReady || it->reply ||
        it->relayGeneration != relayGeneration ||
        it->requestGeneration != requestGeneration ||
        relayGeneration != m_relayGeneration ||
        requestGeneration != m_latestRequestGeneration ||
        m_targetUrl.isEmpty() || m_streamToken.isEmpty())
    {
        return;
    }

    if (!canWriteToSocket(socket))
    {
        closeConnection(socket);
        return;
    }

    if (m_lifecycleClock.elapsed() < m_upstreamNotBeforeMs)
    {
        scheduleUpstreamStart(socket);
        return;
    }

    if (m_activeSocket && m_activeSocket != socket)
    {
        qWarning() << "[MpvHttpStreamRelay] replacing unexpected active "
                      "upstream request";
        QTcpSocket *activeSocket = m_activeSocket;
        closeConnection(activeSocket);
        scheduleUpstreamStart(socket);
        return;
    }

    it->headersSent = false;
    it->upstreamFinished = false;
    it->reply = it->headOnly ? m_network->head(it->request)
                             : m_network->get(it->request);
    it->reply->setReadBufferSize(kReplyReadBufferBytes);
    m_activeSocket = socket;

    qDebug() << "[MpvHttpStreamRelay] upstream request"
             << "| method:" << (it->headOnly ? "HEAD" : "GET")
             << "| targetHost:" << it->request.url().host()
             << "| targetSource:"
             << (it->usingResolvedTarget ? "resolved" : "original")
             << "| range:" << it->request.rawHeader("Range")
             << "| retry:" << it->retryCount
             << "| relayGeneration:" << relayGeneration
             << "| requestGeneration:" << requestGeneration;

    QNetworkReply *reply = it->reply;
    QPointer<QTcpSocket> safeSocket(socket);
    QPointer<QNetworkReply> safeReply(reply);
    connect(reply, &QNetworkReply::readyRead, this,
            [this, safeSocket, safeReply, relayGeneration,
             requestGeneration]()
            {
                if (!safeSocket || !safeReply)
                {
                    return;
                }
                QTcpSocket *socket = safeSocket.data();
                QNetworkReply *reply = safeReply.data();
                auto it = m_connections.find(socket);
                if (it == m_connections.end() || it->reply != reply ||
                    it->relayGeneration != relayGeneration ||
                    it->requestGeneration != requestGeneration)
                {
                    return;
                }
                if (!canWriteToSocket(socket))
                {
                    closeConnection(socket);
                    return;
                }

                pumpReplyToSocket(socket);
            });
    connect(reply, &QNetworkReply::metaDataChanged, this,
            [this, safeSocket, safeReply, relayGeneration,
             requestGeneration]()
            {
                if (!safeSocket || !safeReply)
                {
                    return;
                }
                const auto it = m_connections.constFind(safeSocket.data());
                if (it == m_connections.constEnd() ||
                    it->reply != safeReply.data() ||
                    it->relayGeneration != relayGeneration ||
                    it->requestGeneration != requestGeneration)
                {
                    return;
                }
                rememberResolvedTarget(safeReply.data(), relayGeneration);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, safeSocket, safeReply, relayGeneration,
             requestGeneration]()
            {
                if (!safeSocket || !safeReply)
                {
                    return;
                }
                QTcpSocket *socket = safeSocket.data();
                QNetworkReply *reply = safeReply.data();
                handleUpstreamFinished(socket, reply, relayGeneration,
                                       requestGeneration);
            });
}

void MpvHttpStreamRelay::handleUpstreamFinished(
    QTcpSocket *socket, QNetworkReply *reply, quint64 relayGeneration,
    quint64 requestGeneration)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || it->reply != reply ||
        it->relayGeneration != relayGeneration ||
        it->requestGeneration != requestGeneration)
    {
        return;
    }

    if (m_activeSocket == socket)
    {
        m_activeSocket = nullptr;
    }
    it->upstreamFinished = true;

    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    rememberResolvedTarget(reply, relayGeneration);

    const bool shouldRefreshResolvedTarget =
        (statusCode == 401 || statusCode == 403) && !it->headersSent &&
        it->usingResolvedTarget && !it->resolvedTargetRefreshAttempted;
    if (shouldRefreshResolvedTarget)
    {
        const QUrl expiredTarget = it->request.url();
        if (m_resolvedTargetUrl == expiredTarget)
        {
            m_resolvedTargetUrl.clear();
        }

        it->request.setUrl(m_targetUrl);
        it->usingResolvedTarget = false;
        it->resolvedTargetRefreshAttempted = true;
        it->reply = nullptr;
        it->upstreamFinished = false;
        disconnect(reply, nullptr, this, nullptr);
        reply->deleteLater();
        m_network->clearConnectionCache();

        qWarning() << "[MpvHttpStreamRelay] cached upstream URL rejected; "
                      "refreshing through original target"
                   << "| status:" << statusCode
                   << "| expiredHost:" << expiredTarget.host()
                   << "| relayGeneration:" << relayGeneration
                   << "| requestGeneration:" << requestGeneration;
        scheduleUpstreamStart(socket);
        return;
    }

    if (statusCode == 429 && !it->headersSent &&
        it->retryCount < kMaxRateLimitRetries)
    {
        if (!m_resolvedTargetUrl.isEmpty() &&
            it->request.url() != m_resolvedTargetUrl &&
            reply->url() == m_resolvedTargetUrl)
        {
            it->request.setUrl(m_resolvedTargetUrl);
            it->usingResolvedTarget = true;
            qInfo() << "[MpvHttpStreamRelay] retrying rate-limited response "
                       "through cached resolved URL"
                    << "| host:" << m_resolvedTargetUrl.host()
                    << "| relayGeneration:" << relayGeneration
                    << "| requestGeneration:" << requestGeneration;
        }

        const int retryNumber = ++it->retryCount;
        const int delayMs = retryDelayMs(reply, retryNumber);
        const QByteArray range = it->request.rawHeader("Range");
        const QString responseHost = reply->url().host();

        it->reply = nullptr;
        it->upstreamFinished = false;
        disconnect(reply, nullptr, this, nullptr);
        reply->deleteLater();
        m_network->clearConnectionCache();
        delayFutureUpstreamRequests(delayMs);

        qWarning() << "[MpvHttpStreamRelay] upstream rate limited; retrying "
                      "without exposing 429 to MPV"
                   << "| retry:" << retryNumber << "/"
                   << kMaxRateLimitRetries << "| delayMs:" << delayMs
                   << "| responseHost:" << responseHost
                   << "| range:" << range
                   << "| relayGeneration:" << relayGeneration
                   << "| requestGeneration:" << requestGeneration;
        scheduleUpstreamStart(socket);
        return;
    }

    if (reply->error() != QNetworkReply::NoError && statusCode == 0)
    {
        qWarning() << "[MpvHttpStreamRelay] upstream failed"
                   << "| targetHost:" << it->request.url().host()
                   << "| error:" << reply->errorString();
    }

    pumpReplyToSocket(socket);
}

void MpvHttpStreamRelay::rememberResolvedTarget(
    QNetworkReply *reply, quint64 relayGeneration)
{
    if (!reply || relayGeneration != m_relayGeneration ||
        m_targetUrl.isEmpty())
    {
        return;
    }

    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // A redirect followed by 429 still reveals the signed CDN URL. Reusing it
    // prevents each retry from asking the media server for another link.
    if ((statusCode < 200 || statusCode >= 300) && statusCode != 429)
    {
        return;
    }

    QUrl resolvedTarget = reply->url();
    resolvedTarget.setFragment({});
    const QString scheme = resolvedTarget.scheme().toLower();
    if (!resolvedTarget.isValid() || resolvedTarget.host().isEmpty() ||
        (scheme != QStringLiteral("http") &&
         scheme != QStringLiteral("https")) ||
        resolvedTarget == m_targetUrl ||
        resolvedTarget == m_resolvedTargetUrl)
    {
        return;
    }

    m_resolvedTargetUrl = resolvedTarget;
    qInfo() << "[MpvHttpStreamRelay] cached resolved upstream URL"
            << "| host:" << m_resolvedTargetUrl.host()
            << "| relayGeneration:" << relayGeneration;
}

void MpvHttpStreamRelay::sendReplyHeaders(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || !it->reply || it->headersSent || !canWriteToSocket(socket))
    {
        return;
    }

    QNetworkReply *reply = it->reply;
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 0)
    {
        if (!reply->isFinished())
        {
            return;
        }
        statusCode = 502;
    }

    qDebug() << "[MpvHttpStreamRelay] response"
             << "| status:" << statusCode << "| contentRange:" << reply->rawHeader("Content-Range")
             << "| contentLength:" << reply->rawHeader("Content-Length")
             << "| acceptRanges:" << reply->rawHeader("Accept-Ranges");

    QByteArray response = "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase(statusCode) + "\r\n";
    const auto headerPairs = reply->rawHeaderPairs();
    for (const auto &pair : headerPairs)
    {
        QByteArray name = pair.first;
        const QByteArray lowerName = name.toLower();
        if (isHopByHopHeader(lowerName))
        {
            continue;
        }
        response += name + ": " + pair.second + "\r\n";
    }
    response += "Connection: close\r\n";
    response += "\r\n";

    if (socket->write(response) >= 0)
    {
        it->headersSent = true;
        m_terminalHttpStatus = statusCode >= 400 ? statusCode : 0;
        if (!it->headOnly && statusCode >= 200 && statusCode < 300)
        {
            m_hasStartedSuccessfulMediaResponse = true;
        }
    }
}

void MpvHttpStreamRelay::pumpReplyToSocket(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || !it->reply)
    {
        return;
    }
    if (!canWriteToSocket(socket))
    {
        closeConnection(socket);
        return;
    }

    QNetworkReply *reply = it->reply;
    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool awaitingResolvedTargetRefresh =
        (statusCode == 401 || statusCode == 403) && !it->headersSent &&
        it->usingResolvedTarget && !it->resolvedTargetRefreshAttempted;
    if (awaitingResolvedTargetRefresh)
    {
        return;
    }
    if (statusCode == 429 && !it->headersSent &&
        it->retryCount < kMaxRateLimitRetries)
    {
        return;
    }

    sendReplyHeaders(socket);
    it = m_connections.find(socket);
    if (it == m_connections.end() || it->reply != reply || !it->headersSent)
    {
        return;
    }

    if (!it->headOnly)
    {
        while (reply->bytesAvailable() > 0 && socket->bytesToWrite() < kSocketQueuedBytesHighWater)
        {
            const qint64 budget = kSocketQueuedBytesHighWater - socket->bytesToWrite();
            const qint64 readSize = qMin(kRelayPumpChunkBytes, qMin(budget, reply->bytesAvailable()));
            if (readSize <= 0)
            {
                break;
            }

            const QByteArray chunk = reply->read(readSize);
            if (chunk.isEmpty())
            {
                break;
            }

            const qint64 written = socket->write(chunk);
            if (written > 0)
            {
                recordRelayedBytes(written);
            }
            if (written < static_cast<qint64>(chunk.size()))
            {
                break;
            }

            it = m_connections.find(socket);
            if (it == m_connections.end() || it->reply != reply || !canWriteToSocket(socket))
            {
                return;
            }
        }
    }

    it = m_connections.find(socket);
    if (it == m_connections.end() || it->reply != reply)
    {
        return;
    }

    const bool allReplyDataDrained =
        it->headOnly || reply->bytesAvailable() == 0;
    if (!it->upstreamFinished || !allReplyDataDrained)
    {
        return;
    }

    it->reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();

    if (socket->state() != QAbstractSocket::UnconnectedState)
    {
        socket->flush();
        socket->disconnectFromHost();
    }
    else
    {
        closeConnection(socket);
    }
}

void MpvHttpStreamRelay::writeError(QTcpSocket *socket, int statusCode, const QByteArray &message)
{
    if (!socket)
    {
        return;
    }

    const QByteArray body = message + "\n";
    QByteArray response = "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase(statusCode) + "\r\n";
    response += "Content-Type: text/plain; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    if (canWriteToSocket(socket))
    {
        socket->write(response);
        socket->disconnectFromHost();
    }
    else
    {
        closeConnection(socket);
    }
}

void MpvHttpStreamRelay::closeConnection(QTcpSocket *socket)
{
    if (!socket)
    {
        return;
    }

    auto it = m_connections.find(socket);
    if (it != m_connections.end())
    {
        QNetworkReply *reply = it->reply;
        it->reply = nullptr;
        m_connections.erase(it);

        if (m_activeSocket == socket)
        {
            m_activeSocket = nullptr;
        }

        if (reply)
        {
            disconnect(reply, nullptr, this, nullptr);
            if (!reply->isFinished())
            {
                reply->abort();
                delayFutureUpstreamRequests(kUpstreamHandoffDelayMs);
                m_network->clearConnectionCache();
            }
            reply->deleteLater();
        }
    }

    disconnect(socket, nullptr, this, nullptr);
    if (socket->state() != QAbstractSocket::UnconnectedState)
    {
        socket->abort();
    }
    socket->deleteLater();
}

void MpvHttpStreamRelay::recordRelayedBytes(qint64 bytes)
{
    if (bytes > 0)
    {
        m_bytesRelayedSinceLastTick += bytes;
    }
}

void MpvHttpStreamRelay::delayFutureUpstreamRequests(int delayMs)
{
    if (delayMs <= 0)
    {
        return;
    }

    m_upstreamNotBeforeMs =
        qMax(m_upstreamNotBeforeMs, m_lifecycleClock.elapsed() + delayMs);
}

int MpvHttpStreamRelay::retryDelayMs(const QNetworkReply *reply,
                                     int retryNumber) const
{
    const QByteArray retryAfter = reply->rawHeader("Retry-After").trimmed();
    if (!retryAfter.isEmpty())
    {
        bool secondsOk = false;
        const qlonglong seconds = retryAfter.toLongLong(&secondsOk);
        if (secondsOk && seconds >= 0)
        {
            const qlonglong delayMs =
                seconds > kRateLimitRetryMaxDelayMs / 1000
                    ? kRateLimitRetryMaxDelayMs
                    : seconds * 1000;
            return qBound(kRateLimitRetryBaseDelayMs,
                          static_cast<int>(delayMs),
                          kRateLimitRetryMaxDelayMs);
        }

        const QDateTime retryAt = QDateTime::fromString(
            QString::fromLatin1(retryAfter), Qt::RFC2822Date);
        if (retryAt.isValid())
        {
            const qint64 delayMs =
                QDateTime::currentDateTimeUtc().msecsTo(retryAt.toUTC());
            return qBound(kRateLimitRetryBaseDelayMs,
                          static_cast<int>(qMin<qint64>(
                              delayMs, kRateLimitRetryMaxDelayMs)),
                          kRateLimitRetryMaxDelayMs);
        }
    }

    const int exponentialDelay =
        kRateLimitRetryBaseDelayMs << qMax(0, retryNumber - 1);
    return qMin(exponentialDelay, kRateLimitRetryMaxDelayMs);
}

QByteArray MpvHttpStreamRelay::reasonPhrase(int statusCode)
{
    switch (statusCode)
    {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 429:
        return "Too Many Requests";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 431:
        return "Request Header Fields Too Large";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    default:
        return "Status";
    }
}

bool MpvHttpStreamRelay::isHopByHopHeader(QByteArray name)
{
    name = name.toLower();
    return name == "connection" || name == "keep-alive" || name == "proxy-connection" ||
           name == "proxy-authenticate" ||
           name == "proxy-authorization" || name == "te" || name == "trailer" || name == "transfer-encoding" ||
           name == "upgrade";
}
