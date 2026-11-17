#include "liveeditor.h"
#include "mathrenderer.h"
#include "mediastore.h"
#include "renderdocument.h"

#include <KActionCollection>
#include <KTextEditor/Editor>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QBuffer>
#include <QDir>
#include <QCryptographicHash>
#include <QMimeData>
#include <QSaveFile>
#include <QToolTip>
#include <QImageReader>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextOption>
#include <algorithm>
#include <cmath>
#include <functional>

namespace MarkdownLive {
namespace {
class MarkdownInput final : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<bool(const QMimeData *)> insertMedia;
protected:
    bool canInsertFromMimeData(const QMimeData *data) const override
    { return data->hasImage() || data->hasUrls() || QPlainTextEdit::canInsertFromMimeData(data); }
    void insertFromMimeData(const QMimeData *data) override
    { if (!insertMedia || !insertMedia(data)) QPlainTextEdit::insertFromMimeData(data); }
};

QString inputText(QPlainTextEdit *input)
{
    // toPlainText() normalizes NBSP to a space. Keep all literal characters;
    // only QTextDocument's internal paragraph boundary represents a newline.
    return input->document()->toRawText().replace(QChar::ParagraphSeparator, u'\n');
}


}

LiveEditor::LiveEditor(KTextEditor::View *sourceView, QWidget *parent)
    : QAbstractScrollArea(parent), m_view(sourceView), m_document(sourceView->document())
    , m_input(new MarkdownInput(viewport()))
    , m_repository(std::make_unique<KSyntaxHighlighting::Repository>())
    , m_highlighter(new KSyntaxHighlighting::SyntaxHighlighter(m_input->document()))
    , m_math(new MathRenderer(this)), m_media(new MediaStore(this))
{
    static_cast<MarkdownInput *>(m_input)->insertMedia = [this](const QMimeData *data) { return insertMedia(data); };
    setObjectName(QStringLiteral("markdownLiveEditor"));
    setAccessibleName(tr("Markdown live editor"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setObjectName(QStringLiteral("markdownBlockInput"));
    m_input->setAccessibleName(tr("Markdown source for the current block"));
    m_input->setFrameShape(QFrame::NoFrame);
    m_input->setUndoRedoEnabled(false); // One undo history: KTextEditor owns it.
    m_input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_input->document()->setDocumentMargin(0);
    m_input->installEventFilter(this);
    m_input->hide();
    m_highlighter->setDefinition(m_repository->definitionForName(QStringLiteral("Markdown")));
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(60);
    m_resourcesRefresh.setSingleShot(true);
    m_resourcesRefresh.setInterval(16);
    auto changed = [this] { if (!m_resourcesRefresh.isActive()) m_resourcesRefresh.start(); };
    connect(m_math, &MathRenderer::ready, this, changed);
    connect(m_media, &MediaStore::ready, this, changed);
    connect(&m_resourcesRefresh, &QTimer::timeout, this, [this] { m_cache.clear(); viewport()->update(); });
    connect(&m_refresh, &QTimer::timeout, this, &LiveEditor::rebuild);
    connect(m_input, &QPlainTextEdit::textChanged, this, &LiveEditor::updateInput);
    connect(m_input, &QPlainTextEdit::cursorPositionChanged, this, &LiveEditor::mirrorCursor);
    connect(m_input, &QPlainTextEdit::selectionChanged, this, &LiveEditor::mirrorCursor);
    connect(m_document, &KTextEditor::Document::textChanged, this, [this] {
        if (m_applying) { ++m_changesDuringApply; return; }
        // Another split, formatter, undo or reload owns this revision. Invalidate
        // stale offsets immediately, before accepting another input event.
        m_activeStart = m_activeEnd = m_first = m_last = -1;
        m_input->hide();
        m_input->setEnabled(false);
        if (isVisible()) m_refresh.start();
    });
    connect(m_document, &KTextEditor::Document::readWriteChanged, this, [this] {
        m_input->setReadOnly(!m_document->isReadWrite());
    });
    connect(m_document, &KTextEditor::Document::documentUrlChanged, this, [this] {
        m_cache.clear(); viewport()->update();
    });
    connect(m_document, &QObject::destroyed, this, [this] { m_refresh.stop(); hide(); });
    connect(sourceView, &KTextEditor::View::configChanged, this, &LiveEditor::updateTheme);
    connect(KTextEditor::Editor::instance(), &KTextEditor::Editor::configChanged, this, &LiveEditor::updateTheme);
    updateTheme();
    rebuild();
}

LiveEditor::~LiveEditor() = default;

void LiveEditor::setRemoteImagesEnabled(bool enabled) { m_media->setRemoteEnabled(enabled); }

bool LiveEditor::insertMedia(const QMimeData *data)
{
    if (!m_document || !m_document->isReadWrite()) return true;
    auto warn = [this](const QString &message) {
        QToolTip::showText(m_input->mapToGlobal(QPoint(0, 0)), message, m_input);
        return true;
    };
    if (data->hasImage()) {
        if (!m_document->url().isLocalFile()) return warn(tr("Save this Markdown file before pasting an image."));
        const auto image = qvariant_cast<QImage>(data->imageData());
        if (image.isNull() || qint64(image.width()) * image.height() > 16 * 1024 * 1024)
            return warn(tr("The pasted image is invalid or exceeds 16 megapixels."));
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG") || png.size() > 8 * 1024 * 1024)
            return warn(tr("The pasted image exceeds the 8 MiB file limit."));
        const QFileInfo document(m_document->url().toLocalFile());
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
        m_input->insertPlainText(QStringLiteral("![image](%1)").arg(QString::fromLatin1(QUrl::toPercentEncoding(relative, "/"))));
        return true;
    }
    if (data->hasUrls()) {
        QStringList links;
        for (const auto &url : data->urls()) {
            const QString suffix = QFileInfo(url.path()).suffix().toLower();
            if (!QStringList{"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp"}.contains(suffix)) return false;
            QString path = url.toString(QUrl::FullyEncoded);
            if (url.isLocalFile() && m_document->url().isLocalFile())
                path = QString::fromLatin1(QUrl::toPercentEncoding(QFileInfo(m_document->url().toLocalFile()).dir().relativeFilePath(url.toLocalFile()), "/"));
            links.append(QStringLiteral("![image](%1)").arg(path));
        }
        if (!links.isEmpty()) { m_input->insertPlainText(links.join(u'\n')); return true; }
    }
    return false;
}

void LiveEditor::refresh()
{
    if (!m_document) return;
    if (m_source != m_document->text() || m_blocks.isEmpty()) rebuild();
    // Re-entering preview also reloads local assets whose file metadata changed.
    m_cache.clear();
    viewport()->update();
    m_input->setReadOnly(!m_document->isReadWrite());
}

void LiveEditor::updateTheme()
{
    if (!m_view) return;
    const auto theme = m_view->theme();
    using Theme = KSyntaxHighlighting::Theme;
    auto colors = m_view->palette();
    colors.setColor(QPalette::Base, theme.editorColor(Theme::BackgroundColor));
    colors.setColor(QPalette::Text, theme.textColor(Theme::Normal));
    colors.setColor(QPalette::Highlight, theme.editorColor(Theme::TextSelection));
    colors.setColor(QPalette::HighlightedText, theme.selectedTextColor(Theme::Normal));
    QColor link = theme.textColor(Theme::Information);
    if (!link.isValid() || link == QColor(Qt::transparent)) link = colors.color(QPalette::Link);
    colors.setColor(QPalette::Link, link);
    setPalette(colors);
    viewport()->setPalette(colors);
    viewport()->setAutoFillBackground(true);
    m_input->setPalette(colors);
    QFont font = m_view->configValue(QStringLiteral("font")).value<QFont>();
    if (font.family().isEmpty()) font = KTextEditor::Editor::instance()->font();
    setFont(font);
    m_input->setFont(font);
    m_input->setTabStopDistance(QFontMetricsF(font).horizontalAdvance(u' ')
        * qMax(1, m_document->configValue(QStringLiteral("tab-width")).toInt()));
    m_highlighter->setTheme(theme);
    m_css = QStringLiteral(
        "body { color: %1; } a { color: %2; }"
        "p { margin-top: 0px; margin-bottom: 0px; }"
        "h1 { font-size: 160%; } h2 { font-size: 140%; } h3 { font-size: 120%; }"
        "h4, h5, h6 { font-size: 110%; }"
        "pre, code { background-color: %3; }"
        "blockquote { margin-left: 18px; color: %4; }"
        "table { border-collapse: collapse; } td, th { border: 1px solid %4; padding: 5px; }"
    ).arg(colors.color(QPalette::Text).name(), link.name(),
        QColor(theme.editorColor(Theme::CurrentLine)).name(), QColor(theme.textColor(Theme::Comment)).name());
    m_cache.clear();
    for (int i = 0; i < m_heights.size(); ++i) m_heights[i] = m_blocks[i].blank ? fontMetrics().height() / 2 : fontMetrics().height() * 2;
    sizeInput();
    recalculate();
    viewport()->update();
}

void LiveEditor::rebuild()
{
    m_refresh.stop();
    if (!m_document) return;
    m_source = m_document->text();
    auto parsed = parse(m_source);
    ++m_parseCount;
    m_activeStart = m_activeEnd = m_first = m_last = -1;
    m_input->hide();
    m_input->setEnabled(true);
    m_input->setReadOnly(!m_document->isReadWrite());
    m_blocks = std::move(parsed.blocks);
    m_cache.clear();
    m_heights.clear();
    for (const auto &block : m_blocks) m_heights.append(block.blank ? fontMetrics().height() / 2 : fontMetrics().height() * 2);
    recalculate();
    viewport()->update();
    if (parsed.limited) Q_EMIT limitReached();
}

bool LiveEditor::isActive(int index) const { return m_first >= 0 && index >= m_first && index <= m_last; }

int LiveEditor::indexAt(int offset) const
{
    auto it = std::upper_bound(m_blocks.cbegin(), m_blocks.cend(), offset,
        [](int value, const Block &block) { return value < block.start; });
    return m_blocks.isEmpty() ? -1 : qMax(0, int(it - m_blocks.cbegin()) - 1);
}

int LiveEditor::indexAtY(int y) const
{
    if (m_tops.isEmpty()) return -1;
    const auto it = std::upper_bound(m_tops.cbegin(), m_tops.cend(), y);
    return qBound(0, int(it - m_tops.cbegin()) - 1, int(m_blocks.size()) - 1);
}

QRect LiveEditor::blockRect(int index) const
{
    if (index < 0 || index >= m_tops.size()) return {};
    return QRect(Margin, m_tops[index] - verticalScrollBar()->value(),
        qMax(20, viewport()->width() - 2 * Margin), m_heights[index]);
}

QTextDocument *LiveEditor::render(int index)
{
    if (auto cached = m_cache.object(index)) return cached;
    auto document = new RenderDocument(m_blocks[index], m_document ? m_document->url().adjusted(QUrl::RemoveFilename) : QUrl(),
        font(), palette(), m_css, qMax(20, viewport()->width() - 2 * Margin), devicePixelRatioF(), m_math, m_media);
    const auto language = m_blocks[index].codeLanguage;
    if (!language.isEmpty()) {
        auto definition = m_repository->definitionForName(language);
        if (!definition.isValid()) definition = m_repository->definitionForFileName(QStringLiteral("code.") + language);
        if (!definition.isValid() && language.compare(QStringLiteral("latex"), Qt::CaseInsensitive) == 0)
            definition = m_repository->definitionForName(QStringLiteral("LaTeX"));
        if (definition.isValid()) {
            auto highlighter = new KSyntaxHighlighting::SyntaxHighlighter(document);
            highlighter->setDefinition(definition);
            highlighter->setTheme(m_view->theme());
            highlighter->rehighlight();
        }
    }
    m_cache.insert(index, document);
    return document;
}

void LiveEditor::recalculate()
{
    int top = Margin;
    m_tops.resize(m_heights.size());
    for (int i = 0; i < m_heights.size(); ++i) {
        m_tops[i] = top;
        if (!isActive(i) || i == m_first) top += m_heights[i] + (m_blocks[i].blank ? 0 : 8);
    }
    verticalScrollBar()->setRange(0, qMax(0, top + Margin - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(fontMetrics().lineSpacing() * 3);
    positionInput();
}

void LiveEditor::prepareVisible()
{
    if (m_layout) return;
    m_layout = true;
    int top = verticalScrollBar()->value();
    const int anchor = indexAtY(top);
    const int anchorDelta = anchor < 0 ? 0 : top - m_tops[anchor];
    bool changed = false;
    // Only lay out a viewport plus a small look-ahead. Other heights are estimates.
    int y = anchor < 0 ? Margin : m_tops[anchor];
    for (int i = qMax(0, anchor); i < m_blocks.size(); ++i) {
        if (y > top + viewport()->height() + 200) break;
        if (!isActive(i) && !m_blocks[i].blank) {
            const int height = qMax(fontMetrics().height(), int(std::ceil(render(i)->size().height())));
            if (m_heights[i] != height) { m_heights[i] = height; changed = true; }
        }
        if (!isActive(i) || i == m_first) y += m_heights[i] + (m_blocks[i].blank ? 0 : 8);
    }
    if (changed) {
        recalculate();
        if (anchor >= 0) verticalScrollBar()->setValue(m_tops[anchor] + anchorDelta);
    }
    m_layout = false;
}

void LiveEditor::paintEvent(QPaintEvent *)
{
    prepareVisible();
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const int start = qMax(0, indexAtY(verticalScrollBar()->value()));
    for (int i = start; i < m_blocks.size(); ++i) {
        const auto rect = blockRect(i);
        if (rect.top() > viewport()->height()) break;
        if (isActive(i) || m_blocks[i].blank) continue;
        painter.save();
        painter.translate(rect.topLeft());
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette = palette();
        context.clip = QRectF(0, -rect.top(), rect.width(), viewport()->height());
        render(i)->documentLayout()->draw(&painter, context);
        painter.restore();
    }
}

void LiveEditor::positionInput()
{
    if (m_first < 0) return;
    m_input->setGeometry(blockRect(m_first));
}

void LiveEditor::sizeInput()
{
    if (m_first < 0) return;
    const int limit = qMax(100, viewport()->height() - 40);
    const int natural = int(std::ceil(m_input->document()->size().height() * QFontMetricsF(m_input->font()).lineSpacing())) + 12;
    m_input->setVerticalScrollBarPolicy(natural > limit ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    m_heights[m_first] = qBound(fontMetrics().height() + 12, natural, limit);
}

void LiveEditor::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    m_cache.clear();
    for (int i = 0; i < m_heights.size(); ++i) m_heights[i] = m_blocks[i].blank ? fontMetrics().height() / 2 : fontMetrics().height() * 2;
    if (m_first >= 0) {
        positionInput();
        sizeInput();
    }
    recalculate();
}

void LiveEditor::scrollContentsBy(int, int)
{
    positionInput();
    viewport()->update();
}

void LiveEditor::editAt(int offset)
{
    if (m_refresh.isActive()) rebuild();
    if (m_first >= 0) finishEditing();
    const int index = indexAt(offset);
    if (index >= 0) activateRange(index, index, offset);
}

void LiveEditor::activateRange(int first, int last, int position, int anchor)
{
    if (!m_document || first < 0 || last >= m_blocks.size()) return;
    m_first = first; m_last = last;
    m_activeStart = m_blocks[first].start;
    m_activeEnd = m_blocks[last].end;
    m_activeText = m_source.mid(m_activeStart, m_activeEnd - m_activeStart);
    m_loading = true;
    m_input->setPlainText(m_activeText);
    m_displayText = inputText(m_input);
    m_input->setEnabled(true);
    m_input->setReadOnly(!m_document->isReadWrite());
    QTextCursor cursor(m_input->document());
    cursor.setPosition(qBound(0, (anchor < 0 ? position : anchor) - m_activeStart, int(m_activeText.size())));
    cursor.setPosition(qBound(0, position - m_activeStart, int(m_activeText.size())), QTextCursor::KeepAnchor);
    m_input->setTextCursor(cursor);
    m_loading = false;
    positionInput();
    m_input->show();
    sizeInput();
    recalculate();
    m_input->setFocus();
    mirrorCursor();
    ensureCursorVisible();
    viewport()->update();
}

void LiveEditor::finishEditing()
{
    if (m_first < 0) return;
    const int offset = m_activeStart;
    const int relativeTop = blockRect(m_first).top();
    rebuild();
    const int index = indexAt(offset);
    if (index >= 0) verticalScrollBar()->setValue(m_tops[index] - relativeTop);
    setFocus();
}

void LiveEditor::updateInput()
{
    if (m_loading || m_activeStart < 0 || !m_document) return;
    const QString display = inputText(m_input);
    if (display == m_displayText) return;
    if (!m_document->isReadWrite()) { rebuild(); return; }
    const auto edit = difference(m_displayText, display);
    const QString after = QString(m_activeText).replace(edit.start, edit.removed, edit.inserted);
    const int offset = m_activeStart + edit.start;
    bool accepted = false;
    m_changesDuringApply = 0;
    m_applying = true;
    {
        KTextEditor::Document::EditingTransaction transaction(m_document);
        accepted = m_document->replaceText(
            {m_document->offsetToCursor(offset), m_document->offsetToCursor(offset + edit.removed)}, edit.inserted);
    }
    m_applying = false;
    if (!accepted || m_changesDuringApply > 1) { rebuild(); return; }
    const int delta = int(after.size() - m_activeText.size());
    m_source.replace(offset, edit.removed, edit.inserted);
    m_activeEnd += delta;
    m_blocks[m_last].end += delta;
    for (int i = m_last + 1; i < m_blocks.size(); ++i) {
        m_blocks[i].start += delta; m_blocks[i].end += delta;
        for (auto &math : m_blocks[i].math) { math.start += delta; math.end += delta; }
    }
    m_activeText = after;
    m_displayText = display;
    sizeInput();
    recalculate();
    mirrorCursor();
    ensureCursorVisible();
    viewport()->update();
}

void LiveEditor::mirrorCursor()
{
    if (m_loading || m_activeStart < 0 || !m_view || !m_document) return;
    const auto cursor = m_input->textCursor();
    const auto position = m_document->offsetToCursor(m_activeStart + cursor.position());
    m_view->setCursorPosition(position);
    if (cursor.hasSelection()) {
        const auto anchor = m_document->offsetToCursor(m_activeStart + cursor.anchor());
        m_view->setSelection({qMin(anchor, position), qMax(anchor, position)});
    } else m_view->removeSelection();
}

void LiveEditor::ensureCursorVisible()
{
    if (m_first < 0) return;
    const int y = m_tops[m_first] + m_input->cursorRect().top();
    const int scroll = verticalScrollBar()->value();
    if (y < scroll + 10) verticalScrollBar()->setValue(y - 10);
    else if (y + fontMetrics().height() > scroll + viewport()->height() - 10)
        verticalScrollBar()->setValue(y + fontMetrics().height() - viewport()->height() + 10);
}

int LiveEditor::clickedOffset(int index, const QPoint &point)
{
    if (m_blocks[index].blank) return m_blocks[index].start;
    auto document = render(index);
    int hit = document->documentLayout()->hitTest(point - blockRect(index).topLeft(), Qt::FuzzyHit);
    QTextCursor cursor(document);
    cursor.setPosition(qBound(0, hit, document->characterCount() - 1));
    // Images and formulas represent source ranges, not synthetic visible words.
    if (cursor.charFormat().isImageFormat()) {
        const auto name = cursor.charFormat().toImageFormat().name();
        for (const auto &math : m_blocks[index].math)
            if (name == QStringLiteral("katemath:/") + math.marker) return math.start;
    }
    cursor.select(QTextCursor::WordUnderCursor);
    const auto word = cursor.selectedText();
    const QString raw = m_source.mid(m_blocks[index].start, m_blocks[index].end - m_blocks[index].start);
    // Anchor clicks to the visible word. Markers never become synthetic source edits.
    const QString plain = document->toPlainText();
    int occurrence = 0, at = 0;
    if (!word.isEmpty()) {
        while ((at = plain.indexOf(word, at)) >= 0 && at < cursor.selectionStart()) { ++occurrence; at += int(word.size()); }
        at = -1;
        do { at = raw.indexOf(word, at + 1); } while (at >= 0 && occurrence-- > 0);
        if (at >= 0) return m_blocks[index].start + at + qBound(0, hit - cursor.selectionStart(), int(word.size()));
    }
    return m_blocks[index].start;
}

void LiveEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) { QAbstractScrollArea::mousePressEvent(event); return; }
    if (m_refresh.isActive()) rebuild();
    const int index = indexAtY(int(event->position().y()) + verticalScrollBar()->value());
    if (index < 0) return;
    const int offset = clickedOffset(index, event->position().toPoint());
    const bool selecting = event->modifiers().testFlag(Qt::ShiftModifier) && m_first >= 0;
    const int anchor = selecting ? m_activeStart + m_input->textCursor().anchor() : -1;
    if (m_first >= 0) finishEditing();
    const int target = indexAt(offset);
    if (selecting) activateRange(qMin(target, indexAt(anchor)), qMax(target, indexAt(anchor)), offset, anchor);
    else activateRange(target, target, offset);
    event->accept();
}

bool LiveEditor::history(bool redo)
{
    if (!m_view) return false;
    const int offset = m_activeStart < 0 ? 0 : m_activeStart + m_input->textCursor().position();
    auto action = m_view->actionCollection()->action(redo ? QStringLiteral("edit_redo") : QStringLiteral("edit_undo"));
    if (!action) return false;
    action->trigger();
    if (m_refresh.isActive()) rebuild();
    editAt(qMin(offset, int(m_source.size())));
    return true;
}

void LiveEditor::extendActive(int direction)
{
    const auto cursor = m_input->textCursor();
    const int position = m_activeStart + cursor.position();
    const int anchor = m_activeStart + cursor.anchor();
    activateRange(qMax(0, m_first - (direction < 0)), qMin(int(m_blocks.size()) - 1, m_last + (direction > 0)), position, anchor);
}

bool LiveEditor::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_input && event->type() == QEvent::ContextMenu) {
        const auto context = static_cast<QContextMenuEvent *>(event);
        QMenu menu;
        if (m_view) {
            for (const char *name : {"edit_undo", "edit_redo"})
                if (auto action = m_view->actionCollection()->action(QString::fromLatin1(name))) menu.addAction(action);
        }
        menu.addSeparator();
        menu.addAction(tr("Cut"), m_input, &QPlainTextEdit::cut)->setEnabled(!m_input->isReadOnly());
        menu.addAction(tr("Copy"), m_input, &QPlainTextEdit::copy);
        menu.addAction(tr("Paste"), m_input, &QPlainTextEdit::paste)->setEnabled(!m_input->isReadOnly());
        menu.exec(context->globalPos());
        return true;
    }
    if (object != m_input || (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride))
        return QAbstractScrollArea::eventFilter(object, event);
    auto key = static_cast<QKeyEvent *>(event);
    // Keep standard text editing shortcuts on this surface, including undo in Kate.
    if (event->type() == QEvent::ShortcutOverride) {
        if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)
            || key->matches(QKeySequence::SelectAll) || key->matches(QKeySequence::Cut)
            || key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Paste)
            || key->key() == Qt::Key_Escape) { key->accept(); return true; }
        return false;
    }
    if (key->matches(QKeySequence::Undo)) return history(false);
    if (key->matches(QKeySequence::Redo)) return history(true);
    if (key->matches(QKeySequence::SelectAll)) {
        activateRange(0, int(m_blocks.size()) - 1, int(m_source.size()), 0);
        return true;
    }
    if (key->key() == Qt::Key_Escape) { finishEditing(); return true; }
    if (m_first < 0) return false;
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        // Let Qt complete the input event (and IME) before resolving new blocks.
        QTimer::singleShot(0, this, [this] {
            if (m_first >= 0) editAt(m_activeStart + m_input->textCursor().position());
        });
    }
    const auto cursor = m_input->textCursor();
    const bool atStart = cursor.position() == 0;
    const bool atEnd = cursor.position() == m_activeText.size();
    const bool up = key->key() == Qt::Key_Up && cursor.blockNumber() == 0;
    const bool down = key->key() == Qt::Key_Down && cursor.blockNumber() == m_input->document()->blockCount() - 1;
    const bool navigating = !cursor.hasSelection() && !key->modifiers().testFlag(Qt::ShiftModifier);
    if (navigating && m_first > 0 && ((atStart && key->key() == Qt::Key_Left) || up)) {
        editAt(m_activeStart - 1);
        return true;
    }
    if (navigating && m_last + 1 < m_blocks.size() && ((atEnd && key->key() == Qt::Key_Right) || down)) {
        editAt(m_activeEnd);
        return true;
    }
    if ((atStart && (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Left)) || up) {
        if (m_first > 0) { extendActive(-1); return false; }
    }
    if ((atEnd && (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Right)) || down) {
        if (m_last + 1 < m_blocks.size()) { extendActive(1); return false; }
    }
    return false;
}

