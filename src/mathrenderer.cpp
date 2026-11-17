#include "mathrenderer.h"
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <cmath>

static void initializeMathResources() { Q_INIT_RESOURCE(math_assets); }

namespace MarkdownLive {
class MathWorker : public QObject
{
    Q_OBJECT
public:
    explicit MathWorker(std::shared_ptr<MathRuntime> runtime) : m_runtime(std::move(runtime)) {}
    ~MathWorker() override { m_runtime->engine.store(nullptr); }
    void convert(const QString &source, bool display)
    {
        if (!m_engine) {
            m_engine = std::make_unique<QJSEngine>();
            m_runtime->engine.store(m_engine.get());
            QFile bundle(QStringLiteral(":/markdownlive/third_party/mathjax/engine.js"));
            if (!bundle.open(QIODevice::ReadOnly)) m_error = QStringLiteral("MathJax bundle unavailable");
            else {
                const auto loaded = m_engine->evaluate(QString::fromUtf8(bundle.readAll()), QStringLiteral("mathjax.js"));
                if (loaded.isError()) m_error = loaded.toString();
                m_function = m_engine->globalObject().property(QStringLiteral("KateMath")).property(QStringLiteral("render"));
                if (!m_function.isCallable() && m_error.isEmpty()) m_error = QStringLiteral("MathJax initialization failed");
            }
        }
        MathResult result;
        if (!m_error.isEmpty()) result.error = m_error;
        else {
            m_engine->setInterrupted(false);
            if (m_runtime->stopping.load()) m_engine->setInterrupted(true);
            Q_EMIT started();
            const auto value = m_function.call({QJSValue(source), QJSValue(display)});
            if (value.isError() || m_engine->isInterrupted()) result.error = QStringLiteral("Formula could not be rendered within the time limit");
            else {
                result.svg = value.property(QStringLiteral("svg")).toString().toUtf8();
                result.width = value.property(QStringLiteral("width")).toNumber();
                result.height = value.property(QStringLiteral("height")).toNumber();
                result.depth = value.property(QStringLiteral("depth")).toNumber();
                result.error = value.property(QStringLiteral("error")).toString();
                if (result.error.isEmpty() && (result.svg.size() > 1024 * 1024 || !std::isfinite(result.width)
                    || !std::isfinite(result.height) || result.width <= 0 || result.height <= 0
                    || result.width > 2000 || result.height > 1000)) {
                    result.svg.clear(); result.error = QStringLiteral("Formula exceeds rendering limits");
                }
            }
        }
        Q_EMIT converted(source, display, result);
    }
Q_SIGNALS:
    void started();
    void converted(const QString &, bool, const MarkdownLive::MathResult &);
private:
    std::shared_ptr<MathRuntime> m_runtime;
    std::unique_ptr<QJSEngine> m_engine;
    QJSValue m_function;
    QString m_error;
};

MathRenderer::MathRenderer(QObject *parent) : QObject(parent), m_runtime(std::make_shared<MathRuntime>())
{
    initializeMathResources();
    qRegisterMetaType<MathResult>();
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(300);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (auto engine = m_runtime->engine.load()) engine->setInterrupted(true);
    });
}

MathRenderer::~MathRenderer()
{
    m_deadline.stop();
    m_runtime->stopping.store(true);
    if (m_worker) {
        if (auto engine = m_runtime->engine.load()) engine->setInterrupted(true);
        m_thread.quit();
        m_thread.wait();
    }
}

QString MathRenderer::key(const QString &source, bool display)
{
    return (display ? QStringLiteral("D:") : QStringLiteral("I:")) + source;
}

const MathResult *MathRenderer::request(const QString &source, bool display)
{
    const auto id = key(source, display);
    if (const auto cached = m_cache.object(id)) return cached;
    if (source.size() > 8192) {
        auto error = new MathResult;
        error->error = QStringLiteral("Formula exceeds 8192 characters");
        m_cache.insert(id, error, qMax(1, int(id.size() * 2) / 1024 + 1));
        return error;
    }
    if (!m_pending.contains(id) && m_pending.size() < 64) {
        m_pending.insert(id);
        m_queue.enqueue({source, display});
        dispatch();
    }
    return nullptr;
}

void MathRenderer::dispatch()
{
    if (m_busy || m_queue.isEmpty()) return;
    if (!m_worker) {
        m_worker = new MathWorker(m_runtime);
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(m_worker, &MathWorker::started, this, [this] { m_deadline.start(); });
        connect(m_worker, &MathWorker::converted, this, [this](const QString &source, bool display, const MathResult &result) {
            m_deadline.stop();
            const auto id = key(source, display);
            m_pending.remove(id);
            m_cache.insert(id, new MathResult(result), qMax(1, int(result.svg.size() + id.size() * 2) / 1024 + 1));
            m_busy = false;
            Q_EMIT ready();
            dispatch();
        });
        m_thread.setObjectName(QStringLiteral("MarkdownMath"));
        m_thread.start();
    }
    m_busy = true;
    const auto job = m_queue.dequeue();
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, job] { worker->convert(job.first, job.second); }, Qt::QueuedConnection);
}
}
#include "mathrenderer.moc"
