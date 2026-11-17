#pragma once
#include "markdown.h"
#include <QTextDocument>
#include <QPalette>

namespace MarkdownLive {
class MathRenderer;
class MediaStore;
class RenderDocument : public QTextDocument
{
public:
    RenderDocument(const Block &block, const QUrl &base, const QFont &font, const QPalette &palette,
        const QString &css, int width, qreal pixelRatio, MathRenderer *math, MediaStore *media);
protected:
    QVariant loadResource(int type, const QUrl &name) override;
private:
    QUrl m_base;
    MediaStore *m_media;
    int m_width;
    qsizetype m_imageBytes = 0;
    QPalette m_palette;
};
}
