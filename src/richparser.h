#pragma once
#include "markdown.h"

namespace MarkdownLive {
struct SourceRange { int start = 0, end = 0; };
struct RichLeaf {
    QString marker, text, url, language, insertionPrefix;
    SourceRange source;
    enum Kind { Text, Code, Image, Math, Rule } kind = Text;
    bool display = false;
    bool table = false;
};
struct RichRegion {
    SourceRange source;
    int innerStart = 0, innerEnd = 0;
    QString kind;
};
struct RichParse {
    QString html;
    QVector<RichLeaf> leaves;
    QVector<RichRegion> regions;
    bool limited = false;
};
RichParse parseRich(const QString &source);
QString decodeEntity(const QString &source);
}
