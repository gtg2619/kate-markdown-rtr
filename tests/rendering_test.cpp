#include "mathrenderer.h"
#include "mediastore.h"
#include "richdocument.h"
#include <QTest>
#include <QBuffer>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSvgRenderer>
#include <QPainter>
#include <QTextBlock>
#include <QTextImageFormat>

using namespace MarkdownLive;
class RenderingTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void formulas()
    {
        MathRenderer renderer;
        const QStringList sources = {
            "x^2+y^2=z^2", "\\frac{a+b}{\\sqrt{x}}", "\\sum_{n=1}^{\\infty}\\frac{1}{n^2}",
            "\\int_0^1 x^2\\,dx", "\\left(\\frac{a}{b}\\right)",
            "\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}",
            "\\begin{aligned}a&=b+c\\\\d&=e\\end{aligned}",
            "f(x)=\\begin{cases}x^2&x>0\\\\0&x\\le0\\end{cases}",
            "\\mathbf{A}\\boldsymbol{\\alpha}+\\mathbb{R}", "\\text{velocity}=\\frac{d}{t}"
        };
        QElapsedTimer elapsed; elapsed.start();
        for (const auto &source : sources) for (bool display : {false, true}) {
            QTRY_VERIFY_WITH_TIMEOUT(renderer.request(source, display), 15000);
            const auto result = *renderer.request(source, display);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QVERIFY(result.svg.contains("<svg"));
            QSvgRenderer svg(result.svg);
            QVERIFY(svg.isValid());
            QVERIFY(result.width > 0 && result.height > 0);
            QImage image(400, 200, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image); svg.render(&painter); painter.end();
            int ink = 0;
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) ink += qAlpha(image.pixel(x, y)) > 0;
            QVERIFY(ink > 20);
        }
        qInfo() << "20 MathJax formulas, including cold start:" << elapsed.elapsed() << "ms";
        QVERIFY(renderer.cacheCost() <= 8192);
    }
    void errorsAndIsolation()
    {
        MathRenderer renderer;
        for (const auto &source : {QString("\\notARealCommand{x}"), QString("\\frac{"),
            QString("\\def\\x{\\x}\\x"), QString(9000, u'x')}) {
            QTRY_VERIFY_WITH_TIMEOUT(renderer.request(source, false), 15000);
            QVERIFY(!renderer.request(source, false)->error.isEmpty());
        }
        const QString define = "\\newcommand{\\privateMacro}{x}\\privateMacro";
        QTRY_VERIFY_WITH_TIMEOUT(renderer.request(define, false), 15000);
        QVERIFY2(renderer.request(define, false)->error.isEmpty(), qPrintable(renderer.request(define, false)->error));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.request("\\privateMacro", false), 15000);
        QVERIFY(!renderer.request("\\privateMacro", false)->error.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(renderer.request("x+1", false), 15000);
        QVERIFY(renderer.request("x+1", false)->error.isEmpty());
    }
    void boundedQueueAndShutdown()
    {
        QElapsedTimer elapsed; elapsed.start();
        {
            MathRenderer renderer;
            for (int i = 0; i < 300; ++i) renderer.request(QString("x^{%1}").arg(i), false);
            QVERIFY(renderer.pendingCount() <= 64);
        }
        QVERIFY(elapsed.elapsed() < 5000);
    }
    void localImagesAndBrokenFiles()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QString::fromUtf8("\u56fe\u7247 space.png"));
        QImage original(1200, 800, QImage::Format_RGB32); original.fill(QColor("#24a464")); QVERIFY(original.save(path));
        MediaStore media;
        const auto result = media.request(QUrl::fromLocalFile(path));
        QVERIFY(result && !result->image.isNull());
        const auto broken = media.request(QUrl::fromLocalFile(directory.filePath("missing.png")));
        QVERIFY(broken && !broken->error.isEmpty());
        QVERIFY(!MediaStore::decode("not an image").error.isEmpty());
        QVERIFY(!MediaStore::decode(QByteArray(8 * 1024 * 1024 + 1, 'x')).error.isEmpty());
        QFile oversized(directory.filePath("too-large.png"));
        QVERIFY(oversized.open(QIODevice::WriteOnly)); QVERIFY(oversized.resize(9 * 1024 * 1024)); oversized.close();
        QVERIFY(!media.request(QUrl::fromLocalFile(oversized.fileName()))->error.isEmpty());
        MathRenderer math;
        RichDocument document(&math, &media, nullptr);
        QVERIFY(document.render(QStringLiteral("![image](%1)").arg(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)), {}, QPalette(), 300));
        const auto format = document.begin().begin().fragment().charFormat();
        QCOMPARE(document.findChild<RichObjects *>()->intrinsicSize(&document, 0, format), QSizeF(300, 200));
        QVERIFY(media.cacheCost() <= 16 * 1024);
    }
    void remoteImages()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QImage image(24, 16, QImage::Format_RGB32); image.fill(Qt::red);
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); QVERIFY(image.save(&buffer, "PNG"));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, png] {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " + QByteArray::number(png.size()) + "\r\nConnection: close\r\n\r\n" + png);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/test.png").arg(server.serverPort()));
        MediaStore media;
        QVERIFY(media.request(url));
        QCOMPARE(media.pendingCount(), 0);
        media.setRemoteEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(media.request(url), 5000);
        QCOMPARE(media.request(url)->image.size(), QSize(24, 16));
    }
    void repeatedImageLayoutKeepsResourceAvailable()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("photo.png");
        QImage original(1200, 800, QImage::Format_RGB32); original.fill(Qt::green);
        QVERIFY(original.save(path));
        MediaStore media; MathRenderer math;
        RichDocument document(&math, &media, nullptr);
        QVERIFY(document.render("![photo](photo.png)\n", QUrl::fromLocalFile(directory.path() + '/'), QPalette(), 600));
        const auto height = document.size().height();
        for (int i = 0; i < 80; ++i) {
            QImage canvas(640, 480, QImage::Format_RGB32); canvas.fill(Qt::white);
            QPainter painter(&canvas); painter.translate(0, -(i % 5) * 20);
            document.drawContents(&painter); painter.end();
            QCOMPARE(canvas.pixelColor(100, 100), QColor(Qt::green));
            QCOMPARE(document.size().height(), height);
            QVERIFY(media.cacheCost() <= 16 * 1024);
        }
    }

    void inlineAndDisplayDocument()
    {
        const auto parsed = parse("Text $\\frac{1}{2}$ after.\n\n$$\n\\sum_{n=1}^N n\n$$\n");
        MathRenderer math; MediaStore media;
        for (const auto &block : parsed.blocks) for (const auto &span : block.math)
            QTRY_VERIFY_WITH_TIMEOUT(math.request(span.source, span.display), 15000);
        RichDocument doc(&math,&media,nullptr); doc.setDefaultFont(QFont("DejaVu Sans",12));
        QVERIFY(doc.render("Text $\\frac{1}{2}$ after.\n\n$$\n\\sum_{n=1}^N n\n$$\n",{},QPalette(),600));
        int images = 0;
        for (auto b = doc.begin(); b.isValid(); b = b.next()) for (auto it = b.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            if (format.objectType() != RichObjects::Type) continue;
            ++images;
            const auto size = doc.findChild<RichObjects *>()->intrinsicSize(&doc,it.fragment().position(),format);
            QVERIFY(size.height()>10 && size.width()>2);
        }
        QCOMPARE(images,2);
        QVERIFY(!doc.toPlainText().contains("$"));
    }

    void cancelledRemoteRequestCanRetry()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        int connections = 0;
        QImage source(40, 30, QImage::Format_RGB32); source.fill(Qt::blue);
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); QVERIFY(source.save(&buffer, "PNG"));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto socket = server.nextPendingConnection();
            const int number = ++connections;
            connect(socket, &QTcpSocket::readyRead, socket, [socket, number, png] {
                socket->readAll();
                if (number == 1) return; // Simulate a stalled server while the user disables images.
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(png.size()) + "\r\nConnection: close\r\n\r\n" + png);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/cancel.png").arg(server.serverPort()));
        MediaStore media; media.setRemoteEnabled(true); media.request(url);
        QTRY_COMPARE(connections, 1);
        media.setRemoteEnabled(false);
        QCOMPARE(media.pendingCount(), 0);
        media.setRemoteEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(media.request(url), 5000);
        QCOMPARE(media.request(url)->image.size(), source.size());
        for (int i = 0; i < 80; ++i) media.request(url.resolved(QUrl(QString::number(i))));
        QVERIFY(media.pendingCount() <= 32);
        media.setRemoteEnabled(false);
        QCOMPARE(media.pendingCount(), 0);
    }
    void formulaFollowsPaletteAndFont()
    {
        MathRenderer math; MediaStore media;
        QTRY_VERIFY_WITH_TIMEOUT(math.request("x^2", false), 15000);
        qreal previousWidth = 0;
        for (int size : {10, 20}) {
            QPalette palette; palette.setColor(QPalette::Text, QColor("#23c658"));
            RichDocument doc(&math,&media,nullptr); doc.setDefaultFont(QFont("DejaVu Sans",size));
            QVERIFY(doc.render("$x^2$",{},palette,600));
            const auto format = doc.begin().begin().fragment().charFormat();
            const auto dimensions = doc.findChild<RichObjects *>()->intrinsicSize(&doc,0,format);
            QVERIFY(dimensions.width() > previousWidth); previousWidth = dimensions.width();
            QImage image(640,200,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image); doc.drawContents(&painter); painter.end();
            int coloredPixels = 0;
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.alpha() > 200) { QVERIFY(color.green() > color.red() * 2); ++coloredPixels; }
            }
            QVERIFY(coloredPixels > 10);
        }
    }
};
QTEST_MAIN(RenderingTest)
#include "rendering_test.moc"
