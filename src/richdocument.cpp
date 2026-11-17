#include "richdocument.h"
#include "mathrenderer.h"
#include "mediastore.h"
#include <QAbstractTextDocumentLayout>
#include <QImageReader>
#include <QFileInfo>
#include <QPainter>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <cmath>

namespace MarkdownLive {
RichObjects::RichObjects(QTextDocument *doc, MathRenderer *math, MediaStore *media)
    : QObject(doc), m_document(doc), m_math(math), m_media(media)
{
    doc->documentLayout()->registerHandler(Type, this);
}
QSizeF RichObjects::fit(QSizeF size) const
{
    if (size.isEmpty()) return {140, 26};
    const qreal scale = qMin(1.0, qMin(m_width / size.width(), 640 / size.height()));
    return size * scale;
}
void RichObjects::reset(const RichParse &parsed, const QUrl &base, const QPalette &palette, qreal width)
{
    m_objects.clear(); m_rasters.clear(); m_base = base; m_palette = palette; m_width = qMax(40.0, width);
    for (const auto &leaf : parsed.leaves) {
        Object item; item.leaf = leaf;
        if (leaf.kind == RichLeaf::Image) {
            const auto url = base.resolved(QUrl(leaf.url));
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).size() <= 8 * 1024 * 1024) {
                QImageReader reader(url.toLocalFile());
                item.natural = reader.size();
                item.ready = true; // Pixel decoding is deferred to painting; geometry is already known.
            } else m_media->request(url);
        } else if (leaf.kind == RichLeaf::Math) {
            if (const auto result = m_math->request(leaf.text, leaf.display)) {
                item.natural = QSizeF(result->width, result->height) * QFontMetricsF(m_document->defaultFont()).xHeight();
                item.ready = true;
            }
        } else if (leaf.kind == RichLeaf::Rule) { item.natural = {m_width, 12}; item.ready = true; }
        item.size = fit(item.natural); m_objects.append(item);
    }
}
void RichObjects::setWidth(qreal width)
{
    width = qMax(40.0, width);
    if (qFuzzyCompare(width, m_width)) return;
    m_width = width;
    for (auto &item : m_objects) {
        if (item.leaf.kind == RichLeaf::Rule) item.natural.setWidth(width);
        item.size = fit(item.natural);
    }
    m_rasters.clear();
    m_document->markContentsDirty(0, m_document->characterCount());
}
QSizeF RichObjects::intrinsicSize(QTextDocument *, int, const QTextFormat &format)
{
    const int id = format.intProperty(Id);
    return id >= 0 && id < m_objects.size() ? m_objects[id].size : QSizeF(140, 26);
}
void RichObjects::updateResources()
{
    bool changed = false;
    for (auto &item : m_objects) {
        if (item.ready) continue;
        if (item.leaf.kind == RichLeaf::Math) {
            const auto result = m_math->request(item.leaf.text, item.leaf.display);
            if (!result) continue;
            item.natural = QSizeF(result->width, result->height) * QFontMetricsF(m_document->defaultFont()).xHeight();
        } else if (item.leaf.kind == RichLeaf::Image) {
            const auto result = m_media->request(m_base.resolved(QUrl(item.leaf.url)));
            if (!result) continue;
            item.natural = result->image.size();
        } else continue;
        item.ready = true;
        const auto size = fit(item.natural);
        changed |= size != item.size; item.size = size;
    }
    if (changed) m_document->markContentsDirty(0, m_document->characterCount());
    Q_EMIT m_document->documentLayout()->update();
}
void RichObjects::drawObject(QPainter *p, const QRectF &rect, QTextDocument *, int, const QTextFormat &format)
{
    const int id = format.intProperty(Id);
    if (id < 0 || id >= m_objects.size()) return;
    const auto &item = m_objects[id];
    p->save(); p->setPen(m_palette.color(QPalette::Text));
    if (item.leaf.kind == RichLeaf::Rule) {
        p->drawLine(rect.left(), rect.center().y(), rect.right(), rect.center().y());
    } else if (item.leaf.kind == RichLeaf::Image) {
        const auto result = m_media->request(m_base.resolved(QUrl(item.leaf.url)));
        if (result && !result->image.isNull()) { p->setRenderHint(QPainter::SmoothPixmapTransform); p->drawImage(rect, result->image); }
        else p->drawText(rect, Qt::AlignCenter, result ? tr("[image unavailable]") : tr("[loading image]"));
    } else if (item.leaf.kind == RichLeaf::Math) {
        const QString key = QString::number(id);
        auto raster = m_rasters.object(key);
        if (!raster) {
            const auto result = m_math->request(item.leaf.text, item.leaf.display);
            if (result && result->error.isEmpty()) {
                QByteArray svg = result->svg;
                svg.replace("currentColor", m_palette.color(QPalette::Text).name().toUtf8());
                QSvgRenderer renderer(svg);
                if (renderer.isValid()) {
                    const QSize pixels(int(std::ceil(rect.width() * 2)), int(std::ceil(rect.height() * 2)));
                    auto image = new QImage(pixels, QImage::Format_ARGB32_Premultiplied);
                    image->fill(Qt::transparent);
                    QPainter painter(image); renderer.render(&painter); painter.end();
                    const int cost = qMax(1, int(image->sizeInBytes() / 1024));
                    m_rasters.insert(key, image, cost); raster = m_rasters.object(key);
                }
            }
            if (!raster) p->drawText(rect, Qt::AlignCenter, result ? tr("[invalid formula]") : tr("[rendering formula]"));
        }
        if (raster) p->drawImage(rect, *raster);
    }
    p->restore();
}

