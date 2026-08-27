#ifndef MPVHTTPSTREAMRELAY_H
#define MPVHTTPSTREAMRELAY_H

#include <QElapsedTimer>
#include <QHash>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QTcpServer;
class QTcpSocket;

class MpvHttpStreamRelay : public QObject {
    Q_OBJECT
public:
    explicit MpvHttpStreamRelay(QObject *parent = nullptr);
    ~MpvHttpStreamRelay() override;

    QUrl prepare(const QUrl &targetUrl, const QString &serverId,
                 const QNetworkProxy &proxy);
    void stop();
    int terminalHttpStatus() const { return m_terminalHttpStatus; }
    bool hasStartedSuccessfulMediaResponse() const
    {
        return m_hasStartedSuccessfulMediaResponse;
    }

Q_SIGNALS:
    void upstreamSpeedChanged(qint64 bytesPerSecond);

private:
    struct ConnectionState {
        QByteArray buffer;
        QNetworkRequest request;
        QNetworkReply *reply = nullptr;
        quint64 relayGeneration = 0;
        quint64 requestGeneration = 0;
        int retryCount = 0;
        bool requestReady = false;
        bool usingResolvedTarget = false;
        bool resolvedTargetRefreshAttempted = false;
        bool headersSent = false;
        bool headOnly = false;
        bool upstreamFinished = false;
    };

    void onNewConnection();
    void onSocketReadyRead(QTcpSocket *socket);
    void onSocketDisconnected(QTcpSocket *socket);
    void processRequest(QTcpSocket *socket, const QByteArray &requestData);
    void scheduleUpstreamStart(QTcpSocket *socket);
    void startUpstream(QTcpSocket *socket, quint64 relayGeneration,
                       quint64 requestGeneration);
    void handleUpstreamFinished(QTcpSocket *socket, QNetworkReply *reply,
                                quint64 relayGeneration,
                                quint64 requestGeneration);
    void rememberResolvedTarget(QNetworkReply *reply,
                                quint64 relayGeneration);
    void sendReplyHeaders(QTcpSocket *socket);
    void pumpReplyToSocket(QTcpSocket *socket);
    void writeError(QTcpSocket *socket, int statusCode, const QByteArray &message);
    void closeConnection(QTcpSocket *socket);
    void recordRelayedBytes(qint64 bytes);
    void delayFutureUpstreamRequests(int delayMs);
    int retryDelayMs(const QNetworkReply *reply, int retryNumber) const;

    static QByteArray reasonPhrase(int statusCode);
    static bool isHopByHopHeader(QByteArray name);

    QTcpServer *m_server = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_speedTimer = nullptr;
    QHash<QTcpSocket *, ConnectionState> m_connections;
    QTcpSocket *m_activeSocket = nullptr;
    QUrl m_targetUrl;
    QUrl m_resolvedTargetUrl;
    QString m_serverId;
    QString m_streamToken;
    QElapsedTimer m_lifecycleClock;
    quint64 m_relayGeneration = 0;
    quint64 m_latestRequestGeneration = 0;
    // Keep the release deadline across stop()/prepare() target changes.
    qint64 m_upstreamNotBeforeMs = 0;
    qint64 m_bytesRelayedSinceLastTick = 0;
    int m_terminalHttpStatus = 0;
    bool m_hasStartedSuccessfulMediaResponse = false;
};

#endif 
