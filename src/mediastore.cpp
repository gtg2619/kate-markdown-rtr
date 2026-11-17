#include "mediastore.h"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QNetworkReply>
#include <QDateTime>
#include <memory>

namespace MarkdownLive {
namespace { constexpr qsizetype MaxFile = 8 * 1024 * 1024; }
MediaStore::MediaStore(QObject *parent) : QObject(parent) {}
MediaStore::~MediaStore()
{
    for (auto reply : m_network.findChildren<QNetworkReply *>()) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }
}

MediaResult MediaStore::decode(const QByteArray &data)
{
    if (data.isEmpty() || data.size() > MaxFile) return {{}, QStringLiteral("Image is empty or exceeds 8 MiB")};
    QBuffer buffer;
    buffer.setData(data); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 16 * 1024 * 1024)
        return {{}, QStringLiteral("Image exceeds 16 megapixels or has an invalid header")};
    const auto target = size.width() > 1600 || size.height() > 1200 ? size.scaled(1600, 1200, Qt::KeepAspectRatio) : size;
    reader.setScaledSize(target);
    auto image = reader.read();
    if (image.isNull()) return {{}, QStringLiteral("Image could not be decoded")};
    if (image.width() > 1600 || image.height() > 1200) image = image.scaled(1600, 1200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return {image, {}};
}

void MediaStore::cache(const QString &key, MediaResult result)
{
    const int cost = qMax(1, int(result.image.sizeInBytes() / 1024) + 1);
    m_cache.insert(key, new MediaResult(std::move(result)), cost);
}

const MediaResult *MediaStore::request(const QUrl &url)
{
    QString key = url.toString(QUrl::FullyEncoded);
    if (url.isLocalFile()) {
        const QFileInfo info(url.toLocalFile());
        key += QStringLiteral("|%1|%2").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
        if (auto item = m_cache.object(key)) return item;
        QFile file(url.toLocalFile());
        MediaResult result;
        if (!info.isFile() || info.size() > MaxFile || !file.open(QIODevice::ReadOnly)) result.error = QStringLiteral("Local image is unavailable or exceeds 8 MiB");
        else result = decode(file.read(MaxFile + 1));
        cache(key, result);
        return m_cache.object(key);
    }
    if (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) {
        if (!m_cache.contains(key)) cache(key, {{}, QStringLiteral("Unsupported image URL")});
        return m_cache.object(key);
    }
    if (!m_remote) return &m_disabled;
    if (auto item = m_cache.object(key)) return item;
    if (!m_pending.contains(key) && m_pending.size() < 32) {
        m_pending.insert(key); m_queue.enqueue(url); dispatch();
    }
    return nullptr;
}

void MediaStore::setRemoteEnabled(bool enabled)
{
    if (m_remote == enabled) return;
    m_remote = enabled;
    ++m_generation;
    m_cache.clear();
    if (!enabled) {
        m_queue.clear(); m_pending.clear();
        for (auto reply : m_network.findChildren<QNetworkReply *>()) reply->abort();
    }
    Q_EMIT ready();
}

void MediaStore::dispatch()
{
    while (m_remote && m_active < 4 && !m_queue.isEmpty()) {
        const auto url = m_queue.dequeue();
        QNetworkRequest request(url);
        request.setTransferTimeout(10000);
        request.setMaximumRedirectsAllowed(5);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        auto reply = m_network.get(request);
        const auto generation = m_generation;
        reply->setReadBufferSize(64 * 1024);
        ++m_active;
        auto bytes = std::make_shared<QByteArray>();
        connect(reply, &QIODevice::readyRead, this, [reply, bytes] {
            bytes->append(reply->read(64 * 1024));
            if (bytes->size() > MaxFile) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, url, generation] {
            bytes->append(reply->read(MaxFile + 1 - qMin(MaxFile, bytes->size())));
            const auto result = reply->error() == QNetworkReply::NoError ? decode(*bytes)
                : MediaResult{{}, QStringLiteral("Remote image download failed")};
            const auto key = url.toString(QUrl::FullyEncoded);
            if (generation == m_generation) { cache(key, result); m_pending.remove(key); }
            --m_active;
            reply->deleteLater();
            Q_EMIT ready(); dispatch();
        });
    }
}
}
