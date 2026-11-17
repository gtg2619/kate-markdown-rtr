#include "liveeditor.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Message>
#include <KTextEditor/Plugin>
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QHash>
#include <QTimer>

using MarkdownLive::LiveEditor;

class MarkdownPluginView : public QObject, public KXMLGUIClient
{
    Q_OBJECT
public:
    explicit MarkdownPluginView(KTextEditor::MainWindow *window)
        : QObject(window), m_window(window)
    {
        setComponentName(QStringLiteral("katemarkdownlive"), i18n("Markdown RTR"));
        setXMLFile(QStringLiteral(":/markdownlive/ui.rc"));
        m_toggle = actionCollection()->addAction(QStringLiteral("markdown_live_toggle"));
        m_toggle->setText(i18n("Markdown Live Editing"));
        m_toggle->setIcon(QIcon::fromTheme(QStringLiteral("text-markdown")));
        m_toggle->setCheckable(true);
        KActionCollection::setDefaultShortcut(m_toggle, QKeySequence(QStringLiteral("Ctrl+Shift+M")));
        connect(m_toggle, &QAction::triggered, this, &MarkdownPluginView::toggle);
        m_remoteImages = actionCollection()->addAction(QStringLiteral("markdown_live_remote_images"));
        m_remoteImages->setText(i18n("Load Remote Images in Markdown"));
        m_remoteImages->setCheckable(true);
        connect(m_remoteImages, &QAction::toggled, this, [this](bool enabled) {
            for (auto editor : std::as_const(m_editors)) if (editor) editor->setRemoteImagesEnabled(enabled);
        });
        connect(window, &KTextEditor::MainWindow::viewChanged, this, &MarkdownPluginView::updateAction);
        connect(window, &KTextEditor::MainWindow::viewCreated, this, &MarkdownPluginView::hook);
        for (auto view : window->views()) hook(view);
        if (window->guiFactory()) window->guiFactory()->addClient(this);
        updateAction();
    }

    ~MarkdownPluginView() override
    {
        if (m_window->guiFactory()) m_window->guiFactory()->removeClient(this);
        const auto editors = m_editors.values();
        m_editors.clear();
        for (auto editor : editors) {
            if (!editor) continue;
            auto view = editor->sourceView();
            const bool focused = editor->hasFocus() || editor->isAncestorOf(QApplication::focusWidget());
            delete editor;
            if (focused && view) view->setFocus();
        }
    }

private:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        auto view = qobject_cast<KTextEditor::View *>(object);
        auto editor = m_editors.value(view);
        if (editor && event->type() == QEvent::Resize) editor->setGeometry(view->rect());
        return QObject::eventFilter(object, event);
    }

    void hook(KTextEditor::View *view)
    {
        connect(view, &QObject::destroyed, this, [this, view] { m_editors.remove(view); });
        connect(view->document(), &KTextEditor::Document::documentUrlChanged, this, &MarkdownPluginView::updateAction);
        connect(view->document(), &KTextEditor::Document::highlightingModeChanged, this, &MarkdownPluginView::updateAction);
        // Native search UI lives inside the original view; reveal it for these actions.
        for (const char *name : {"edit_find", "edit_replace", "go_goto_line"}) {
            if (auto action = view->actionCollection()->action(QString::fromLatin1(name)))
                connect(action, &QAction::triggered, this, [this, view, action] {
                    auto editor = m_editors.value(view);
                    if (!editor || editor->isHidden()) return;
                    disable(view);
                    QTimer::singleShot(0, action, &QAction::trigger);
                });
        }
    }

    static bool markdown(KTextEditor::Document *document)
    {
        const auto name = document->url().fileName().toLower();
        return document->highlightingMode().contains(QStringLiteral("markdown"), Qt::CaseInsensitive)
            || document->mimeType() == QStringLiteral("text/markdown")
            || name.endsWith(QStringLiteral(".md")) || name.endsWith(QStringLiteral(".markdown"));
    }

    void updateAction()
    {
        const auto view = m_window->activeView();
        m_toggle->setEnabled(view && markdown(view->document()));
        const auto editor = m_editors.value(view);
        m_toggle->setChecked(editor && !editor->isHidden());
    }

    void disable(KTextEditor::View *view)
    {
        if (const auto editor = m_editors.value(view)) {
            editor->finishEditing();
            editor->hide();
            view->setFocus();
        }
        updateAction();
    }

    void toggle(bool enabled)
    {
        const auto view = m_window->activeView();
        if (!view) return;
        if (!enabled) { disable(view); return; }
        auto editor = m_editors.value(view);
        if (!editor) {
            // A child overlay keeps Kate's real document tab, save actions, split
            // ownership, modified flag and close confirmation fully intact.
            editor = new LiveEditor(view, view);
            editor->setRemoteImagesEnabled(m_remoteImages->isChecked());
            m_editors.insert(view, editor);
            view->installEventFilter(this);
            connect(editor, &LiveEditor::sourceRequested, this, [this, view] { disable(view); });
            connect(editor, &LiveEditor::limitReached, this, [this, view] {
                disable(view);
                auto message = new KTextEditor::Message(
                    i18n("This document exceeds the live rendering limit. Continue editing in Kate's source view."),
                    KTextEditor::Message::Information);
                message->setAutoHide(7000);
                message->setView(view);
                view->document()->postMessage(message);
            });
        }
        editor->setGeometry(view->rect());
        editor->show();
        editor->raise();
        editor->refresh();
        if (editor->blockCount() == 0) { disable(view); return; }
        editor->setFocus();
        updateAction();
    }

    KTextEditor::MainWindow *m_window;
    QAction *m_toggle;
    QAction *m_remoteImages;
    QHash<KTextEditor::View *, QPointer<LiveEditor>> m_editors;
};

class MarkdownPlugin : public KTextEditor::Plugin
{
    Q_OBJECT
public:
    explicit MarkdownPlugin(QObject *parent) : KTextEditor::Plugin(parent) {}
    QObject *createView(KTextEditor::MainWindow *window) override { return new MarkdownPluginView(window); }
};

K_PLUGIN_FACTORY_WITH_JSON(MarkdownPluginFactory, "plugin.json", registerPlugin<MarkdownPlugin>();)
#include "plugin.moc"
