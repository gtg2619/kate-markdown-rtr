#pragma once
#include <QObject>
#include <QImage>
#include <QCache>
#include <QNetworkAccessManager>
#include <QSet>
#include <QQueue>
#include <QUrl>

namespace MarkdownLive {
struct MediaResult { QImage image; QString error; };
class MediaStore : public QObject
{
    Q_OBJECT
public:
    explicit MediaStore(QObject *parent = nullptr);
    ~MediaStore() override;
    const MediaResult *request(const QUrl &url);
    void setRemoteEnabled(bool enabled);
    bool remoteEnabled() const { return m_remote; }
    int pendingCount() const { return m_pending.size(); }
    int cacheCost() const { return m_cache.totalCost(); }
    static MediaResult decode(const QByteArray &data);
Q_SIGNALS:
    void ready();
private:
    void dispatch();
    void cache(const QString &key, MediaResult result);
    QNetworkAccessManager m_network;
    QCache<QString, MediaResult> m_cache{16 * 1024}; // KiB.
    QQueue<QUrl> m_queue;
    QSet<QString> m_pending;
    bool m_remote = false;
    int m_active = 0;
    quint64 m_generation = 0;
    MediaResult m_disabled{{}, QStringLiteral("Remote image loading is disabled")};
};
}
