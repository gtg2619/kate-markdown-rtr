#include "renderdocument.h"
#include "mathrenderer.h"
#include "mediastore.h"
#include <QFontMetricsF>
#include <QPainter>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextCursor>
#include <cmath>

namespace MarkdownLive {
RenderDocument::RenderDocument(const Block &block, const QUrl &base, const QFont &font, const QPalette &palette,
    const QString &css, int width, qreal pixelRatio, MathRenderer *math, MediaStore *media)
    : m_base(base), m_media(media), m_width(width), m_palette(palette)
{
    setDocumentMargin(0);
    setDefaultFont(font);
    setDefaultStyleSheet(css);
    auto option = defaultTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setDefaultTextOption(option);
    QString html = block.html;
    for (const auto &span : block.math) {
        const QString url = QStringLiteral("katemath:/") + span.marker;
        const QString original = QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(url, span.source.toHtmlEscaped());
        const auto rendered = math->request(span.source, span.display);
        if (!rendered || !rendered->error.isEmpty()) {
            html.replace(original, QStringLiteral("<code>%1</code>").arg(span.source.toHtmlEscaped()));
            continue;
        }
        const qreal ex = QFontMetricsF(font).xHeight();
        const qreal scale = qMin(1.0, qMin(width / (rendered->width * ex), 600 / (rendered->height * ex)));
        const QSizeF logical(qMax(1.0, rendered->width * ex * scale), qMax(1.0, rendered->height * ex * scale));
        QByteArray svg = rendered->svg;
        svg.replace("currentColor", palette.color(QPalette::Text).name().toUtf8());
        QSvgRenderer painterSvg(svg);
        if (!painterSvg.isValid()) {
            html.replace(original, QStringLiteral("<code>%1</code>").arg(span.source.toHtmlEscaped()));
            continue;
        }
        // Supersample thin math strokes even on 1x displays; small equals signs
        // and fraction rules must survive QTextDocument's fractional sizing.
        const qreal dpr = qBound(2.0, pixelRatio, 3.0);
        const QSize pixels(int(std::ceil(logical.width() * dpr)), int(std::ceil(logical.height() * dpr)));
        const qint64 bytes = qint64(pixels.width()) * pixels.height() * 4;
        if (bytes + m_imageBytes > 8 * 1024 * 1024) {
            html.replace(original, QStringLiteral("<code>%1</code>").arg(span.source.toHtmlEscaped()));
            continue;
        }
        m_imageBytes += bytes;
        QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent); image.setDevicePixelRatio(dpr);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painterSvg.render(&painter, QRectF(QPointF(0, 0), logical));
        painter.end();
        addResource(QTextDocument::ImageResource, QUrl(url), image);
        html.replace(original, QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\">")
            .arg(url).arg(logical.width()).arg(logical.height()));
    }
    setHtml(html);
    // Set the family without discarding bold/italic or heading size, and give
    // formula objects a native inline alignment instead of a text-only baseline.
    QVector<QTextCursor> fragments;
    for (auto block = begin(); block.isValid(); block = block.next()) for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (!fragment.isValid()) continue;
        QTextCursor cursor(this);
        cursor.setPosition(fragment.position());
        cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
        fragments.append(cursor);
    }
    for (auto cursor : fragments) {
        QTextCharFormat format;
        format.setFontFamilies({font.family()});
        if (cursor.charFormat().isImageFormat()) {
            const auto name = cursor.charFormat().toImageFormat().name();
            if (name.startsWith(QStringLiteral("katemath:/"))) {
                format.setVerticalAlignment(QTextCharFormat::AlignMiddle);
                for (const auto &span : block.math) if (name.endsWith(span.marker)) format.setToolTip(span.source);
            }
        }
        cursor.mergeCharFormat(format);
    }
    setTextWidth(width);
}

QVariant RenderDocument::loadResource(int type, const QUrl &name)
{
    QImage placeholder(150, 26, QImage::Format_ARGB32_Premultiplied);
    placeholder.fill(m_palette.color(QPalette::AlternateBase));
    QPainter painter(&placeholder);
    painter.setPen(m_palette.color(QPalette::Text));
    painter.drawText(placeholder.rect(), Qt::AlignCenter, QStringLiteral("[image unavailable]"));
    painter.end();
    if (type != QTextDocument::ImageResource) return placeholder;
    const auto item = m_media->request(m_base.resolved(name));
    if (!item || item->image.isNull()) return placeholder;
    QImage image = item->image;
    if (image.width() > m_width || image.height() > 640) image = image.scaled(m_width, 640, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (m_imageBytes + image.sizeInBytes() > 8 * 1024 * 1024) return placeholder;
    m_imageBytes += image.sizeInBytes();
    return image;
}
}
