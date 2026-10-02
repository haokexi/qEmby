#ifndef MEDIASOURCEPREFERENCEUTILS_H
#define MEDIASOURCEPREFERENCEUTILS_H

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <models/media/playbackinfo.h>

namespace MediaSourcePreferenceUtils {

QStringList splitPreferredVersionRules(const QString &rawRules);

QList<int> preferredMediaSourceOrder(const QList<MediaSourceInfo> &mediaSources,
                                     const QString &rawRules);

int resolveNearestUnattemptedMediaSourceIndex(
    const QList<MediaSourceInfo> &mediaSources,
    const MediaSourceInfo &currentSource,
    const QSet<QString> &attemptedSourceIds);

int resolvePreferredMediaSourceIndex(const QList<MediaSourceInfo> &mediaSources,
                                     const QString &rawRules,
                                     const QString &rememberedSourceId = QString());

QString rememberedMediaSourceId(const QString &serverId, const QString &mediaId);
void rememberMediaSourceId(const QString &serverId, const QString &mediaId,
                           const QString &mediaSourceId);

} 

#endif 
