#pragma once

#include <QString>
#include <QVector>

namespace MarkdownLive {
struct MathSpan {
    int start = 0, end = 0;
    QString source;
    QString marker;
    bool display = false;
    bool standalone = false;
};
struct Block {
    int start = 0; // UTF-16 offsets into the original KTextEditor document.
    int end = 0;   // Exclusive; blocks partition ALL source, including invisible definitions.
    QString html;
    bool blank = false;
    QString codeLanguage;
    QVector<MathSpan> math;
};
struct ParseResult {
    QVector<Block> blocks;
    bool limited = false;
};
inline constexpr int MaxSourceSize = 1024 * 1024;
inline constexpr int MaxBlocks = 4000;
ParseResult parse(const QString &source);

struct Edit {
    int start;
    int removed;
    QString inserted;
};
// A minimal UTF-16 splice, with boundaries kept outside surrogate pairs.
Edit difference(const QString &before, const QString &after);
}
