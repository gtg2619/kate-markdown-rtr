#pragma once
#include "richparser.h"
#include <QTextDocument>
#include <QTextObjectInterface>
#include <QCache>
#include <QPalette>

namespace MarkdownLive {
class MathRenderer;
class MediaStore;
class RichObjects : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    RichObjects(QTextDocument *, MathRenderer *, MediaStore *);
    void reset(const RichParse &, const QUrl &, const QPalette &, qreal width);
    QSizeF intrinsicSize(QTextDocument *, int, const QTextFormat &) override;
    void drawObject(QPainter *, const QRectF &, QTextDocument *, int, const QTextFormat &) override;
    void updateResources();
    void setWidth(qreal);
    static constexpr int Type = QTextFormat::UserObject + 1;
    static constexpr int Id = QTextFormat::UserProperty + 1;
private:
    struct Object { RichLeaf leaf; QSizeF natural, size; bool ready = false; };
    QSizeF fit(QSizeF) const;
    QTextDocument *m_document;
    MathRenderer *m_math;
    MediaStore *m_media;
    QVector<Object> m_objects;
    QUrl m_base;
    QPalette m_palette;
    qreal m_width = 600;
    QCache<QString, QImage> m_rasters{8192};
};

class RichDocument : public QTextDocument
{
public:
    RichDocument(MathRenderer *, MediaStore *, QObject *parent);
    bool render(const QString &, const QUrl &, const QPalette &, qreal width);
    int sourcePosition(int display, bool right = false) const;
    int displayPosition(int source) const;
    SourceRange sourceSelection(int start, int end, bool includeMarkers = true) const;
    QString insertionPrefix(int display) const { return m_prefixes.value(display); }
    QString repairSelection(SourceRange range, const QString &inserted) const;
    bool adoptPlainEdit(const QString &source, int displayStart, int removed, const QString &inserted, SourceRange range);
    const QVector<RichRegion> &regions() const { return m_parse.regions; }
    const QVector<RichLeaf> &leaves() const { return m_parse.leaves; }
    void updateResources() { m_objects->updateResources(); }
    void setObjectWidth(qreal width) { m_objects->setWidth(width); }
    QString languageAt(int position) const;
private:
    QString m_source;
    RichParse m_parse;
    QVector<SourceRange> m_map;
    QHash<int, int> m_anchors;
    QHash<int, QString> m_prefixes;
    RichObjects *m_objects;
};
}