bool LiveEditor::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        const auto key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)
            || key->matches(QKeySequence::SelectAll) || key->matches(QKeySequence::Paste)) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void LiveEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Undo)) { history(false); return; }
    if (event->matches(QKeySequence::Redo)) { history(true); return; }
    if (event->key() == Qt::Key_Escape) { Q_EMIT sourceRequested(); return; }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        editAt(m_view && m_document ? int(m_document->cursorToOffset(m_view->cursorPosition())) : 0);
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        if (!m_blocks.isEmpty()) activateRange(0, int(m_blocks.size()) - 1, int(m_source.size()), 0);
        return;
    }
    if (event->matches(QKeySequence::Paste) || event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete
        || (!event->text().isEmpty() && !(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)))) {
        if (!m_document || !m_document->isReadWrite()) { event->accept(); return; }
        editAt(m_view && m_document ? int(m_document->cursorToOffset(m_view->cursorPosition())) : 0);
        if (m_first >= 0) {
            if (event->matches(QKeySequence::Paste)) m_input->paste();
            else if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
                auto cursor = m_input->textCursor();
                if (event->key() == Qt::Key_Backspace) cursor.deletePreviousChar();
                else cursor.deleteChar();
                m_input->setTextCursor(cursor);
            } else m_input->insertPlainText(event->text());
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}
}
