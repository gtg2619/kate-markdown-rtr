#include "liveeditor.h"
#include "richdocument.h"
#include "mathrenderer.h"
#include "mediastore.h"
#include <KActionCollection>
#include <KTextEditor/Editor>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QDropEvent>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextTable>
#include <QToolTip>
namespace MarkdownLive {
namespace {
class KeepScrollPosition
{
public:
    explicit KeepScrollPosition(QScrollBar *bar) : m_bar(bar), m_value(bar->value()) {}
    ~KeepScrollPosition() { m_bar->setValue(m_value); }
private:
    QScrollBar *m_bar;
    int m_value;
};
}
LiveEditor::LiveEditor(KTextEditor::View *view, QWidget *parent)
    : QTextEdit(parent), m_view(view), m_sourceDocument(view->document())
    , m_math(new MathRenderer(this)), m_media(new MediaStore(this))
    , m_repository(std::make_unique<KSyntaxHighlighting::Repository>())
{
    m_rich = new RichDocument(m_math, m_media, this); setDocument(m_rich);
    setObjectName(QStringLiteral("markdownLiveEditor"));
    setAccessibleName(tr("Markdown rich text editor"));
    setFrameShape(QFrame::NoFrame); setAcceptRichText(false); setUndoRedoEnabled(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_refresh.setSingleShot(true); m_refresh.setInterval(0);
    m_resourceRefresh.setSingleShot(true); m_resourceRefresh.setInterval(16);
    connect(&m_refresh, &QTimer::timeout, this, &LiveEditor::refresh);
    const auto resourceReady = [this] { if (!m_resourceRefresh.isActive()) m_resourceRefresh.start(); };
    connect(m_math, &MathRenderer::ready, this, resourceReady);
    connect(m_media, &MediaStore::ready, this, resourceReady);
    connect(&m_resourceRefresh, &QTimer::timeout, this, [this] {
        const int scroll = verticalScrollBar()->value();
        m_loading = true; m_rich->updateResources(); settleLayout(); m_loading = false;
        verticalScrollBar()->setValue(scroll); viewport()->update();
    });
    connect(m_rich, &QTextDocument::contentsChange, this, [this](int position, int removed, int added) {
        if (!m_loading && !m_pending) { m_changePosition = position; m_removed = removed; m_added = added; }
    });
    connect(this, &QTextEdit::textChanged, this, &LiveEditor::changed);
    connect(this, &QTextEdit::cursorPositionChanged, this, &LiveEditor::mirrorCursor);
    connect(this, &QTextEdit::selectionChanged, this, &LiveEditor::mirrorCursor);
    connect(m_sourceDocument, &KTextEditor::Document::textChanged, this, [this] {
        if (m_applying) return;
        m_pending = true; m_caret = -1; setReadOnly(true); m_refresh.start();
    });
    connect(m_sourceDocument, &KTextEditor::Document::readWriteChanged, this, [this] { setReadOnly(!m_sourceDocument->isReadWrite()); });
    connect(m_sourceDocument, &KTextEditor::Document::documentUrlChanged, this, [this] { rebuild(); });
    connect(m_sourceDocument, &QObject::destroyed, this, [this] { m_refresh.stop(); hide(); });
    connect(view, &KTextEditor::View::configChanged, this, &LiveEditor::updateTheme);
    connect(KTextEditor::Editor::instance(), &KTextEditor::Editor::configChanged, this, &LiveEditor::updateTheme);
    updateTheme();
}
LiveEditor::~LiveEditor() = default;
int LiveEditor::blockCount() const { return m_limited ? 0 : m_rich->blockCount(); }
void LiveEditor::setRemoteImagesEnabled(bool enabled) { m_media->setRemoteEnabled(enabled); rebuild(); }
void LiveEditor::updateTheme()
{
    if (!m_view) return;
    using Theme = KSyntaxHighlighting::Theme;
    const auto theme = m_view->theme(); auto colors = m_view->palette();
    colors.setColor(QPalette::Base, theme.editorColor(Theme::BackgroundColor));
    colors.setColor(QPalette::Text, theme.textColor(Theme::Normal));
    colors.setColor(QPalette::Highlight, theme.editorColor(Theme::TextSelection));
    colors.setColor(QPalette::HighlightedText, theme.selectedTextColor(Theme::Normal));
    setPalette(colors);
    QFont font = m_view->configValue(QStringLiteral("font")).value<QFont>();
    if (font.family().isEmpty()) font = KTextEditor::Editor::instance()->font();
    setFont(font); m_rich->setDefaultFont(font);
    setTabStopDistance(QFontMetricsF(font).horizontalAdvance(u' ') * qMax(1, m_sourceDocument->configValue(QStringLiteral("tab-width")).toInt()));
    m_rich->setDefaultStyleSheet(QStringLiteral(
        "body { color: %1; } a { color: %2; } p { margin-top: 0px; margin-bottom: 10px; }"
        "h1 { font-size: 160%; } h2 { font-size: 140%; } h3 { font-size: 120%; }"
        "h4, h5, h6 { font-size: 110%; } pre, code { background-color: %3; }"
        "blockquote { margin-left: 18px; color: %4; } td, th { border: 1px solid %4; padding: 5px; }")
        .arg(colors.color(QPalette::Text).name(), colors.color(QPalette::Link).name(),
            QColor(theme.editorColor(Theme::CurrentLine)).name(), QColor(theme.textColor(Theme::Comment)).name()));
    rebuild();
}
void LiveEditor::rebuild(int position, int anchor)
{
    if (!m_sourceDocument || m_loading) return;
    m_refresh.stop();
    if (position < 0 && textCursor().hasSelection()) anchor = m_rich->sourcePosition(textCursor().anchor());
    if (position < 0) position = m_caret >= 0 ? m_caret : m_rich->sourcePosition(textCursor().position());
    if (anchor < 0) anchor = position;
    const int scroll = verticalScrollBar()->value();
    const bool initial = m_parseCount == 0;
    m_loading = true; m_source = m_sourceDocument->text(); ++m_parseCount;
    m_limited = !m_rich->render(m_source, m_sourceDocument->url().adjusted(QUrl::RemoveFilename), palette(), qMax(40, viewport()->width() - 40));
    if (!m_limited) for (const auto &leaf : m_rich->leaves()) {
        if (leaf.kind != RichLeaf::Code || leaf.language.isEmpty()) continue;
        auto definition = m_repository->definitionForName(leaf.language);
        if (!definition.isValid()) definition = m_repository->definitionForFileName(QStringLiteral("code.") + leaf.language);
        if (!definition.isValid()) continue;
        QTextDocument code; code.setPlainText(leaf.text);
        KSyntaxHighlighting::SyntaxHighlighter highlighter(&code);
        highlighter.setDefinition(definition); highlighter.setTheme(m_view->theme()); highlighter.rehighlight();
        auto target = m_rich->findBlock(m_rich->displayPosition(leaf.source.start));
        for (auto block = code.begin(); block.isValid() && target.isValid(); block = block.next(), target = target.next())
            target.layout()->setFormats(block.layout()->formats());
    }
    m_rich->markContentsDirty(0, m_rich->characterCount());
    settleLayout();
    m_display = m_rich->toRawText(); m_pending = false;
    if (initial) position = anchor = m_rich->sourcePosition(0, true);
    QTextCursor cursor(m_rich);
    cursor.setPosition(m_rich->displayPosition(qBound(0, anchor, int(m_source.size()))));
    cursor.setPosition(m_rich->displayPosition(qBound(0, position, int(m_source.size()))), QTextCursor::KeepAnchor);
    setTextCursor(cursor); m_caret = qBound(0, position, int(m_source.size())); m_caretDisplay = cursor.position();
    setReadOnly(!m_sourceDocument->isReadWrite()); verticalScrollBar()->setValue(scroll); m_loading = false;
    mirrorCursor();
    if (m_limited) Q_EMIT limitReached();
}
void LiveEditor::refresh()
{
    if (m_sourceDocument && (m_pending || m_source != m_sourceDocument->text())) rebuild();
}
void LiveEditor::finishEditing() { refresh(); mirrorCursor(); }
void LiveEditor::reloadResources() { rebuild(); }
void LiveEditor::editAt(int sourceOffset)
{
    refresh(); QTextCursor cursor(m_rich); cursor.setPosition(m_rich->displayPosition(sourceOffset));
    setTextCursor(cursor); m_caret = sourceOffset; m_caretDisplay = cursor.position(); setFocus();
}
void LiveEditor::setTextCursor(const QTextCursor &cursor)
{
    const KeepScrollPosition keep(verticalScrollBar());
    QTextEdit::setTextCursor(cursor);
}
void LiveEditor::selectAll()
{
    const KeepScrollPosition keep(verticalScrollBar());
    QTextEdit::selectAll();
}
void LiveEditor::apply(int start, int end, const QString &inserted)
{
    if (!m_sourceDocument || !m_sourceDocument->isReadWrite() || m_limited) return;
    start = qBound(0, start, int(m_source.size())); end = qBound(start, end, int(m_source.size()));
    m_applying = true;
    {
        KTextEditor::Document::EditingTransaction transaction(m_sourceDocument);
        m_sourceDocument->replaceText({m_sourceDocument->offsetToCursor(start), m_sourceDocument->offsetToCursor(end)}, inserted);
    }
    m_applying = false; m_caret = start + int(inserted.size()); m_pending = true; m_refresh.start();
}
void LiveEditor::replaceSelection(const QString &text)
{
    refresh(); if (isReadOnly()) return;
    const auto cursor = textCursor(); auto range = m_rich->sourceSelection(cursor.selectionStart(), cursor.selectionEnd(), text.isEmpty());
    if (!cursor.hasSelection() && m_caret >= 0 && cursor.position() == m_caretDisplay) range = {m_caret, m_caret};
    const QString inserted = (cursor.hasSelection() ? QString() : m_rich->insertionPrefix(cursor.position())) + text;
    apply(range.start, range.end, m_rich->repairSelection(range, inserted));
    m_caret = range.start + int(inserted.size());
    refresh(); ensureCursorVisible();
}
void LiveEditor::changed()
{
    if (m_loading || m_pending) return;
    const QString display = m_rich->toRawText();
    if (display == m_display) return;
    auto edit = difference(m_display, display);
    if (m_changePosition >= 0) {
        edit = difference(m_display.mid(m_changePosition, m_removed), display.mid(m_changePosition, m_added));
        edit.start += m_changePosition;
    }
    m_changePosition = -1;
    auto range = m_rich->sourceSelection(edit.start, edit.start + edit.removed, edit.inserted.isEmpty());
    if (!edit.removed && m_caret >= 0 && edit.start == m_caretDisplay) range = {m_caret, m_caret};
    QString inserted = edit.inserted; inserted.replace(QChar::ParagraphSeparator, u'\n');
    if (!edit.removed) inserted.prepend(m_rich->insertionPrefix(edit.start));
    apply(range.start, range.end, m_rich->repairSelection(range, inserted));
    m_caret = range.start + int(inserted.size());
    if (m_sourceDocument && m_rich->adoptPlainEdit(m_sourceDocument->text(), edit.start, edit.removed, inserted, range)) {
        m_refresh.stop(); m_pending = false; m_source = m_sourceDocument->text(); m_display = display;
        m_caretDisplay = edit.start + int(inserted.size());
    }
}
void LiveEditor::mirrorCursor()
{
    if (m_loading || m_pending || !m_view || !m_sourceDocument || m_rich->toRawText() != m_display) return;
    const auto cursor = textCursor();
    if (cursor.position() != m_caretDisplay) { m_caret = m_rich->sourcePosition(cursor.position()); m_caretDisplay = cursor.position(); }
    m_view->setCursorPosition(m_sourceDocument->offsetToCursor(qMax(0, m_caret)));
    if (cursor.hasSelection()) {
        const auto range = m_rich->sourceSelection(cursor.selectionStart(), cursor.selectionEnd());
        m_view->setSelection({m_sourceDocument->offsetToCursor(range.start), m_sourceDocument->offsetToCursor(range.end)});
    } else m_view->removeSelection();
}
bool LiveEditor::history(bool redo)
{
    if (!m_view) return false;
    if (auto action = m_view->actionCollection()->action(redo ? QStringLiteral("edit_redo") : QStringLiteral("edit_undo"))) {
        action->trigger(); refresh(); return true;
    }
    return false;
}
bool LiveEditor::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        const auto key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo) || key->matches(QKeySequence::SelectAll)
            || key->matches(QKeySequence::Cut) || key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Paste)) {
            event->accept(); return true;
        }
    }
    return QTextEdit::event(event);
}
void LiveEditor::keyPressEvent(QKeyEvent *event)
{
    refresh();
    if (event->matches(QKeySequence::Undo)) { history(false); return; }
    if (event->matches(QKeySequence::Redo)) { history(true); return; }
    if (event->matches(QKeySequence::SelectAll)) { selectAll(); return; }
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right
        || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down
        || event->key() == Qt::Key_Home || event->key() == Qt::Key_End) {
        const KeepScrollPosition keep(verticalScrollBar());
        QTextEdit::keyPressEvent(event); return;
    }
    if ((event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab
        || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && textCursor().currentTable()) {
        auto cursor = textCursor();
        cursor.movePosition(event->key() == Qt::Key_Backtab ? QTextCursor::PreviousCell : QTextCursor::NextCell);
        setTextCursor(cursor); event->accept(); return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (isReadOnly()) return;
        if (event->modifiers().testFlag(Qt::ControlModifier)) { event->accept(); return; }
        int at = m_caret >= 0 ? m_caret : m_rich->sourcePosition(textCursor().position());
        QString newline = event->modifiers().testFlag(Qt::ShiftModifier) ? QStringLiteral("  \n") : QStringLiteral("\n\n");
        for (const auto &region : m_rich->regions()) if (at >= region.innerStart && at <= region.innerEnd) {
            if (region.kind == "code_block") newline = QStringLiteral("\n");
            if (region.kind == "table") { QTextEdit::keyPressEvent(event); refresh(); return; }
        }
        if (!textCursor().hasSelection() && textCursor().atBlockEnd() && newline != QStringLiteral("\n")) {
            for (const auto &region : m_rich->regions())
                if ((region.kind == "paragraph" || region.kind == "heading") && at >= region.innerStart && at <= region.innerEnd) {
                    m_caret = at = region.source.end; break;
                }
        }
        const int lineStart = m_source.lastIndexOf(u'\n', qMax(0, at - 1)) + 1;
        const auto list = QRegularExpression(QStringLiteral("^(\\s*(?:[-+*]|[0-9]+[.)]) +)(?:\\[[ xX]\\] +)?")).match(m_source.mid(lineStart));
        if (list.hasMatch()) newline = u'\n' + list.captured(1);
        if (!textCursor().hasSelection() && newline == QStringLiteral("\n\n")) {
            QString opening, closing;
            for (const auto &region : m_rich->regions()) {
                if (region.kind != "strong" && region.kind != "emph" && region.kind != "strikethrough" && region.kind != "code" && region.kind != "link") continue;
                if (at <= region.innerStart || at >= region.innerEnd) continue;
                opening += m_source.mid(region.source.start, region.innerStart - region.source.start);
                closing.prepend(m_source.mid(region.innerEnd, region.source.end - region.innerEnd));
            }
            newline = closing + newline + opening;
        }
        replaceSelection(newline); return;
    }
    QTextEdit::keyPressEvent(event); refresh();
}
void LiveEditor::mousePressEvent(QMouseEvent *event)
{
    const KeepScrollPosition keep(verticalScrollBar());
    refresh(); m_caret = -1; m_caretDisplay = -1; QTextEdit::mousePressEvent(event); mirrorCursor();
}
void LiveEditor::mouseReleaseEvent(QMouseEvent *event)
{
    const KeepScrollPosition keep(verticalScrollBar());
    QTextEdit::mouseReleaseEvent(event);
}
void LiveEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    const KeepScrollPosition keep(verticalScrollBar());
    QTextEdit::mouseDoubleClickEvent(event);
}
void LiveEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (viewport()->rect().contains(event->position().toPoint())) {
        const KeepScrollPosition keep(verticalScrollBar());
        QTextEdit::mouseMoveEvent(event);
    } else QTextEdit::mouseMoveEvent(event); // Deliberate selection dragging outside the viewport.
}
void LiveEditor::focusInEvent(QFocusEvent *event)
{
    const KeepScrollPosition keep(verticalScrollBar());
    QTextEdit::focusInEvent(event);
}
void LiveEditor::inputMethodEvent(QInputMethodEvent *event)
{
    refresh(); QTextEdit::inputMethodEvent(event); refresh();
}
void LiveEditor::resizeEvent(QResizeEvent *event)
{
    QTextEdit::resizeEvent(event);
    if (m_rich) { m_loading = true; m_rich->setObjectWidth(qMax(40, viewport()->width() - 40)); settleLayout(); m_loading = false; }
}
void LiveEditor::settleLayout()
{
    const QSize size = m_rich->documentLayout()->documentSize().toSize();
    // QTextEdit can retain a provisional range until its next layout timer.
    // Publish the completed geometry before the first pointer/scroll event.
    verticalScrollBar()->setRange(0, qMax(0, size.height() - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
}
void LiveEditor::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu;
    if (m_view) for (const char *name : {"edit_undo", "edit_redo"})
        if (auto action = m_view->actionCollection()->action(QString::fromLatin1(name))) menu.addAction(action);
    menu.addSeparator();
    menu.addAction(tr("Cut"), this, &QTextEdit::cut)->setEnabled(!isReadOnly() && textCursor().hasSelection());
    menu.addAction(tr("Copy"), this, &QTextEdit::copy)->setEnabled(textCursor().hasSelection());
    menu.addAction(tr("Paste"), this, &QTextEdit::paste)->setEnabled(!isReadOnly());
    menu.addAction(tr("Select All"), this, &LiveEditor::selectAll);
    menu.exec(event->globalPos());
}
bool LiveEditor::canInsertFromMimeData(const QMimeData *data) const
{ return data->hasImage() || data->hasUrls() || data->hasText(); }
void LiveEditor::insertFromMimeData(const QMimeData *data)
{
    refresh(); if (isReadOnly()) return;
    if (!insertMedia(data)) replaceSelection(data->text());
    refresh();
}
QMimeData *LiveEditor::createMimeDataFromSelection() const
{
    auto mime = new QMimeData;
    const auto cursor = textCursor();
    const auto range = m_rich->sourceSelection(cursor.selectionStart(), cursor.selectionEnd());
    mime->setText(m_source.mid(range.start, range.end - range.start));
    return mime;
}
void LiveEditor::dropEvent(QDropEvent *event)
{
    refresh(); if (isReadOnly()) return;
    setTextCursor(cursorForPosition(event->position().toPoint()));
    insertFromMimeData(event->mimeData()); event->setDropAction(Qt::CopyAction); event->accept();
}
bool LiveEditor::insertMedia(const QMimeData *data)
{
    if (!m_sourceDocument || !m_sourceDocument->isReadWrite()) return true;
    auto warn = [this](const QString &message) {
        QToolTip::showText(this->mapToGlobal(QPoint(0, 0)), message, this);
        return true;
    };
    if (data->hasImage()) {
        if (!m_sourceDocument->url().isLocalFile()) return warn(tr("Save this Markdown file before pasting an image."));
        const auto image = qvariant_cast<QImage>(data->imageData());
        if (image.isNull() || qint64(image.width()) * image.height() > 16 * 1024 * 1024)
            return warn(tr("The pasted image is invalid or exceeds 16 megapixels."));
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG") || png.size() > 8 * 1024 * 1024)
            return warn(tr("The pasted image exceeds the 8 MiB file limit."));
        const QFileInfo document(m_sourceDocument->url().toLocalFile());
        const QString folder = document.fileName() + QStringLiteral(".assets");
        const QString name = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".png");
        const QString relative = folder + u'/' + name;
        const QString path = document.dir().filePath(relative);
        if (!QDir(document.absolutePath()).mkpath(folder)) return warn(tr("Cannot create the image directory."));
        if (QFileInfo::exists(path)) {
            QFile existing(path);
            if (!existing.open(QIODevice::ReadOnly) || existing.read(png.size() + 1) != png)
                return warn(tr("An image with this name already exists and has different contents."));
        } else {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size() || !file.commit())
                return warn(tr("Cannot save the pasted image."));
        }
        replaceSelection(QStringLiteral("![image](%1)").arg(QString::fromLatin1(QUrl::toPercentEncoding(relative, "/"))));
        return true;
    }
    if (data->hasUrls()) {
        QStringList links;
        for (const auto &url : data->urls()) {
            const QString suffix = QFileInfo(url.path()).suffix().toLower();
            if (!QStringList{"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp"}.contains(suffix)) return false;
            QString path = url.toString(QUrl::FullyEncoded);
            if (url.isLocalFile() && m_sourceDocument->url().isLocalFile())
                path = QString::fromLatin1(QUrl::toPercentEncoding(QFileInfo(m_sourceDocument->url().toLocalFile()).dir().relativeFilePath(url.toLocalFile()), "/"));
            links.append(QStringLiteral("![image](%1)").arg(path));
        }
        if (!links.isEmpty()) { replaceSelection(links.join(u'\n')); return true; }
    }
    return false;
}

}
