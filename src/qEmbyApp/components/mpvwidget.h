#ifndef MPVWIDGET_H
#define MPVWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QString>
#include <mpv/render_gl.h>
#include "mpvcontroller.h"

class MpvHttpStreamRelay;

class MpvWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit MpvWidget(QWidget *parent = nullptr);
    ~MpvWidget() override;

    
    void shutdown();

    void loadMedia(const QString &url, const QString &serverId = QString(),
                   bool forceRelay = false);
    bool retryWithRelay();
    bool startupRateLimitedBeforeMedia() const;
    void play();
    void pause();
    void stop();
    void seek(double positionInSeconds);

    MpvController* controller() const { return m_controller; }

signals:
    
    void positionChanged(double position);
    void durationChanged(double duration);
    void playbackStateChanged(bool isPaused);
    void networkSpeedChanged(qint64 bytesPerSecond);
    void relayActiveChanged(bool active);
    void errorOccurred(const QString &errorMsg);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private slots:
    void cleanupGL(); 
    void maybeUpdate(); 

private:
    static void onMpvRenderUpdate(void *ctx);
    static void *getProcAddress(void *ctx, const char *name);
    void loadMediaNow(const QString &url, const QString &serverId,
                      bool wasPending, bool forceRelay = false);

    MpvController *m_controller;
    MpvHttpStreamRelay *m_streamRelay = nullptr;
    bool m_usingStreamRelay = false;
    bool m_relayRetryAttempted = false;
    mpv_render_context *m_mpv_gl;

    
    QString m_pendingUrl;
    QString m_pendingServerId;
    bool m_pendingForceRelay = false;
    QString m_currentUrl;
    QString m_currentServerId;
};

#endif 
