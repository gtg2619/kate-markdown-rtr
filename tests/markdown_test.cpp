#include "markdown.h"
#include <QTest>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTextDocument>

using namespace MarkdownLive;
class MarkdownTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void mathDelimiters_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<int>("count");
        QTest::newRow("inline") << QString::fromUtf8("\u4e2d\u6587 $x^2$ and \\(\\alpha+\\beta\\).") << 2;
        QTest::newRow("display") << QString("before\n$$\\frac{a}{b}$$\nafter\n\\[\nx^2+y^2\n\\]\n") << 2;
        QTest::newRow("aligned") << QString("$$\n\\begin{aligned}\na&=b\\\\\nc&=d\n\\end{aligned}\n$$\n") << 1;
        QTest::newRow("code") << QString("`$x$` ``\\(a\\)``\n\n```tex\n$$a$$\n```\n\n    $b$\n") << 0;
        QTest::newRow("escaped") << QString("\\$not math\\$ and \\\\(literal\\)\n") << 0;
        QTest::newRow("currency") << QString("costs $5 and $10, but $2+3$ is math") << 1;
        QTest::newRow("link") << QString("[text](https://example.org/$path$) ![alt $x$](a.png)") << 0;
        QTest::newRow("unclosed") << QString("$x\n\\[ unfinished\n") << 0;
        QTest::newRow("table") << QString("| Math |\n| --- |\n| $x^2$ |\n") << 1;
        QTest::newRow("quote") << QString("> $$\n> x^2\n> $$\n") << 1;
        QTest::newRow("list") << QString("- $$x^2$$\n- normal\n") << 1;
        QTest::newRow("pipes") << QString("| Math |\n| --- |\n| $|x|$ |\n") << 1;
        QTest::newRow("marker") << QString("KATEMATH0Z0END $x$ KATEMATH1Z0END") << 1;
        QTest::newRow("code-boundary") << QString("$start `code $` end") << 0;
    }
    void mathDelimiters()
    {
        QFETCH(QString, source); QFETCH(int, count);
        const auto result = parse(source);
        QVERIFY(!result.limited);
        int offset = 0, found = 0;
        for (const auto &block : result.blocks) {
            QCOMPARE(block.start, offset); offset = block.end;
            found += int(block.math.size());
            for (const auto &math : block.math) {
                QVERIFY(block.html.contains(QStringLiteral("katemath:/") + math.marker));
                QVERIFY(math.start >= block.start && math.end <= block.end);
            }
        }
        QCOMPARE(found, count);
        QCOMPARE(offset, source.size());
    }
    void standardsAndCoverage()
    {
        const QString source = QString::fromUtf8("# \u6807\u9898 \U0001f600\n\n**bold** and *em* ~~gone~~ [ref][id]\n\n"
            "- [x] done\n- [ ] todo\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n"
            "```cpp\n<literal>\n```\n\n[id]: https://example.org\n");
        const auto result = parse(source);
        QVERIFY(!result.limited);
        int offset = 0;
        QString html;
        for (const auto &block : result.blocks) {
            QCOMPARE(block.start, offset);
            QVERIFY(block.end >= block.start);
            offset = block.end;
            html += block.html;
        }
        QCOMPARE(offset, source.size());
        QVERIFY(html.contains("<h1>"));
        QVERIFY(html.contains("<strong>bold</strong>"));
        QVERIFY(html.contains("<del>gone</del>"));
        QVERIFY(html.contains("<table>"));
        QVERIFY(html.contains("&#9745;"));
        QVERIFY(html.contains("href=\"https://example.org\""));
        QVERIFY(html.contains("&lt;literal&gt;"));
        QVERIFY(std::any_of(result.blocks.cbegin(), result.blocks.cend(), [](const auto &block) { return block.codeLanguage == "cpp"; }));
        QVERIFY(html.contains("[id]:")); // Invisible definitions stay reachable for editing.
    }
    void inertHtml()
    {
        const auto result = parse("<script>alert(1)</script>\n\n[x](javascript:alert)\n\n![x](https://invalid.test/x.png)\n");
        QString html;
        for (const auto &block : result.blocks) html += block.html;
        QVERIFY(!html.contains("<script>"));
        QVERIFY(!html.contains("href=\"javascript:"));
    }
    void partitionRandomSource()
    {
        QRandomGenerator random(42);
        const QString alphabet = QString::fromUtf8("ab\u4e2d\u6587\U0001f600\n\t #*_`[]()<>!-|&$\\");
        for (int n = 0; n < 300; ++n) {
            QString text;
            for (int i = 0; i < n * 3; ++i) text += alphabet[random.bounded(alphabet.size())];
            const auto result = parse(text);
            QVERIFY(!result.limited);
            int position = 0;
            for (const auto &block : result.blocks) {
                QCOMPARE(block.start, position);
                QVERIFY(block.end >= position);
                position = block.end;
            }
            QCOMPARE(position, text.size());
        }
    }
    void splices()
    {
        const QStringList texts = {"", "a", "a\nb", QString::fromUtf8("\u4f60\u597d\U0001f600\u7532"), QString::fromUtf8("\u4f60\u597d\U0001f601\u7532"), "**x**"};
        for (const auto &before : texts) for (const auto &after : texts) {
            const auto edit = difference(before, after);
            QString applied = before;
            applied.replace(edit.start, edit.removed, edit.inserted);
            QCOMPARE(applied, after);
            QVERIFY(edit.start >= before.size() || !before[edit.start].isLowSurrogate());
        }
    }
    void boundsAndPerformance()
    {
        QVERIFY(parse(QString(MaxSourceSize + 1, u'x')).limited);
        QString source;
        for (int i = 0; i < 1000; ++i) source += QStringLiteral("## Heading %1\n\nParagraph with **bold**, `code`, and [link](https://example.org).\n\n").arg(i);
        QElapsedTimer timer;
        timer.start();
        const auto result = parse(source);
        const auto milliseconds = timer.elapsed();
        QVERIFY(!result.limited);
        qInfo() << "Parse" << source.size() << "UTF-16 units /" << result.blocks.size() << "blocks:" << milliseconds << "ms";
        QVERIFY2(milliseconds < 2000, "Parsing unexpectedly slow; generous CI ceiling, not a benchmark claim");
        QVERIFY(parse(QString("x\n\n").repeated(5000)).limited);
    }
    void hostileDelimitersStayBounded()
    {
        for (const auto &text : {QString(256 * 1024, u'\\'), QString("\\[ ").repeated(12000),
            QString("KATEMATH0Z") + QString(128 * 1024, u'Z'), QString("$x$ ").repeated(5000)}) {
            QElapsedTimer timer; timer.start();
            const auto result = parse(text);
            QVERIFY(!result.limited);
            QCOMPARE(result.blocks.first().start, 0);
            QCOMPARE(result.blocks.last().end, text.size());
            QVERIFY2(timer.elapsed() < 3000, "Pathological delimiter scan exceeded the generous regression ceiling");
        }
    }
};
QTEST_MAIN(MarkdownTest)
#include "markdown_test.moc"
