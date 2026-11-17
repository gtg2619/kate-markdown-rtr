#include "richparser.h"
#include "mathsyntax.h"
#include <cmark-gfm.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm-core-extensions.h>
#include <houdini.h>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cstdlib>
#include <memory>

namespace MarkdownLive {
namespace {
struct Positions {
    const QString &text;
    QVector<int> lines{0};
    QHash<int, QVector<int>> columns;
    explicit Positions(const QString &s) : text(s) {
        for (int i = 0; i < s.size(); ++i) if (s[i] == u'\n') lines.append(i + 1);
        lines.append(int(s.size()));
    }
    int at(int line, int column) {
        line = qBound(0, line - 1, int(lines.size()) - 2);
        if (!columns.contains(line)) {
            QVector<int> result;
            for (int i = lines[line]; i < lines[line + 1];) {
                const int n = text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate() ? 2 : 1;
                for (int b = 0; b < text.mid(i, n).toUtf8().size(); ++b) result.append(i);
                i += n;
            }
            result.append(lines[line + 1]); columns.insert(line, result);
        }
        const auto &map = columns[line];
        return map[qBound(0, column - 1, int(map.size()) - 1)];
    }
};
using Node = std::unique_ptr<cmark_node, decltype(&cmark_node_free)>;
}

RichParse parseRich(const QString &source)
{
    RichParse out;
    if (source.size() > MaxSourceSize) { out.limited = true; return out; }
    // The core parser owns one-time extension registration.
    static const bool initialized = [] { parse(QString()); return true; }();
    Q_UNUSED(initialized);
    const int options = CMARK_OPT_VALIDATE_UTF8 | CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE;
    auto makeParser = [&] {
        auto parser = cmark_parser_new(options);
        for (const char *name : {"table", "strikethrough", "autolink", "tagfilter", "tasklist"})
            cmark_parser_attach_syntax_extension(parser, cmark_find_syntax_extension(name));
        return parser;
    };
    std::unique_ptr<cmark_parser, decltype(&cmark_parser_free)> parser(makeParser(), cmark_parser_free);
    auto bytes = source.toUtf8(); cmark_parser_feed(parser.get(), bytes.constData(), size_t(bytes.size()));
    Node root(cmark_parser_finish(parser.get()), cmark_node_free);
    if (!root) { out.limited = true; return out; }
    const auto maths = scanMath(source, root.get());
    QString masked;
    QVector<int> offsets;
    auto append = [&](QStringView text, int start, bool original) {
        for (int i = 0; i < text.size(); ++i) { masked += text[i]; offsets.append(start + (original ? i : 0)); }
    };
    int previous = 0;
    for (const auto &math : maths) {
        append(QStringView(source).mid(previous, math.start - previous), previous, true);
        if (math.standalone) append(QStringView(u"###### "), math.start, false);
        append(math.marker, math.start, false);
        for (int i = math.start; i < math.end; ++i) if (source[i] == u'\n') append(QStringView(u"\n"), i, false);
        previous = math.end;
    }
    append(QStringView(source).mid(previous), previous, true); offsets.append(int(source.size()));
    if (!maths.isEmpty()) {
        parser.reset(makeParser()); bytes = masked.toUtf8();
        cmark_parser_feed(parser.get(), bytes.constData(), size_t(bytes.size()));
        root.reset(cmark_parser_finish(parser.get()));
    }
    if (!root) { out.limited = true; return out; }
    Positions pos(masked);
    QSet<QString> prefixes;
    const QRegularExpression markers(QStringLiteral("RTRMARK[0-9]+Z"));
    auto matches = markers.globalMatch(source);
    while (matches.hasNext()) prefixes.insert(matches.next().captured());
    int salt = 0; QString prefix;
    do { prefix = QStringLiteral("RTRMARK%1Z").arg(salt++); } while (prefixes.contains(prefix));
    auto add = [&](RichLeaf leaf) {
        leaf.marker = prefix + QString::number(out.leaves.size()) + u'X';
        out.leaves.append(leaf); return leaf.marker;
    };
    auto custom = [](cmark_node *node, const QString &html) {
        auto replacement = cmark_node_new((cmark_node_get_type(node) & CMARK_NODE_TYPE_MASK) == CMARK_NODE_TYPE_BLOCK ? CMARK_NODE_CUSTOM_BLOCK : CMARK_NODE_CUSTOM_INLINE);
        const auto encoded = html.toUtf8(); cmark_node_set_on_enter(replacement, encoded.constData());
        cmark_node_replace(node, replacement);
        cmark_node_free(node);
    };
    QVector<cmark_node *> nodes;
    auto it = cmark_iter_new(root.get());
    for (auto event = cmark_iter_next(it); event != CMARK_EVENT_DONE; event = cmark_iter_next(it))
        if (event == CMARK_EVENT_ENTER) nodes.append(cmark_iter_get_node(it));
    cmark_iter_free(it);
    QSet<cmark_node *> imageChildren;
    int blocks = 0;
    for (auto node : nodes) {
        if (imageChildren.contains(node)) continue;
        const auto type = cmark_node_get_type(node);
        if (type == CMARK_NODE_DOCUMENT) continue;
        const QString kind = QString::fromLatin1(cmark_node_get_type_string(node));
        if ((type & CMARK_NODE_TYPE_MASK) == CMARK_NODE_TYPE_BLOCK && ++blocks > MaxBlocks) { out.limited = true; return out; }
        int ms = pos.at(cmark_node_get_start_line(node), cmark_node_get_start_column(node));
        int me = pos.at(cmark_node_get_end_line(node), cmark_node_get_end_column(node) + 1);
        bool inTable = false;
        for (auto parent = cmark_node_parent(node); parent; parent = cmark_node_parent(parent)) {
            if (QString::fromLatin1(cmark_node_get_type_string(parent)) != "table_cell") continue;
            inTable = true;
            const int first = pos.at(cmark_node_get_start_line(parent), cmark_node_get_start_column(parent));
            const int last = pos.at(cmark_node_get_end_line(parent), cmark_node_get_end_column(parent) + 1);
            QString cell; QVector<int> cellMap;
            for (int i = first; i < last; ++i) {
                cellMap.append(i);
                if (masked[i] == u'\\' && i + 1 < last && masked[i + 1] == u'|') ++i;
                cell += masked[i];
            }
            cellMap.append(last); Positions cellPositions(cell);
            const int column = cmark_node_get_start_column(parent);
            ms = cellMap[cellPositions.at(1, cmark_node_get_start_column(node) - column + 1)];
            me = cellMap[cellPositions.at(1, cmark_node_get_end_column(node) - column + 2)];
            break;
        }
        SourceRange range{offsets[qBound(0, ms, int(offsets.size()) - 1)], offsets[qBound(0, me, int(offsets.size()) - 1)]};
        if (type == CMARK_NODE_TEXT && range.end == range.start)
            range.end = qMin(int(source.size()), range.start + int(QString::fromUtf8(cmark_node_get_literal(node)).size()));
        if ((kind == "table_cell" || type == CMARK_NODE_HEADING) && !cmark_node_first_child(node)) {
            RichLeaf leaf; leaf.source = {range.end, range.end};
            if (kind == "table_cell" && cmark_node_get_start_column(node) == 0) {
                // GFM pads short rows with virtual cells. Materialize their
                // separators on the first edit instead of writing at offset 0.
                auto row = cmark_node_parent(node);
                const int rowEnd = offsets[pos.at(cmark_node_get_end_line(row), cmark_node_get_end_column(row) + 1)];
                leaf.source = {rowEnd, rowEnd};
                int skipped = 0;
                for (auto sibling = cmark_node_previous(node); sibling && cmark_node_get_start_column(sibling) == 0; sibling = cmark_node_previous(sibling)) ++skipped;
                const bool trailingPipe = rowEnd > 0 && source[rowEnd - 1] == u'|';
                leaf.insertionPrefix = QStringLiteral(" | ").repeated(skipped + (trailingPipe ? 0 : 1)) + (trailingPipe ? QStringLiteral(" ") : QString());
            }
            auto child = cmark_node_new(CMARK_NODE_TEXT); const auto marker = add(leaf).toUtf8();
            cmark_node_set_literal(child, marker.constData()); cmark_node_append_child(node, child);
        }
        if (type == CMARK_NODE_STRONG || type == CMARK_NODE_EMPH || type == CMARK_NODE_LINK || kind == "strikethrough"
            || type == CMARK_NODE_PARAGRAPH || type == CMARK_NODE_HEADING || type == CMARK_NODE_CODE_BLOCK || kind == "table") {
            RichRegion region{range, range.start, range.end, kind};
            if (type == CMARK_NODE_STRONG || type == CMARK_NODE_EMPH || kind == "strikethrough") {
                const int delimiter = type == CMARK_NODE_EMPH ? 1 : 2;
                region.innerStart += delimiter; region.innerEnd -= delimiter;
            }
            if (type == CMARK_NODE_LINK) {
                auto first = cmark_node_first_child(node), last = cmark_node_last_child(node);
                if (first && last) {
                    region.innerStart = offsets[pos.at(cmark_node_get_start_line(first), cmark_node_get_start_column(first))];
                    region.innerEnd = offsets[pos.at(cmark_node_get_end_line(last), cmark_node_get_end_column(last) + 1)];
                }
            }
            if (type == CMARK_NODE_HEADING) {
                const auto lead = QRegularExpression(QStringLiteral("^ {0,3}#{1,6}(?:[ \\t]+|$)")).match(source.mid(range.start, range.end - range.start));
                if (lead.hasMatch()) region.innerStart += int(lead.capturedLength());
            }
            out.regions.append(region);
        }
        if (type == CMARK_NODE_IMAGE) {
            auto descendants = cmark_iter_new(node);
            for (auto event = cmark_iter_next(descendants); event != CMARK_EVENT_DONE; event = cmark_iter_next(descendants))
                if (event == CMARK_EVENT_ENTER && cmark_iter_get_node(descendants) != node) imageChildren.insert(cmark_iter_get_node(descendants));
            cmark_iter_free(descendants);
            RichLeaf leaf; leaf.kind = RichLeaf::Image; leaf.source = range;
            leaf.url = QString::fromUtf8(cmark_node_get_url(node));
            custom(node, add(leaf)); continue;
        }
        if (type == CMARK_NODE_THEMATIC_BREAK) {
            RichLeaf leaf; leaf.kind = RichLeaf::Rule; leaf.source = range;
            custom(node, QStringLiteral("<p>%1</p>").arg(add(leaf))); continue;
        }
        if (type == CMARK_NODE_HTML_INLINE || type == CMARK_NODE_HTML_BLOCK) {
            RichLeaf leaf; leaf.source = range; leaf.text = source.mid(range.start, range.end - range.start);
            custom(node, type == CMARK_NODE_HTML_BLOCK ? QStringLiteral("<pre>%1</pre>").arg(add(leaf)) : add(leaf)); continue;
        }
        if (type != CMARK_NODE_TEXT && type != CMARK_NODE_CODE && type != CMARK_NODE_CODE_BLOCK) continue;
        RichLeaf leaf; leaf.source = range; leaf.text = QString::fromUtf8(cmark_node_get_literal(node)); leaf.table = inTable;
        leaf.kind = type == CMARK_NODE_TEXT ? RichLeaf::Text : RichLeaf::Code;
        if (type == CMARK_NODE_CODE) {
            // cmark reports the contents of a code span, excluding its fences.
            int ticks = 0; while (range.start > ticks && source[range.start - ticks - 1] == u'`') ++ticks;
            out.regions.append({{range.start - ticks, qMin(int(source.size()), range.end + ticks)}, range.start, range.end, QStringLiteral("code")});
        }
        if (type == CMARK_NODE_CODE_BLOCK) {
            leaf.language = QString::fromUtf8(cmark_node_get_fence_info(node)).section(u' ', 0, 0);
            int length = 0, indent = 0; char fence = 0;
            if (cmark_node_get_fenced(node, &length, &indent, &fence)) {
                const int content = source.indexOf(u'\n', range.start);
                const int closing = source.lastIndexOf(u'\n', qMax(range.start, range.end - 1)) + 1;
                const QString tail = source.mid(closing, range.end - closing).trimmed();
                const auto closingFence = QRegularExpression(QStringLiteral("^%1{%2,}[ \\t]*$").arg(QRegularExpression::escape(QString(QLatin1Char(fence)))).arg(length));
                if (content < 0 || closing <= range.start || !closingFence.match(tail).hasMatch()) {
                    leaf.kind = RichLeaf::Text; leaf.text = source.mid(range.start, range.end - range.start);
                    leaf.language.clear(); custom(node, QStringLiteral("<pre>%1</pre>").arg(add(leaf))); continue;
                }
                leaf.source = {content + 1, closing};
                if (!out.regions.isEmpty()) { out.regions.last().innerStart = content + 1; out.regions.last().innerEnd = closing; }
            } else if (leaf.source.end < source.size() && source[leaf.source.end] == u'\n') {
                ++leaf.source.end;
            }
        }
        // Math placeholders remain part of text leaves until they are replaced below.
        const auto token = add(leaf).toUtf8(); cmark_node_set_literal(node, token.constData());
    }
    std::unique_ptr<char, decltype(&std::free)> html(cmark_render_html(root.get(), options, cmark_parser_get_syntax_extensions(parser.get())), std::free);
    if (!html) { out.limited = true; return out; }
    out.html = QString::fromUtf8(html.get());
    if (source.endsWith(QStringLiteral("\n\n"))) {
        RichLeaf leaf; leaf.source = {int(source.size()), int(source.size())};
        out.html += QStringLiteral("<p>%1</p>").arg(add(leaf));
    }
    const int initialLeaves = out.leaves.size();
    for (int i = 0; i < initialLeaves; ++i) {
        auto &leaf = out.leaves[i];
        QString content = leaf.text.toHtmlEscaped();
        if (leaf.kind == RichLeaf::Text) {
            for (const auto &math : maths) {
                if (!content.contains(math.marker)) continue;
                // The parent text leaf maps its object character to this source range.
                content.replace(math.marker, QStringLiteral("<img src=\"rtr-math:/%1\">").arg(math.start));
            }
        }
        if (leaf.kind == RichLeaf::Image || leaf.kind == RichLeaf::Rule)
            content = QStringLiteral("<img src=\"rtr-object:/%1\">").arg(i);
        out.html.replace(leaf.marker, leaf.marker + QStringLiteral("BEGIN") + content + leaf.marker + QStringLiteral("END"));
    }
    for (const auto &math : maths) {
        RichLeaf leaf; leaf.kind = RichLeaf::Math; leaf.source = {math.start, math.end}; leaf.text = math.source; leaf.display = math.display;
        leaf.url = QStringLiteral("rtr-math:/%1").arg(math.start); out.leaves.append(leaf);
        if (math.standalone) {
            // Generated headings only delimit math; they are not semantic headings.
            const QRegularExpression block(QStringLiteral("<h6>([^<]*<img src=\"%1\">[^<]*)</h6>").arg(QRegularExpression::escape(leaf.url)));
            out.html.replace(block, QStringLiteral("<p align=\"center\">\\1</p>"));
        }
    }
    out.html.replace(QRegularExpression(QStringLiteral("<input[^>]*checked[^>]*>")), QStringLiteral("&#9745; "));
    out.html.replace(QRegularExpression(QStringLiteral("<input[^>]*>")), QStringLiteral("&#9744; "));
    return out;
}
QString decodeEntity(const QString &source)
{
    const auto bytes = source.toUtf8();
    cmark_strbuf buffer = CMARK_BUF_INIT(cmark_get_default_mem_allocator());
    houdini_unescape_html_f(&buffer, reinterpret_cast<const uint8_t *>(bytes.constData()), bufsize_t(bytes.size()));
    const auto result = QString::fromUtf8(reinterpret_cast<const char *>(buffer.ptr), buffer.size);
    cmark_strbuf_free(&buffer);
    return result;
}
}
