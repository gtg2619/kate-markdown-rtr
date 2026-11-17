#include "markdown.h"
#include "mathsyntax.h"

#include <cmark-gfm.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm-core-extensions.h>
#include <QRegularExpression>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace MarkdownLive {
namespace {
QString literal(const QString &source)
{
    return QStringLiteral("<pre>%1</pre>").arg(source.toHtmlEscaped());
}
}

ParseResult parse(const QString &source)
{
    ParseResult result;
    if (source.size() > MaxSourceSize) {
        result.limited = true;
        return result;
    }
    static std::once_flag registered;
    std::call_once(registered, cmark_gfm_core_extensions_ensure_registered);
    const int options = CMARK_OPT_VALIDATE_UTF8 | CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE;
    std::unique_ptr<cmark_parser, decltype(&cmark_parser_free)> parser(cmark_parser_new(options), cmark_parser_free);
    if (!parser) { result.limited = true; return result; }
    for (const char *name : {"table", "strikethrough", "autolink", "tagfilter", "tasklist"}) {
        cmark_parser_attach_syntax_extension(parser.get(), cmark_find_syntax_extension(name));
    }
    const auto utf8 = source.toUtf8();
    cmark_parser_feed(parser.get(), utf8.constData(), size_t(utf8.size()));
    std::unique_ptr<cmark_node, decltype(&cmark_node_free)> root(cmark_parser_finish(parser.get()), cmark_node_free);
    if (!root) { result.limited = true; return result; }
    const auto math = scanMath(source, root.get());
    if (!math.isEmpty()) {
        const auto masked = maskMath(source, math).toUtf8();
        parser.reset(cmark_parser_new(options));
        for (const char *name : {"table", "strikethrough", "autolink", "tagfilter", "tasklist"})
            cmark_parser_attach_syntax_extension(parser.get(), cmark_find_syntax_extension(name));
        cmark_parser_feed(parser.get(), masked.constData(), size_t(masked.size()));
        root.reset(cmark_parser_finish(parser.get()));
        if (!root) { result.limited = true; return result; }
    }

    QVector<int> lines{0};
    for (int i = 0; i < source.size(); ++i) if (source[i] == u'\n') lines.append(i + 1);
    lines.append(int(source.size()));
    int consumed = 0;
    qsizetype mathIndex = 0;
    auto gap = [&](int end) {
        if (end <= consumed) return;
        const auto text = source.mid(consumed, end - consumed);
        const bool blank = text.trimmed().isEmpty();
        result.blocks.append({consumed, end, blank ? QString() : literal(text), blank, {}, {}});
        consumed = end;
    };
    for (auto node = cmark_node_first_child(root.get()); node; node = cmark_node_next(node)) {
        if (result.blocks.size() >= MaxBlocks) { result.blocks.clear(); result.limited = true; return result; }
        const int startLine = cmark_node_get_start_line(node) - 1;
        const int endLine = cmark_node_get_end_line(node);
        if (startLine < 0 || endLine <= startLine || endLine >= lines.size()) continue;
        const int start = lines[startLine];
        int end = lines[endLine];
        if (start < consumed || end <= start) continue;
        gap(start);
        QString html;
        QVector<MathSpan> blockMath;
        while (mathIndex < math.size() && math[mathIndex].start < start) ++mathIndex;
        while (mathIndex < math.size() && math[mathIndex].start < end) {
                const auto &span = math[mathIndex++];
                blockMath.append(span);
                if (span.end > end) {
                    const auto newline = source.indexOf(u'\n', span.end);
                    end = newline < 0 ? int(source.size()) : int(newline + 1);
                }
        }
        // Very large individual blocks remain literal; avoid pathological table/layout work.
        if (end - start > 64 * 1024) {
            html = literal(source.mid(start, end - start));
            blockMath.clear();
        } else {
            std::unique_ptr<char, decltype(&std::free)> rendered(
                cmark_render_html(node, options, cmark_parser_get_syntax_extensions(parser.get())), std::free);
            if (!rendered) { result.blocks.clear(); result.limited = true; return result; }
            html = QString::fromUtf8(rendered.get());
            for (const auto &span : blockMath) {
                const QString image = QStringLiteral("<img src=\"katemath:/%1\" alt=\"%2\">").arg(span.marker, span.source.toHtmlEscaped());
                if (span.standalone)
                    html.replace(QStringLiteral("<h6>%1</h6>").arg(span.marker), QStringLiteral("<p align=\"center\">%1</p>").arg(image));
                const QString markerInText = span.marker;
                if (!span.standalone) html.replace(markerInText, image);
            }
            // QTextDocument has no HTML form controls. Render GFM tasks as native glyphs.
            static const QRegularExpression checkbox(QStringLiteral("<input[^>]*>"));
            auto matches = checkbox.globalMatch(html);
            QVector<QRegularExpressionMatch> found;
            while (matches.hasNext()) found.append(matches.next());
            for (auto it = found.crbegin(); it != found.crend(); ++it) {
                html.replace(it->capturedStart(), it->capturedLength(),
                    it->captured().contains(QStringLiteral("checked")) ? QStringLiteral("&#9745; ") : QStringLiteral("&#9744; "));
            }
        }
        QString language;
        if (cmark_node_get_type(node) == CMARK_NODE_CODE_BLOCK)
            language = QString::fromUtf8(cmark_node_get_fence_info(node)).section(u' ', 0, 0);
        result.blocks.append({start, end, html, false, language, blockMath});
        consumed = end;
    }
    gap(int(source.size()));
    if (result.blocks.size() > MaxBlocks) { result.blocks.clear(); result.limited = true; }
    if (!result.limited && result.blocks.isEmpty()) result.blocks.append({0, 0, {}, true, {}, {}});
    return result;
}

Edit difference(const QString &before, const QString &after)
{
    int prefix = 0;
    const int common = int(qMin(before.size(), after.size()));
    while (prefix < common && before[prefix] == after[prefix]) ++prefix;
    if (prefix > 0 && prefix < before.size() && before[prefix].isLowSurrogate()) --prefix;
    int oldEnd = int(before.size()), newEnd = int(after.size());
    while (oldEnd > prefix && newEnd > prefix && before[oldEnd - 1] == after[newEnd - 1]) { --oldEnd; --newEnd; }
    if (oldEnd < before.size() && oldEnd > prefix && before[oldEnd].isLowSurrogate()) { ++oldEnd; ++newEnd; }
    return {prefix, oldEnd - prefix, after.mid(prefix, newEnd - prefix)};
}
}
