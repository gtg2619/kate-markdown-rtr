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
#include "richdocument.h"
#include "mathrenderer.h"
#include "mediastore.h"
#include <QAbstractTextDocumentLayout>
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
#include <QTextTable>
#include <QTextBlock>
#include <QWheelEvent>
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
    void richSyntax_data()
    {
        QTest::addColumn<QString>("source"); QTest::addColumn<QString>("visible");
        QTest::newRow("bold") << "A **bold** and *emphasis*." << "A bold and emphasis.";
        QTest::newRow("incomplete") << "A **bold and `code" << "A **bold and `code";
        QTest::newRow("links") << "[label](https://example.org) and <https://example.org>" << "label and https://example.org";
        QTest::newRow("entities") << "A &amp; &#128512; \\*literal\\*" << QString::fromUtf8("A & \U0001f600 *literal*");
        QTest::newRow("code") << "A `code` and `` a ` b ``." << "A code and a ` b.";
        QTest::newRow("heading") << "# Title\n\nBody\n" << "Title\nBody";
        QTest::newRow("setext") << "Title\n===\n\nBody" << "Title\nBody";
        QTest::newRow("unicode") << QString::fromUtf8("# \u4e2d\u6587 \U0001f600\n\n**\u8f93\u5165**\u00a0text") << QString::fromUtf8("\u4e2d\u6587 \U0001f600\n\u8f93\u5165 text");
        QTest::newRow("fence") << "```cpp\nint a = 1;\n```" << "int a = 1;\n";
        QTest::newRow("unclosed-fence") << "```cpp\nint a = 1;" << "```cpp\nint a = 1;";
        QTest::newRow("math") << "A $x^2$ and **bold**." << QString("A \ufffc and bold.");
        QTest::newRow("display-math") << "$$\nx^2\n$$\n\nBody" << QString("\ufffc\nBody");
        QTest::newRow("image") << "![alt](missing.png) after" << QString("\ufffc after");
        QTest::newRow("list") << "- first\n- **second**\n" << "first\nsecond";
        QTest::newRow("task") << "- [x] done\n- [ ] next" << QString("\u2611 done\n\u2610 next");
        QTest::newRow("quote") << "> quoted **text**\n> continued" << "quoted text continued";
        QTest::newRow("spaces") << "one   two\ncontinued" << "one two continued";
        QTest::newRow("rule") << "---\n\nbody" << QString("\ufffc\nbody");
        QTest::newRow("raw-html") << "a <b>raw</b>" << "a <b>raw</b>";
    }
    void richSyntax()
    {
        QFETCH(QString, source); QFETCH(QString, visible);
        MathRenderer math; MediaStore media; RichDocument rich(&math, &media, nullptr);
        QVERIFY2(rich.render(source, {}, QApplication::palette(), 600), qPrintable(source));
        QCOMPARE(rich.toPlainText(), visible);
        QVERIFY(!rich.toPlainText().contains("RTRMARK"));
        QVERIFY(rich.sourcePosition(rich.characterCount() - 1) <= source.size());
    }
    void exactSourceEdits_data()
    {
        QTest::addColumn<QString>("source"); QTest::addColumn<QString>("needle"); QTest::addColumn<QString>("expected");
        QTest::newRow("inline-code") << "A `code` end" << "code" << "A `X` end";
        QTest::newRow("nested") << "A ***both*** end" << "both" << "A ***X*** end";
        QTest::newRow("entity") << "A &amp; end" << "&" << "A X end";
        QTest::newRow("escape") << "A \\* end" << "*" << "A X end";
        QTest::newRow("reference") << "A [label][x] end\n\n[x]: /url\n" << "label" << "A [X][x] end\n\n[x]: /url\n";
        QTest::newRow("autolink") << "A <https://example.org> end" << "https://example.org" << "A <X> end";
        QTest::newRow("fenced-code") << "```cpp\nint value;\n```\n\nend" << "value" << "```cpp\nint X;\n```\n\nend";
        QTest::newRow("indented-code") << "    int value;\n\nend" << "value" << "    int X;\n\nend";
        QTest::newRow("table") << "| head | second |\n| --- | --- |\n| value | data |" << "value" << "| head | second |\n| --- | --- |\n| X | data |";
        QTest::newRow("partial-left") << "a **bold** text" << "ld te" << "a **boX**xt";
        QTest::newRow("partial-right") << "a **bold** text" << "a bo" << "X**ld** text";
        QTest::newRow("partial-nested") << "a ***both*** text" << "th te" << "a ***boX***xt";
        QTest::newRow("indented-leading") << "    first\n      second\n" << "  second" << "    first\n    X\n";
        QTest::newRow("table-escape") << "| a | b |\n| --- | --- |\n| x\\|y | c |" << "x|y" << "| a | b |\n| --- | --- |\n| X | c |";
        QTest::newRow("hardbreak") << "a\\\nb" << "b" << "a\\\nX";
        QTest::newRow("task") << "- [x] value\n- [ ] next" << "value" << "- [x] X\n- [ ] next";
        QTest::newRow("math") << "A $x$ end" << QString("\ufffc") << "A X end";
        QTest::newRow("image") << "A ![alt](missing.png) end" << QString("\ufffc") << "A X end";
    }
    void exactSourceEdits()
    {
        QFETCH(QString, source); QFETCH(QString, needle); QFETCH(QString, expected);
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText(source); std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get()); live.resize(800,600); live.show();
        QVERIFY2(live.blockCount() > 0, qPrintable(source));
        const int start = live.toPlainText().indexOf(needle); QVERIFY(start >= 0);
        auto cursor = live.textCursor(); cursor.setPosition(start); cursor.setPosition(start + needle.size(),QTextCursor::KeepAnchor); live.setTextCursor(cursor);
        QTest::keyClicks(&live,"X"); QCOMPARE(doc->text(),expected);
    }
    void repeatedTextUsesActualEditPosition()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        const QString source = "a **a** a"; doc->setText(source);
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr)); LiveEditor live(view.get());
        live.resize(800,600); live.show(); live.editAt(1); QTest::keyClicks(&live,"a");
        QCOMPARE(doc->text(),QString("aa **a** a"));
        live.editAt(5); QTest::keyClicks(&live,"a"); QCOMPARE(doc->text(),QString("aa **aa** a"));
    }
    void paragraphsAndCodeNewlines()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText("# **Title**"); std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr)); LiveEditor live(view.get());
        live.resize(800,600); live.show(); live.editAt(9);
        QTest::keyClick(&live, Qt::Key_Return); QCOMPARE(doc->text(),QString("# **Title**\n\n"));
        QCOMPARE(live.toPlainText(),QString("Title\n"));
        QTest::keyClicks(&live,"Next"); QCOMPARE(doc->text(),QString("# **Title**\n\nNext"));
        doc->setText("```cpp\nint a;\n```\n"); live.editAt(13);
        QTest::keyClick(&live,Qt::Key_Return); QCOMPARE(doc->text(),QString("```cpp\nint a;\n\n```\n"));
        QVERIFY(!live.toPlainText().contains("```"));
        doc->setText("A **bold** end"); live.editAt(6); QTest::keyClick(&live,Qt::Key_Return);
        QCOMPARE(doc->text(),QString("A **bo**\n\n**ld** end"));
        QCOMPARE(live.toPlainText(),QString("A bo\nld end"));
    }
    void largeDocumentInteriorEditsStayIncremental()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        QString expected = QString("## Header\n\nParagraph **bold** text.\n\n").repeated(500); doc->setText(expected);
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr)); LiveEditor live(view.get()); live.resize(800,600); live.show();
        const int parses = live.parseCount(); QElapsedTimer timer; timer.start();
        for (int i=0;i<300;++i) {
            const int at = expected.indexOf("Paragraph") + 3; live.editAt(at);
            QTest::keyClicks(&live,"x"); expected.insert(at,'x');
            QCOMPARE(doc->text(),expected);
            // Keep the same word for the next iteration.
            QTest::keyClick(&live,Qt::Key_Backspace); expected.remove(at,1); QCOMPARE(doc->text(),expected);
        }
        qInfo() << "600 edits in 1000 blocks:" << timer.elapsed() << "ms";
        QCOMPARE(live.parseCount(),parses); QVERIFY(timer.elapsed()<5000);
    }
    void incompleteAndComplexSyntax_data()
    {
        QTest::addColumn<QString>("source");
        const QStringList inputs = {"[incomplete](url", "![incomplete](url", "**one *two***", "&NotEqualTilde; &#0; &copy;", "  text  with spaces", "> ```python\n>     value = 1\n> ```", "    first\n      second\n", "a  \nb", "a\\\nb", "## Heading ##", "[label][ref]\n\n[ref]: /url \"Title\"", "| a | b |\n| --- | --- |\n| x\\|y | **b** |", "- outer\n  - inner\n\n    more", "one $x$two$y$ three", "```python\nvalue = 1\n``` incomplete", "\tcode\n", "# ", "[empty]()", "<https://example.org/?a=1&b=2>", "[a\\*b](/x)", "[![alt](img.png)](/x)"};
        for (int i=0;i<inputs.size();++i) QTest::newRow(qPrintable(QString::number(i))) << inputs[i];
    }
    void incompleteAndComplexSyntax()
    {
        QFETCH(QString, source); MathRenderer math; MediaStore media; RichDocument rich(&math,&media,nullptr);
        QVERIFY2(rich.render(source,{},QApplication::palette(),700),qPrintable(source));
        QVERIFY(!rich.toPlainText().contains("RTRMARK"));
    }
    void demoRendersWithoutSourceExpansion()
    {
        QFile file(QStringLiteral(TEST_SOURCE_DIR "/docs/demo.md")); QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());
        MathRenderer math; MediaStore media; RichDocument rich(&math,&media,nullptr);
        QVERIFY(rich.render(source,QUrl::fromLocalFile(QStringLiteral(TEST_SOURCE_DIR "/docs/")),QApplication::palette(),900));
        QVERIFY(!rich.toPlainText().contains("RTRMARK")); QVERIFY(!rich.toPlainText().contains("$$")); QVERIFY(!rich.toPlainText().contains("```"));
    }
    void ordinaryEditingAndIncompleteDelimiters()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText(""); std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get()); live.resize(800,600); live.show();
        QTest::keyClicks(&live, "**bold"); QCOMPARE(live.toPlainText(), QString("**bold"));
        QTest::keyClicks(&live, "**"); QCOMPARE(live.toPlainText(), QString("bold"));
        QTest::keyClicks(&live, " after"); QCOMPARE(doc->text(), QString("**bold** after"));
        live.editAt(4); QTest::keyClicks(&live, "X"); QCOMPARE(doc->text(), QString("**boXld** after"));
        QCOMPARE(live.toPlainText(), QString("boXld after"));
        QTest::keyClick(&live, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!doc->text().contains('X'));
        QTest::keyClick(&live, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(doc->text().contains("boXld"));
    }
    void browsingNeverChangesLayout()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        const QString source = QString("# Title\n\nParagraph **bold** and [link](/url).\n\n- one\n- two\n\n").repeated(150);
        doc->setText(source); std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get()); live.resize(800,600); live.show(); QTest::qWait(30);
        const auto size = live.document()->documentLayout()->documentSize();
        const int maximum = live.verticalScrollBar()->maximum(), parses = live.parseCount();
        for (int i = 0; i < 40; ++i) {
            live.verticalScrollBar()->setValue((i * 701) % maximum);
            QTest::mouseClick(live.viewport(), Qt::LeftButton, Qt::NoModifier, {100,100});
            QTest::mouseClick(live.viewport(), Qt::LeftButton, Qt::ShiftModifier, {150,200});
            live.grab(); QCOMPARE(live.document()->documentLayout()->documentSize(), size);
            QCOMPARE(live.verticalScrollBar()->maximum(), maximum);
        }
        live.selectAll(); live.copy(); QCOMPARE(doc->text(), source); QCOMPARE(live.parseCount(), parses);
        QVERIFY(!live.toPlainText().contains("**"));
    }
    void selectionAndBoundaries()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        const QString source = "one **bold**\n\ntwo [label](/url)\n\n[ref]: /untouched\n";
        doc->setText(source); std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get()); live.resize(800,600); live.show();
        auto cursor = live.textCursor(); cursor.setPosition(4); cursor.setPosition(8,QTextCursor::KeepAnchor); live.setTextCursor(cursor);
        QTest::keyClicks(&live,"new"); QCOMPARE(doc->text(), QString("one **new**\n\ntwo [label](/url)\n\n[ref]: /untouched\n"));
        live.editAt(doc->text().indexOf("two")); QTest::keyClick(&live,Qt::Key_Backspace);
        QCOMPARE(doc->text(),QString("one **new**two [label](/url)\n\n[ref]: /untouched\n"));
        live.selectAll(); QTest::keyClicks(&live,"replacement"); QCOMPARE(doc->text(),QString("replacement"));
    }
    void unicodeImeAndExternalChanges()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        const QString source = QString::fromUtf8("\u4e2d\u6587\u00a0\U0001f600\n\n**bold**\n");
        doc->setText(source); std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get()); live.resize(800,600); live.show(); live.editAt(2);
        QInputMethodEvent preedit(QString::fromUtf8("\u8f93\u5165"),{}); QApplication::sendEvent(&live,&preedit); QCOMPARE(doc->text(),source);
        QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("\u8f93\u5165\U0001f601")); QApplication::sendEvent(&live,&commit);
        QCOMPARE(doc->text(),QString(source).insert(2,QString::fromUtf8("\u8f93\u5165\U0001f601")));
        doc->setText("outside **bold**"); live.editAt(0); QTest::keyClicks(&live,"X"); QCOMPARE(doc->text(),QString("Xoutside **bold**"));
        doc->setReadWrite(false); QTest::keyClicks(&live,"bad"); QCOMPARE(doc->text(),QString("Xoutside **bold**"));
        doc->setReadWrite(true);
        connect(doc.get(),&KTextEditor::Document::textChanged,doc.get(),[&] { if (doc->text().startsWith('x')) doc->insertText({0,0},"formatted "); });
        live.editAt(0); QTest::keyClicks(&live,"x"); QCOMPARE(doc->text(),QString("formatted xXoutside **bold**"));
        live.editAt(0); QTest::keyClicks(&live,"!"); QVERIFY(doc->text().startsWith("!formatted"));
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
        live.paste();
        QVERIFY(doc->text().contains("![image](notes%20space.md.assets/"));
        const auto match = QRegularExpression(QStringLiteral("!\\[image\\]\\(([^)]+)\\)")).match(doc->text());
        QVERIFY(match.hasMatch());
        const auto file = directory.filePath(QUrl::fromPercentEncoding(match.captured(1).toUtf8()));
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(QImage(file).size(), image.size());
        QTest::keyClick(&live, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(!doc->text().contains("![image]"));
        QVERIFY(QFileInfo::exists(file)); // Undo must never delete a possibly shared asset.
        QApplication::clipboard()->clear();
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
        }
        delete view;
    }
    void emptyCellAndEscapeAndTheme()
    {
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText("| a | b |\n| --- | --- |\n|  | value |\n");
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr)); view->resize(800,600); view->show(); view->activateWindow();
        const QFont font(QStringLiteral("DejaVu Sans Mono"),12); view->setConfigValue(QStringLiteral("font"),font);
        LiveEditor live(view.get(),view.get()); live.resize(800,600); live.show(); live.setFocus();
        QCOMPARE(live.font().family(),font.family());
        QCOMPARE(live.palette().color(QPalette::Base),QColor(view->theme().editorColor(KSyntaxHighlighting::Theme::BackgroundColor)));
        QTextTable *table=nullptr;
        for (auto b=live.document()->begin();b.isValid()&&!table;b=b.next()) { QTextCursor c(b); table=c.currentTable(); }
        QVERIFY(table); live.setTextCursor(table->cellAt(1,0).firstCursorPosition()); QTest::keyClicks(&live,"X");
        QCOMPARE(doc->text(),QString("| a | b |\n| --- | --- |\n|  X| value |\n"));
        doc->setText("| a | b | c |\n| --- | --- | --- |\n| value |\n"); live.refresh(); table=nullptr;
        for (auto b=live.document()->begin();b.isValid()&&!table;b=b.next()) { QTextCursor c(b); table=c.currentTable(); }
        QVERIFY(table); live.setTextCursor(table->cellAt(1,2).firstCursorPosition()); QTest::keyClicks(&live,"X");
        QCOMPARE(doc->text(),QString("| a | b | c |\n| --- | --- | --- |\n| value | |  X\n"));
        QAction closePanel(view.get()); closePanel.setShortcut(QKeySequence(Qt::Key_Escape)); view->addAction(&closePanel);
        QSignalSpy closed(&closePanel,&QAction::triggered); QTest::qWait(20);
        QTest::keyClick(&live,Qt::Key_Escape); QCOMPARE(closed.count(),1); QVERIFY(live.isVisible());
        const auto source=doc->text(); QTest::keyClick(&live,Qt::Key_Return,Qt::ControlModifier); QCOMPARE(doc->text(),source);
        doc->setText("```python\ndef energy(mass):\n    return mass * 2\n```\n"); live.refresh();
        live.document()->documentLayout()->documentSize();
        QVERIFY(live.toPlainText().contains("def energy(mass):"));
        for (auto b=live.document()->begin();b.isValid();b=b.next()) if (!b.text().isEmpty()) QVERIFY(b.layout()->lineCount()>0);
    }
    void pointerAndVisibleCaretNeverScroll()
    {
        QTemporaryDir directory; const auto path=directory.filePath("tall.png");
        QImage image(300,640,QImage::Format_RGB32); image.fill(Qt::green); QVERIFY(image.save(path));
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        const QString source=QString("Above paragraph\n\n").repeated(10)+"![tall]("+path+")\n\n"+QString("Below **paragraph** text\n\n").repeated(60);
        doc->setText(source); std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr));
        LiveEditor live(view.get()); live.resize(800,400); live.show(); QTest::qWait(30);
        const auto geometry=live.document()->documentLayout()->documentSize(); const int parses=live.parseCount();
        for (int top : {0,150,300,500,750,1000}) {
            live.verticalScrollBar()->setValue(top); const int scroll=live.verticalScrollBar()->value();
            for (const auto point : {QPoint(100,40),QPoint(100,200),QPoint(400,350)}) {
                for (int i=0;i<4;++i) {
                    QTest::mouseClick(live.viewport(),Qt::LeftButton,Qt::NoModifier,point);
                    QCoreApplication::processEvents(); QCOMPARE(live.verticalScrollBar()->value(),scroll);
                    QTest::mouseDClick(live.viewport(),Qt::LeftButton,Qt::NoModifier,point);
                    QTest::mouseClick(live.viewport(),Qt::LeftButton,Qt::ShiftModifier,point+QPoint(40,0));
                    QCoreApplication::processEvents(); QCOMPARE(live.verticalScrollBar()->value(),scroll);
                    for (auto key : {Qt::Key_Left,Qt::Key_Right,Qt::Key_Up,Qt::Key_Down,Qt::Key_Home,Qt::Key_End}) {
                        QTest::keyClick(&live,key); QTest::keyClick(&live,key,Qt::ShiftModifier);
                        QCOMPARE(live.verticalScrollBar()->value(),scroll);
                    }
                    live.setTextCursor(live.cursorForPosition(point)); QCOMPARE(live.verticalScrollBar()->value(),scroll);
                }
            }
            live.selectAll(); QCOMPARE(live.verticalScrollBar()->value(),scroll);
            QTest::keyClick(&live,Qt::Key_Home,Qt::ControlModifier); QCOMPARE(live.verticalScrollBar()->value(),scroll);
            QTest::keyClick(&live,Qt::Key_End,Qt::ControlModifier); QCOMPARE(live.verticalScrollBar()->value(),scroll);
            QTest::qWait(30); QCOMPARE(live.verticalScrollBar()->value(),scroll);
        }
        live.verticalScrollBar()->setValue(0);
        QWheelEvent wheel(QPointF(100,100),live.viewport()->mapToGlobal(QPoint(100,100)),QPoint(),QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(live.viewport(),&wheel); QVERIFY(live.verticalScrollBar()->value()>0);
        QCOMPARE(live.document()->documentLayout()->documentSize(),geometry); QCOMPARE(live.parseCount(),parses); QCOMPARE(doc->text(),source);
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
        QTRY_VERIFY(action->isChecked()); // Existing Markdown tabs open in RTR automatically.
        QPointer<QTextEdit> surface(view->findChild<QTextEdit *>(QStringLiteral("markdownLiveEditor")));
        QVERIFY(surface && surface->isVisible());
        view->setFocus(); // Kate restores focus after automatic tab activation.
        QTRY_VERIFY(surface->hasFocus() || surface->isAncestorOf(QApplication::focusWidget()));
        QCOMPARE(surface->geometry(), view->rect());
        view->resize(1000, 700);
        QCOMPARE(surface->geometry(), view->rect());
        auto input = surface.data();
        QVERIFY(input && input->isVisible());
        QTest::keyClicks(input, "edit ");
        QVERIFY(doc->isModified());
        QVERIFY(doc->save());
        QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(saved.readAll()), doc->text());
        action->trigger();
        QVERIFY(surface->isHidden());
        Q_EMIT window.viewChanged(view.get());
        QCoreApplication::processEvents();
        QVERIFY(!action->isChecked() && surface->isHidden());
        doc->insertText({0, 0}, "outside ");
        action->trigger();
        QVERIFY(surface->isVisible());
        delete pluginView;
        QVERIFY(surface.isNull());
        QVERIFY2(doc->text().contains("outside # edit"), qPrintable(doc->text()));
    }
    void autoStartsAfterMarkdownSaveAndNewView()
    {
        QPluginLoader loader(QStringLiteral(PLUGIN_FILE));
        auto factory = qobject_cast<KPluginFactory *>(loader.instance()); QVERIFY(factory);
        std::unique_ptr<KTextEditor::Plugin> plugin(factory->create<KTextEditor::Plugin>(nullptr));
        Host host; KTextEditor::MainWindow window(&host);
        std::unique_ptr<KTextEditor::Document> doc(KTextEditor::Editor::instance()->createDocument(nullptr));
        doc->setText("# New document\n");
        std::unique_ptr<KTextEditor::View> view(doc->createView(nullptr, &window));
        host.view = view.get(); view->resize(800, 600); view->show();
        auto pluginView = plugin->createView(&window);
        auto action = dynamic_cast<KXMLGUIClient *>(pluginView)->actionCollection()->action(QStringLiteral("markdown_live_toggle"));
        QCoreApplication::processEvents(); QVERIFY(!action->isChecked());
        QTemporaryDir directory;
        QVERIFY(doc->saveAs(QUrl::fromLocalFile(directory.filePath("new.md"))));
        QTRY_VERIFY(action->isChecked());
        action->trigger(); QVERIFY(!action->isChecked());
        QVERIFY(doc->saveAs(QUrl::fromLocalFile(directory.filePath("renamed.md"))));
        QCoreApplication::processEvents(); QVERIFY(!action->isChecked());
        std::unique_ptr<KTextEditor::View> second(doc->createView(nullptr, &window));
        host.view = second.get(); second->resize(800, 600); second->show();
        Q_EMIT window.viewCreated(second.get()); Q_EMIT window.viewChanged(second.get());
        QTRY_VERIFY(action->isChecked());
        host.view = view.get(); Q_EMIT window.viewChanged(view.get());
        QCoreApplication::processEvents(); QVERIFY(!action->isChecked());
        delete pluginView;
    }
};
QTEST_MAIN(EditorTest)
#include "editor_test.moc"
