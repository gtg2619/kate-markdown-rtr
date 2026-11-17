#include "mathsyntax.h"
#include <cmark-gfm.h>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace MarkdownLive {
QVector<MathSpan> scanMath(const QString &source, cmark_node *tree)
{
    QVector<int> starts{0};
    for (int i = 0; i < source.size(); ++i) if (source[i] == u'\n') starts.append(i + 1);
    starts.append(int(source.size()));
    QByteArray protectedText(source.size(), 0);
    QHash<int, QVector<int>> columns;
    auto offset = [&](int line, int column) {
        line = qBound(0, line - 1, int(starts.size()) - 2);
        auto it = columns.find(line);
        if (it == columns.end()) {
            QVector<int> map;
            for (int i = starts[line]; i < starts[line + 1];) {
                int units = source[i].isHighSurrogate() && i + 1 < starts[line + 1] && source[i + 1].isLowSurrogate() ? 2 : 1;
                const int bytes = int(source.mid(i, units).toUtf8().size());
                for (int b = 0; b < bytes; ++b) map.append(i);
                i += units;
            }
            map.append(starts[line + 1]);
            it = columns.insert(line, map);
        }
        return it.value()[qBound(0, column - 1, int(it->size()) - 1)];
    };
    auto iterator = cmark_iter_new(tree);
    for (auto event = cmark_iter_next(iterator); event != CMARK_EVENT_DONE; event = cmark_iter_next(iterator)) {
        if (event != CMARK_EVENT_ENTER) continue;
        auto node = cmark_iter_get_node(iterator);
        const auto type = cmark_node_get_type(node);
        if (type != CMARK_NODE_CODE && type != CMARK_NODE_CODE_BLOCK && type != CMARK_NODE_HTML_BLOCK
            && type != CMARK_NODE_HTML_INLINE && type != CMARK_NODE_LINK && type != CMARK_NODE_IMAGE) continue;
        const int startLine = cmark_node_get_start_line(node);
        const int endLine = cmark_node_get_end_line(node);
        if (startLine <= 0 || endLine < startLine || endLine >= starts.size()) continue;
        const bool block = type == CMARK_NODE_CODE_BLOCK || type == CMARK_NODE_HTML_BLOCK;
        const int begin = block ? starts[startLine - 1] : offset(startLine, cmark_node_get_start_column(node));
        const int end = block ? starts[endLine] : offset(endLine, cmark_node_get_end_column(node) + 1);
        for (int i = begin; i < qMin(end, int(source.size())); ++i) protectedText[i] = 1;
    }
    cmark_iter_free(iterator);
    QSet<QString> usedPrefixes;
    static const QRegularExpression markers(QStringLiteral("KATEMATH[0-9]+Z"));
    auto matches = markers.globalMatch(source);
    while (matches.hasNext()) usedPrefixes.insert(matches.next().captured());
    int prefixId = 0;
    QString prefix;
    do { prefix = QStringLiteral("KATEMATH%1Z").arg(prefixId++); } while (usedPrefixes.contains(prefix));
    QByteArray escapes(source.size(), 0);
    bool slash = false;
    for (int i = 0; i < source.size(); ++i) {
        escapes[i] = slash;
        slash = source[i] == u'\\' && !slash;
    }
    auto escaped = [&](int position) { return escapes[position] != 0; };
    // Unclosed delimiters must not cause quadratic rescanning of hostile input.
    qsizetype work = 0;
    QVector<MathSpan> result;
    for (int i = 0; i < source.size();) {
        if (protectedText[i] || escaped(i)) { ++i; continue; }
        QString closing;
        int openSize = 0;
        bool display = false;
        if (source[i] == u'$') {
            display = i + 1 < source.size() && source[i + 1] == u'$';
            openSize = display ? 2 : 1;
            closing = display ? QStringLiteral("$$") : QStringLiteral("$");
            if (!display && (i + 1 == source.size() || source[i + 1].isSpace())) { ++i; continue; }
        } else if (source[i] == u'\\' && i + 1 < source.size() && (source[i + 1] == u'(' || source[i + 1] == u'[')) {
            display = source[i + 1] == u'[';
            openSize = 2;
            closing = display ? QStringLiteral("\\]") : QStringLiteral("\\)");
        } else { ++i; continue; }
        int end = i + openSize;
        bool found = false;
        for (; end < source.size() && end - i <= 8192; ++end) {
            if (++work > 32 * source.size()) return {};
            if (protectedText[end]) break;
            if (!display && source[end] == u'\n') break;
            if (!QStringView(source).mid(end).startsWith(closing) || escaped(end)) continue;
            if (closing == QStringLiteral("$") && (end == i + 1 || source[end - 1].isSpace()
                || (end + 1 < source.size() && (source[end + 1].isDigit() || source[end + 1] == u'$')))) continue;
            found = true;
            break;
        }
        if (!found) { i += openSize; continue; }
        const int lineStart = i == 0 ? 0 : int(source.lastIndexOf(u'\n', i - 1) + 1);
        const int nextNewline = source.indexOf(u'\n', end + closing.size());
        const int lineEnd = nextNewline < 0 ? int(source.size()) : nextNewline;
        const auto lead = source.mid(lineStart, i - lineStart);
        static const QRegularExpression container(QStringLiteral("^\\s*(?:>\\s*)*(?:(?:[-+*]|[0-9]+[.)])\\s+)?$"));
        const bool standalone = display && container.match(lead).hasMatch()
            && source.mid(end + closing.size(), lineEnd - end - closing.size()).trimmed().isEmpty();
        QString math = source.mid(i + openSize, end - i - openSize);
        if (standalone && lead.contains(u'>')) {
            const QString quotePrefix = lead.left(lead.lastIndexOf(u'>') + 1);
            math.replace(u'\n' + quotePrefix, QStringLiteral("\n"));
        }
        result.append({i, end + int(closing.size()), math, prefix + QString::number(result.size()) + QStringLiteral("END"), display, standalone});
        if (result.size() > 4096) return {};
        i = end + int(closing.size());
    }
    return result;
}

QString maskMath(const QString &source, const QVector<MathSpan> &math)
{
    QString masked;
    int previous = 0;
    for (const auto &span : math) {
        masked += source.mid(previous, span.start - previous);
        // A generated heading interrupts adjacent paragraphs without introducing
        // new lines. Source line numbers remain the original document's numbers.
        if (span.standalone) masked += QStringLiteral("###### ");
        masked += span.marker;
        masked += QString(source.mid(span.start, span.end - span.start).count(u'\n'), u'\n');
        previous = span.end;
    }
    masked += source.mid(previous);
    return masked;
}
}
