#pragma once

#include "markdown.h"
#include <QAbstractScrollArea>
#include <QCache>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <KTextEditor/Document>
#include <KTextEditor/View>

class QPlainTextEdit;
class QTextDocument;
class QMimeData;
namespace KSyntaxHighlighting { class SyntaxHighlighter; class Repository; }

namespace MarkdownLive {
class MathRenderer;
class MediaStore;
class LiveEditor : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit LiveEditor(KTextEditor::View *sourceView, QWidget *parent = nullptr);
    ~LiveEditor() override;
    void refresh();
    void finishEditing();
    void editAt(int sourceOffset);
    int activeStart() const { return m_activeStart; }
    int activeEnd() const { return m_activeEnd; }
    int blockCount() const { return int(m_blocks.size()); }
    int cachedDocuments() const { return m_cache.size(); }
    int parseCount() const { return m_parseCount; }
    QPlainTextEdit *input() const { return m_input; }
    KTextEditor::View *sourceView() const { return m_view; }
    QRect blockRect(int index) const;
    void setRemoteImagesEnabled(bool enabled);
    QSize minimumSizeHint() const override { return {0, 0}; }
    QSize sizeHint() const override { return {800, 600}; }

Q_SIGNALS:
    void limitReached();

protected:
    bool event(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
    void scrollContentsBy(int, int) override;

private:
    void updateTheme();
    void updateInput();
    void mirrorCursor();
    void rebuild();
    void recalculate();
    void positionInput();
    void prepareVisible();
    void ensureCursorVisible();
    void sizeInput();
    bool insertMedia(const QMimeData *data);
    void activateRange(int first, int last, int position, int anchor = -1);
    void extendActive(int direction);
    int indexAt(int offset) const;
    int indexAtY(int y) const;
    int clickedOffset(int index, const QPoint &point);
    QTextDocument *render(int index);
    bool history(bool redo);
    bool isActive(int index) const;

    QPointer<KTextEditor::View> m_view;
    QPointer<KTextEditor::Document> m_document;
    QPlainTextEdit *m_input;
    std::unique_ptr<KSyntaxHighlighting::Repository> m_repository;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter;
    QVector<Block> m_blocks;
    QVector<int> m_heights, m_tops;
    QCache<int, QTextDocument> m_cache{48};
    QTimer m_refresh;
    QTimer m_resourcesRefresh;
    MathRenderer *m_math;
    MediaStore *m_media;
    QString m_source, m_activeText, m_displayText, m_css;
    int m_activeStart = -1, m_activeEnd = -1;
    int m_first = -1, m_last = -1;
    int m_parseCount = 0;
    int m_changesDuringApply = 0;
    bool m_applying = false, m_loading = false, m_layout = false;
    static constexpr int Margin = 20;
};
}
