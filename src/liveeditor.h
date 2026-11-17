#pragma once
#include <QTextEdit>
#include <QPointer>
#include <QTimer>
#include <KTextEditor/Document>
#include <KTextEditor/View>
#include <memory>
namespace KSyntaxHighlighting { class Repository; }
namespace MarkdownLive {
class MathRenderer;
class MediaStore;
class RichDocument;
class LiveEditor : public QTextEdit
{
    Q_OBJECT
public:
    explicit LiveEditor(KTextEditor::View *, QWidget *parent = nullptr);
    ~LiveEditor() override;
    void refresh();
    void reloadResources();
    void finishEditing();
    void editAt(int sourceOffset); // Position a caret; never reveal source.
    void setTextCursor(const QTextCursor &);
    void selectAll();
    int blockCount() const;
    int parseCount() const { return m_parseCount; }
    KTextEditor::View *sourceView() const { return m_view; }
    void setRemoteImagesEnabled(bool);
    QSize minimumSizeHint() const override { return {0, 0}; }
    QSize sizeHint() const override { return {800, 600}; }
Q_SIGNALS:
    void limitReached();
protected:
    bool event(QEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void focusInEvent(QFocusEvent *) override;
    void inputMethodEvent(QInputMethodEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    bool canInsertFromMimeData(const QMimeData *) const override;
    void insertFromMimeData(const QMimeData *) override;
    QMimeData *createMimeDataFromSelection() const override;
    void dropEvent(QDropEvent *) override;
private:
    void updateTheme();
    void rebuild(int sourcePosition = -1, int sourceAnchor = -1);
    void changed();
    void mirrorCursor();
    void settleLayout();
    bool history(bool redo);
    bool insertMedia(const QMimeData *);
    void replaceSelection(const QString &);
    void apply(int start, int end, const QString &);
    QPointer<KTextEditor::View> m_view;
    QPointer<KTextEditor::Document> m_sourceDocument;
    RichDocument *m_rich = nullptr;
    MathRenderer *m_math;
    MediaStore *m_media;
    std::unique_ptr<KSyntaxHighlighting::Repository> m_repository;
    QTimer m_refresh, m_resourceRefresh;
    QString m_source, m_display;
    int m_parseCount = 0, m_caret = -1, m_caretDisplay = -1;
    int m_changePosition = -1, m_removed = 0, m_added = 0;
    bool m_loading = false, m_applying = false, m_pending = false, m_limited = false;
};
}
