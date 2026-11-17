#pragma once
#include <QObject>
#include <QCache>
#include <QQueue>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>

class QJSEngine;
namespace MarkdownLive {
struct MathResult {
    QByteArray svg;
    double width = 0, height = 0, depth = 0; // In ex units.
    QString error;
};
struct MathRuntime { std::atomic<QJSEngine *> engine{nullptr}; std::atomic_bool stopping{false}; };
class MathWorker;

class MathRenderer : public QObject
{
    Q_OBJECT
public:
    explicit MathRenderer(QObject *parent = nullptr);
    ~MathRenderer() override;
    const MathResult *request(const QString &source, bool display);
    int pendingCount() const { return m_pending.size(); }
    int cacheCost() const { return m_cache.totalCost(); }
    static QString key(const QString &source, bool display);
Q_SIGNALS:
    void ready();
private:
    void dispatch();
    QThread m_thread;
    MathWorker *m_worker = nullptr;
    QCache<QString, MathResult> m_cache{8192}; // KiB, including error results.
    QQueue<QPair<QString, bool>> m_queue;
    QSet<QString> m_pending;
    std::shared_ptr<MathRuntime> m_runtime;
    QTimer m_deadline;
    bool m_busy = false;
};
}
Q_DECLARE_METATYPE(MarkdownLive::MathResult)