RichDocument::RichDocument(MathRenderer *math, MediaStore *media, QObject *parent)
    : QTextDocument(parent), m_objects(new RichObjects(this, math, media))
{
    setUndoRedoEnabled(false); setDocumentMargin(20);
}

bool RichDocument::render(const QString &source, const QUrl &base, const QPalette &palette, qreal width)
{
    m_source = source; m_parse = parseRich(source);
    if (m_parse.limited) return false;
    m_objects->reset(m_parse, base, palette, width);
    setHtml(m_parse.html.trimmed());
    QString raw = toRawText();
    // Recreate the vector: Qt 6.11 QList::fill can reuse front-slack left by
    // marker removal without reserving enough space at the end.
    m_map = QVector<SourceRange>(raw.size(), SourceRange{-1, -1});
    QVector<QPair<int, int>> remove;
    QVector<QPair<QTextCursor, int>> anchors;
    QVector<QString> prefixes;
    m_anchors.clear(); m_prefixes.clear();
    QHash<QString, int> objects;
    for (int id = 0; id < m_parse.leaves.size(); ++id) {
        const auto &leaf = m_parse.leaves[id];
        if (leaf.kind == RichLeaf::Math) objects.insert(leaf.url, id);
        else if (leaf.kind == RichLeaf::Image || leaf.kind == RichLeaf::Rule)
            objects.insert(QStringLiteral("rtr-object:/%1").arg(id), id);
    }
    for (const auto &leaf : m_parse.leaves) {
        if (leaf.marker.isEmpty()) continue;
        const QString begin = leaf.marker + QStringLiteral("BEGIN"), end = leaf.marker + QStringLiteral("END");
        const int a = raw.indexOf(begin), b = raw.indexOf(end, a + begin.size());
        if (a < 0 || b < 0) return false;
        remove.append({a, int(begin.size())}); remove.append({b, int(end.size())});
        if (a + begin.size() == b) {
            QTextCursor anchor(this); anchor.setPosition(b); anchors.append({anchor, leaf.source.start});
            prefixes.append(leaf.insertionPrefix);
        }
        int at = leaf.source.start;
        for (int j = a + int(begin.size()); j < b; ++j) {
            if (leaf.kind == RichLeaf::Code && leaf.text.contains(u'\n')
                && (j == a + begin.size() || raw[j - 1] == QChar::ParagraphSeparator)) {
                int endOfLine = raw.indexOf(QChar::ParagraphSeparator, j);
                if (endOfLine < 0 || endOfLine > b) endOfLine = b;
                int sourceEnd = source.indexOf(u'\n', at);
                if (sourceEnd < 0 || sourceEnd > leaf.source.end) sourceEnd = leaf.source.end;
                const QString line = raw.mid(j, endOfLine - j);
                if (source.mid(at, sourceEnd - at).endsWith(line)) at = sourceEnd - int(line.size());
            }
            if (raw[j] == QChar::ObjectReplacementCharacter) {
                QTextCursor cursor(this); cursor.setPosition(j); cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                const auto name = cursor.charFormat().toImageFormat().name();
                if (!objects.contains(name)) return false;
                const int id = objects.value(name); const auto &object = m_parse.leaves[id];
                m_map[j] = object.source; at = object.source.end;
                QTextCharFormat format; format.setObjectType(RichObjects::Type); format.setProperty(RichObjects::Id, id);
                format.setVerticalAlignment(QTextCharFormat::AlignMiddle);
                format.setToolTip(object.kind == RichLeaf::Math ? object.text : object.url);
                cursor.setCharFormat(format); continue;
            }
            int start = at;
            // cmark has already identified the semantic leaf; decode only within
            // that leaf, never search for a repeated visible word in the document.
            if (at < source.size() && source[at] == u'\\' && at + 1 < leaf.source.end
                && ((leaf.kind == RichLeaf::Text && source[at + 1].unicode() < 128
                    && !source[at + 1].isLetterOrNumber() && !source[at + 1].isSpace())
                    || (leaf.table && source[at + 1] == u'|'))) ++at;
            if (leaf.kind == RichLeaf::Text && at < source.size() && source[at] == u'&') {
                const int semicolon = source.indexOf(u';', at + 1);
                if (semicolon > at && semicolon - at <= 34 && semicolon < leaf.source.end) {
                    const auto encoded = source.mid(at, semicolon - at + 1);
                    const auto decoded = decodeEntity(encoded);
                    if (decoded != encoded && !decoded.isEmpty() && raw.mid(j, decoded.size()) == decoded) {
                        for (int k = 0; k < decoded.size(); ++k) m_map[j + k] = {start, semicolon + 1};
                        j += int(decoded.size()) - 1; at = semicolon + 1; continue;
                    }
                }
            }
            // Code spans normalize newlines to spaces; HTML import collapses
            // consecutive spaces in normal prose. Consume that exact whitespace.
            const auto matches = [&](QChar c) { return c == raw[j] || (c.isSpace() && raw[j].isSpace()); };
            while (at < leaf.source.end && !matches(source[at]) && source[at].isSpace()) { ++at; start = at; }
            if (at == leaf.source.end && leaf.kind == RichLeaf::Code && raw[j] == QChar::ParagraphSeparator) {
                m_map[j] = {at, at}; continue; // cmark supplies a final code newline at EOF.
            }
            if (at >= leaf.source.end || !matches(source[at])) return false;
            ++at;
            m_map[j] = {start, at};
        }
    }
    std::sort(remove.begin(), remove.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QTextCursor cursor(this); cursor.beginEditBlock();
    for (const auto &[position, length] : remove) {
        cursor.setPosition(position); cursor.setPosition(position + length, QTextCursor::KeepAnchor); cursor.removeSelectedText();
        m_map.remove(position, length);
    }
    cursor.endEditBlock();
    for (int i = 0; i < anchors.size(); ++i) {
        m_anchors.insert(anchors[i].first.position(), anchors[i].second);
        m_prefixes.insert(anchors[i].first.position(), prefixes[i]);
    }
    // Native paragraph and table boundaries have no leaf. Map them to the
    // source gap between semantic blocks, leaving inline closing markers intact.
    for (int i = 0; i < m_map.size(); ++i) if (m_map[i].start < 0) {
        int left = i ? m_map[i - 1].end : 0;
        int next = i + 1; while (next < m_map.size() && m_map[next].start < 0) ++next;
        int right = next < m_map.size() ? m_map[next].start : int(source.size());
        for (const auto &region : m_parse.regions) {
            if (region.kind == "paragraph" || region.kind == "heading" || region.kind == "code_block") {
                if (region.source.start < left && region.source.end >= left && region.source.end <= right) left = region.source.end;
                if (region.source.start >= left && region.innerStart <= right) right = region.innerStart;
            }
        }
        m_map[i] = {left, qMax(left, right)};
    }
    const auto visible = toRawText();
    for (int i = 0; i < visible.size(); ++i) if (visible[i] == QChar(0x2610) || visible[i] == QChar(0x2611)) {
        int next = i + 1; while (next < m_map.size() && visible[next].isSpace()) ++next;
        if (next >= m_map.size()) continue;
        const int bracket = source.lastIndexOf(u'[', m_map[next].start);
        if (bracket >= 0 && bracket + 2 < source.size() && source[bracket + 2] == u']') {
            m_map[i] = {bracket, bracket + 3};
            if (i + 1 < next) m_map[i + 1] = {bracket + 3, m_map[next].start};
        }
    }
    cursor.select(QTextCursor::Document);
    QTextCharFormat family; family.setFontFamilies({defaultFont().family()}); cursor.mergeCharFormat(family);
    // Force Qt to finish layout once. Scrolling must never discover new heights.
    setTextWidth(width + 2 * documentMargin()); documentLayout()->documentSize();
    return true;
}
int RichDocument::sourcePosition(int display, bool right) const
{
    display = qBound(0, display, int(m_map.size()));
    if (m_anchors.contains(display)) return m_anchors.value(display);
    if (m_map.isEmpty()) return int(m_source.size());
    if (right && display < m_map.size()) return m_map[display].start;
    return display ? m_map[display - 1].end : m_map[0].start;
}
int RichDocument::displayPosition(int source) const
{
    for (auto it = m_anchors.cbegin(); it != m_anchors.cend(); ++it) if (it.value() == source) return it.key();
    for (int i = 0; i < m_map.size(); ++i) {
        if (source <= m_map[i].start) return i;
        if (source < m_map[i].end) return i + 1;
    }
    return int(m_map.size());
}
SourceRange RichDocument::sourceSelection(int start, int end, bool includeMarkers) const
{
    if (start == end) return {sourcePosition(start), sourcePosition(end)};
    if (start == 0 && end == m_map.size()) return {0, int(m_source.size())};
    SourceRange range{sourcePosition(start, true), sourcePosition(end)};
    for (auto it = m_parse.regions.crbegin(); it != m_parse.regions.crend(); ++it) {
        if (it->kind != "strong" && it->kind != "emph" && it->kind != "link" && it->kind != "strikethrough" && it->kind != "code") continue;
        if (it->source.start >= range.end || it->source.end <= range.start) continue;
        const int a = displayPosition(it->source.start), b = displayPosition(it->source.end);
        if (start <= a && end >= b && (includeMarkers || start < a || end > b)) {
            range.start = qMin(range.start, it->source.start); range.end = qMax(range.end, it->source.end);
        }
    }
    return range;
}
QString RichDocument::languageAt(int position) const
{
    const int source = sourcePosition(position, true);
    for (const auto &leaf : m_parse.leaves)
        if (leaf.kind == RichLeaf::Code && source >= leaf.source.start && source < leaf.source.end) return leaf.language;
    return {};
}
QString RichDocument::repairSelection(SourceRange range, const QString &inserted) const
{
    if (range.start == range.end) return inserted;
    QString closing, opening;
    for (const auto &region : m_parse.regions) {
        if (region.kind != "strong" && region.kind != "emph" && region.kind != "strikethrough" && region.kind != "code" && region.kind != "link") continue;
        if (range.start > region.innerStart && range.start < region.innerEnd && range.end >= region.source.end)
            closing.prepend(m_source.mid(region.innerEnd, region.source.end - region.innerEnd));
        if (range.start <= region.source.start && range.end > region.innerStart && range.end < region.innerEnd)
            opening += m_source.mid(region.source.start, region.innerStart - region.source.start);
    }
    return inserted + closing + opening;
}
bool RichDocument::adoptPlainEdit(const QString &source, int displayStart, int removed, const QString &inserted, SourceRange range)
{
    // Interior word edits cannot change CommonMark structure. Keep Qt's native
    // edit and move source ranges instead of reparsing/layouting the document.
    const auto letters = [](QStringView text) {
        for (QChar c : text) if (!c.isLetterOrNumber()) return false;
        return true;
    };
    if (range.start <= 0 || range.end >= m_source.size()
        || !m_source[range.start - 1].isLetterOrNumber() || !m_source[range.end].isLetterOrNumber()
        || !letters(inserted) || !letters(QStringView(m_source).mid(range.start, range.end - range.start))
        || removed != range.end - range.start) return false;
    bool insideText = false;
    for (const auto &leaf : m_parse.leaves)
        if (leaf.kind == RichLeaf::Text && range.start > leaf.source.start && range.end < leaf.source.end) insideText = true;
    if (!insideText || QString(m_source).replace(range.start, range.end - range.start, inserted) != source) return false;
    const int delta = int(inserted.size()) - (range.end - range.start);
    const auto shift = [&](int &offset) { if (offset >= range.end) offset += delta; };
    for (auto &entry : m_map) { shift(entry.start); if (entry.end > range.start) entry.end += delta; }
    m_map.remove(displayStart, removed);
    for (int i = 0; i < inserted.size(); ++i) m_map.insert(displayStart + i, {range.start + i, range.start + i + 1});
    for (auto &leaf : m_parse.leaves) { shift(leaf.source.start); shift(leaf.source.end); }
    for (auto &region : m_parse.regions) {
        shift(region.source.start); shift(region.source.end); shift(region.innerStart); shift(region.innerEnd);
    }
    QHash<int, int> anchors;
    QHash<int, QString> prefixes;
    for (auto it = m_anchors.cbegin(); it != m_anchors.cend(); ++it) {
        int sourceOffset = it.value(); shift(sourceOffset);
        const int display = it.key() >= displayStart + removed ? it.key() + int(inserted.size()) - removed : it.key();
        anchors.insert(display, sourceOffset); prefixes.insert(display, m_prefixes.value(it.key()));
    }
    m_anchors.swap(anchors);
    m_prefixes.swap(prefixes);
    m_source = source;
    return true;
}
}
