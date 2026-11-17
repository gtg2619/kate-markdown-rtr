#pragma once
#include "markdown.h"
struct cmark_node;
namespace MarkdownLive {
QVector<MathSpan> scanMath(const QString &source, cmark_node *tree);
QString maskMath(const QString &source, const QVector<MathSpan> &math);
}
