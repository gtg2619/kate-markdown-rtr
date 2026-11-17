#include "liveeditor.h"
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Plugin>
#include <KPluginFactory>
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <KSyntaxHighlighting/Theme>
#include <KActionCollection>
#include <QAction>
#include <QPlainTextEdit>
#include <QTest>
#include <QScrollBar>
#include <QPluginLoader>
#include <QInputMethodEvent>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QSignalSpy>
#include <QSplitter>
#include <QClipboard>
#include <QRegularExpression>
#include <memory>

using namespace MarkdownLive;

class Host : public QObject
{
    Q_OBJECT
public:
    KTextEditor::View *view = nullptr;
public Q_SLOTS:
    KTextEditor::View *activeView() { return view; }
    QList<KTextEditor::View *> views() { return view ? QList<KTextEditor::View *>{view} : QList<KTextEditor::View *>{}; }
    KXMLGUIFactory *guiFactory() { return nullptr; }
    QWidget *window() { return view; }
    QWidget *createViewBar(KTextEditor::View *) { return nullptr; }
    void deleteViewBar(KTextEditor::View *) {}
};

class EditorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void editingFormulaPreservesSurroundingSource()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        const QString source = "Inline $x^2$ and **bold**.\n\n$$\n\\frac{a}{b}\n$$\n\n![image](missing.png)\n";
        doc->setText(source);
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get()); live.resize(800, 600); live.show();
        QTest::qWait(200); // Allow a resource completion while editing the original source.
        const int offset = source.indexOf("x^2") + 3;
        live.editAt(offset);
        QTest::keyClicks(live.input(), "+y");
        QTest::qWait(300);
        QVERIFY(live.input()->isVisible());
        QCOMPARE(doc->text(), QString(source).insert(offset, "+y"));
        live.finishEditing();
        QCOMPARE(doc->text(), QString(source).insert(offset, "+y"));
    }
    void pastedImageIsEmbeddedAndUndoPreservesFile()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("notes space.md");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("# Images\n\n"); }
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        QVERIFY(doc->openUrl(QUrl::fromLocalFile(path)));
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get()); live.resize(800, 600); live.show(); live.editAt(10);
        QImage image(120, 80, QImage::Format_RGB32); image.fill(Qt::green);
        QApplication::clipboard()->setImage(image);
        live.input()->paste();
        QVERIFY(doc->text().contains("![image](notes%20space.md.assets/"));
        const auto match = QRegularExpression(QStringLiteral("!\\[image\\]\\(([^)]+)\\)")).match(doc->text());
        QVERIFY(match.hasMatch());
        const auto file = directory.filePath(QUrl::fromPercentEncoding(match.captured(1).toUtf8()));
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(QImage(file).size(), image.size());
        QTest::keyClick(live.input(), Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!doc->text().contains("![image]"));
        QVERIFY(QFileInfo::exists(file)); // Undo must never delete a possibly shared asset.
        QApplication::clipboard()->clear();
    }
    void editPreservesSourceAndUndo()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        const QString source = QString::fromUtf8("# \u6807\u9898 \U0001f600\n\nA **bold** paragraph.\n\n[ref]: /untouched\n");
        doc->setText(source);
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get());
        live.resize(800, 600); live.show();
        const int offset = source.indexOf("paragraph");
        live.editAt(offset);
        const int parses = live.parseCount();
        QTest::keyClicks(live.input(), "new ");
        QCOMPARE(doc->text(), QString(source).insert(offset, "new "));
        QCOMPARE(live.parseCount(), parses); // No full parse on individual keystrokes.
        QTest::keyClick(live.input(), Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(doc->text().size() < source.size() + 4);
        live.finishEditing();
        QVERIFY(doc->text().endsWith("[ref]: /untouched\n"));
    }
    void externalEditsAndReadOnly()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText("first\n\nsecond\n");
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get());
        live.resize(800, 600); live.show(); live.editAt(2);
        doc->insertText({0, 0}, "outside ");
        QCOMPARE(live.activeStart(), -1);
        QVERIFY(!live.input()->isEnabled());
        QTRY_VERIFY(live.input()->isEnabled());
        live.editAt(0);
        doc->setReadWrite(false);
        QTest::keyClicks(live.input(), "bad");
        QCOMPARE(doc->text(), QString("outside first\n\nsecond\n"));
        doc->setReadWrite(true);
        QTest::keyClicks(live.input(), "ok ");
        QVERIFY(doc->text().startsWith("ok outside"));
    }
    void selectionAndBoundaryDeletion()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText("one\n\ntwo\n");
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get());
        live.resize(800, 600); live.show(); live.editAt(5);
        QTest::keyClick(live.input(), Qt::Key_Backspace);
        QCOMPARE(doc->text(), QString("one\ntwo\n"));
        QTest::keyClick(live.input(), Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(live.input()->textCursor().selectedText().size(), doc->text().size());
        QTest::keyClicks(live.input(), "replacement");
        QCOMPARE(doc->text(), QString("replacement"));
    }
    void viewportCache()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText(QString("## Header\n\nParagraph **bold** text.\n\n").repeated(500));
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get());
        live.resize(800, 600); live.show();
        for (int i = 0; i < 30; ++i) {
            live.verticalScrollBar()->setValue(i * 700);
            live.grab();
            QVERIFY(live.cachedDocuments() <= 48);
        }
        QVERIFY(live.blockCount() >= 1000);
    }
    void unicodeImeAndNewBlock()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        const QString source = QString::fromUtf8("\u4e2d\u6587\u00a0\U0001f600\u2028\u539f\u6587\u2029\u4fdd\u7559\n\nnext\n");
        doc->setText(source);
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get());
        live.resize(800, 600); live.show(); live.editAt(2);
        QInputMethodEvent preedit(QString::fromUtf8("\u8f93\u5165"), {});
        QApplication::sendEvent(live.input(), &preedit);
        QCOMPARE(doc->text(), source);
        QInputMethodEvent commit;
        commit.setCommitString(QString::fromUtf8("\u8f93\u5165\U0001f601"));
        QApplication::sendEvent(live.input(), &commit);
        QCOMPARE(doc->text(), QString(source).insert(2, QString::fromUtf8("\u8f93\u5165\U0001f601")));
        live.finishEditing();
        doc->setText("# title");
        live.refresh(); live.editAt(7);
        QTest::keyClick(live.input(), Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(doc->text(), QString("# title\n"));
        QTest::keyClicks(live.input(), "next");
        QCOMPARE(doc->text(), QString("# title\nnext"));
    }
    void randomEditsAndLatency()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        QString expected = "# Heading\n\nkeep **formatting** and [references][x]\n\n[x]: /local\n";
        doc->setText(expected);
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get());
        live.resize(800, 600); live.show(); live.editAt(11);
        const int parses = live.parseCount();
        QElapsedTimer timer; timer.start();
        for (int i = 0; i < 300; ++i) {
            auto cursor = live.input()->textCursor();
            const int position = (i * 17) % (live.activeEnd() - live.activeStart());
            cursor.setPosition(position);
            live.input()->setTextCursor(cursor);
            const QString insertion = i % 2 ? QString::fromUtf8("\u4e2d") : QString("x");
            expected.insert(live.activeStart() + position, insertion);
            cursor.insertText(insertion);
            QCOMPARE(doc->text(), expected);
        }
        qInfo() << "300 source-preserving edits:" << timer.elapsed() << "ms";
        QCOMPARE(live.parseCount(), parses);
        QVERIFY(timer.elapsed() < 3000);
    }
    void fontPaletteAndScreenshot()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText(QString::fromUtf8("# Markdown \u5373\u65f6\u7f16\u8f91\n\n"
            "\u8fd9\u662f **\u539f\u751f Qt \u6e32\u67d3**\uff0c\u4f7f\u7528 Kate \u7684\u5b57\u4f53\u4e0e\u914d\u8272\u3002\u70b9\u51fb\u4efb\u610f\u6bb5\u843d\u5373\u53ef\u539f\u4f4d\u7f16\u8f91\u3002\n\n"
            "## \u652f\u6301 CommonMark \u4e0e GFM\n\n"
            "- [x] \u4fdd\u7559\u539f\u59cb Markdown \u683c\u5f0f\n- [x] \u590d\u7528 Kate \u6587\u6863\u4e0e\u64a4\u9500\u5386\u53f2\n- [ ] \u5b8c\u6210\u4e0b\u4e00\u6bb5\u6587\u5b57\n\n"
            "| \u529f\u80fd | \u5b9e\u73b0 |\n| --- | --- |\n| \u6e32\u67d3 | cmark-gfm + QTextDocument |\n| \u7f16\u8f91 | \u5355\u4e2a\u539f\u751f\u6587\u672c\u63a7\u4ef6 |\n\n"
            "> \u5207\u6362\u6bb5\u843d\u65f6\u6062\u590d\u9884\u89c8\uff1bEsc \u5b8c\u6210\u5f53\u524d\u5757\u3002\n\n"
            "```cpp\nauto message = QStringLiteral(\"Hello, Kate!\");\n```\n"));
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        const QFont font(QStringLiteral("DejaVu Sans Mono"), 12);
        view->setConfigValue(QStringLiteral("font"), font);
        LiveEditor live(view.get());
        live.resize(1000, 800); live.show();
        QCOMPARE(live.input()->font().family(), font.family());
        QCOMPARE(live.palette().color(QPalette::Base), QColor(view->theme().editorColor(KSyntaxHighlighting::Theme::BackgroundColor)));
        QFont changed = font; changed.setPointSize(14);
        view->setConfigValue(QStringLiteral("font"), changed);
        QTRY_COMPARE(live.input()->font().pointSize(), 14);
        view->setConfigValue(QStringLiteral("font"), font);
        live.grab().save(QStringLiteral("live-preview.png"));
        live.editAt(doc->text().indexOf(QString::fromUtf8("\u8fd9\u662f")) + 2);
        live.grab().save(QStringLiteral("live-editing.png"));
    }
    void loadedPluginLifecycleAndSave()
    {
        QPluginLoader loader(QStringLiteral(PLUGIN_FILE));
        auto factory = qobject_cast<KPluginFactory *>(loader.instance());
        QVERIFY2(factory, qPrintable(loader.errorString()));
        std::unique_ptr<KTextEditor::Plugin> plugin(factory->create<KTextEditor::Plugin>(nullptr));
        QVERIFY(plugin);
        Host host;
        KTextEditor::MainWindow window(&host);
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        QTemporaryDir directory;
        const QString path = directory.filePath("example.md");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("# Original\n\nBody\n"); }
        QVERIFY(doc->openUrl(QUrl::fromLocalFile(path)));
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr, &window));
        host.view = view.get();
        view->resize(900, 650); view->show();
        auto pluginView = plugin->createView(&window);
        auto client = dynamic_cast<KXMLGUIClient *>(pluginView);
        QVERIFY(client);
        auto action = client->actionCollection()->action(QStringLiteral("markdown_live_toggle"));
        QVERIFY(action && action->isEnabled());
        action->trigger();
        QPointer<QAbstractScrollArea> surface(view->findChild<QAbstractScrollArea *>(QStringLiteral("markdownLiveEditor")));
        QVERIFY(surface && surface->isVisible());
        QCOMPARE(surface->geometry(), view->rect());
        view->resize(1000, 700);
        QCOMPARE(surface->geometry(), view->rect());
        QTest::keyClick(surface, Qt::Key_Return);
        auto input = surface->findChild<QPlainTextEdit *>();
        QVERIFY(input && input->isVisible());
        QTest::keyClicks(input, "edit ");
        QVERIFY(doc->isModified());
        QVERIFY(doc->save());
        QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(saved.readAll()), doc->text());
        action->trigger();
        QVERIFY(surface->isHidden());
        doc->insertText({0, 0}, "outside ");
        action->trigger();
        QVERIFY(surface->isVisible());
        delete pluginView;
        QVERIFY(surface.isNull());
        QVERIFY(doc->text().contains("outside edit"));
    }
    void editorSplitCanShrinkWithoutNativeSurface()
    {
        QSplitter split(Qt::Vertical);
        split.setChildrenCollapsible(false);
        new QWidget(&split);
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText("# Heading\n\nBody\n");
        auto view = doc->createView(&split);
        auto live = new LiveEditor(view, view);
        live->setGeometry(view->rect());
        live->show();
        split.resize(900, 900); split.show();
        for (int n = 0; n < 3; ++n) {
            split.setSizes({200, 680}); QCoreApplication::processEvents();
            const int grown = view->height();
            split.setSizes({700, 180}); QCoreApplication::processEvents();
            QVERIFY(view->height() < grown);
            QVERIFY(view->height() <= 200);
            QVERIFY(!live->testAttribute(Qt::WA_NativeWindow));
            QVERIFY(!live->input()->testAttribute(Qt::WA_NativeWindow));
        }
        delete view;
    }
    void synchronousFormatterDoesNotLeaveStaleOffsets()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText("paragraph\n");
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get());
        live.resize(800, 600); live.show(); live.editAt(0);
        connect(doc.get(), &KTextEditor::Document::textChanged, doc.get(), [&] {
            if (doc->text().startsWith("x")) doc->insertText({0, 0}, "formatted ");
        });
        QTest::keyClicks(live.input(), "x");
        QCOMPARE(doc->text(), QString("formatted xparagraph\n"));
        QCOMPARE(live.activeStart(), -1);
        live.editAt(0);
        QTest::keyClicks(live.input(), "!");
        QCOMPARE(doc->text(), QString("!formatted xparagraph\n"));
    }
};
QTEST_MAIN(EditorTest)
#include "editor_test.moc"
